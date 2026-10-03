/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  LayerDevice
//  -----------
//  Device-level part of the simulated device group.  The driver sees an
//  ordinary single-device VkDevice (vkCreateDevice gets physicalDeviceCount 1),
//  so every device mask and device index the application uses for the N
//  simulated devices is checked against the group and rewritten for the one
//  real device (masks -> 1, indices -> 0) before it reaches the driver:
//
//    vkQueueSubmit / vkQueueSubmit2     VkDeviceGroupSubmitInfo, VkSemaphoreSubmitInfo::deviceIndex,
//                                       VkCommandBufferSubmitInfo::deviceMask
//    vkBeginCommandBuffer               VkDeviceGroupCommandBufferBeginInfo
//    vkCmdSetDeviceMask                 deviceMask
//    vkCmdBeginRenderPass[2]/Rendering  VkDeviceGroupRenderPassBeginInfo
//    vkAllocateMemory                   VkMemoryAllocateFlagsInfo::deviceMask
//    vkBindBuffer/ImageMemory2          VkBind*MemoryDeviceGroupInfo
//    vkQueueBindSparse                  VkDeviceGroupBindSparseInfo
//    vkQueuePresentKHR                  VkDeviceGroupPresentInfoKHR
//    vkAcquireNextImage2KHR             VkAcquireNextImageInfoKHR::deviceMask
//    vkGetDeviceGroupPeerMemoryFeatures answered by the layer (one memory: every access works)
//
//  Mapping memory that has an instance on several devices is reported
//  (VUID-vkMapMemory-memory-00683): on a real group such memory cannot be mapped.
//
//  Structures in the application's pNext chains are const and may be pointed
//  to from elsewhere, so they are patched in place for the duration of the
//  call and restored afterwards.

#include <cstdlib>
#include <cstring>
#include <vector>

#include "LayerConfig.hpp"
#include "LayerDispatch.hpp"
#include "LayerLog.hpp"
#include "LayerMemoryModel.hpp"

namespace VkSim
{

namespace
{

// Temporarily overwrites fields of const application structures
class PatchList
{
public:
    template <typename T>
    void Set(const T& Field, T Value)
    {
        static_assert(sizeof(T) <= sizeof(uint64_t), "field too large");
        Entry E{const_cast<T*>(&Field), 0, sizeof(T)};
        std::memcpy(&E.Old, E.pAddr, sizeof(T));
        m_Entries.push_back(E);
        *const_cast<T*>(&Field) = Value;
    }

    // An array of Count copies of Value that lives until the patches are restored
    const uint32_t* MakeArray(uint32_t Count, uint32_t Value)
    {
        m_Arrays.emplace_back(Count, Value);
        return m_Arrays.back().data();
    }

    ~PatchList()
    {
        for (auto It = m_Entries.rbegin(); It != m_Entries.rend(); ++It)
            std::memcpy(It->pAddr, &It->Old, It->Size);
    }

private:
    struct Entry
    {
        void*    pAddr;
        uint64_t Old;
        size_t   Size;
    };
    std::vector<Entry>                 m_Entries;
    std::vector<std::vector<uint32_t>> m_Arrays;
};

template <typename T>
const T* FindInChain(const void* pNext, VkStructureType SType)
{
    for (auto* p = static_cast<const VkBaseInStructure*>(pNext); p != nullptr; p = p->pNext)
        if (p->sType == SType)
            return reinterpret_cast<const T*>(p);
    return nullptr;
}

bool Validating() { return GetConfig().Validate; }

// ---- Memory-instance model: resources, views, render passes ----------------

uint64_t H(const void* Handle) { return reinterpret_cast<uint64_t>(Handle); }

// Records an operation of the command buffer (with its current device mask)
template <typename F>
void Record(VkCommandBuffer CmdBuf, F&& Build)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    std::lock_guard<std::mutex> Lock{D->Mutex};
    if (!D->Model)
        return;
    Op O;
    Build(*D, O);
    if (!O.Accesses.empty() || !O.Secondaries.empty())
        ModelAddOp(*D, CmdBuf, std::move(O));
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateBuffer(VkDevice Device, const VkBufferCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->CreateBuffer(Device, pInfo, pAllocator, pBuffer);
    if (Result == VK_SUCCESS && pInfo != nullptr)
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        ModelOnBufferCreated(*D, *pBuffer, *pInfo);
    }
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyBuffer(VkDevice Device, VkBuffer Buffer, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        ModelOnDestroyed(*D, H(Buffer));
    }
    D->DestroyBuffer(Device, Buffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateImage(VkDevice Device, const VkImageCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->CreateImage(Device, pInfo, pAllocator, pImage);
    if (Result == VK_SUCCESS && pInfo != nullptr)
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        ModelOnImageCreated(*D, *pImage, *pInfo);
    }
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyImage(VkDevice Device, VkImage Image, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        ModelOnDestroyed(*D, H(Image));
    }
    D->DestroyImage(Device, Image, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindBufferMemory(VkDevice Device, VkBuffer Buffer, VkDeviceMemory Memory, VkDeviceSize Offset)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        ModelOnBind(*D, H(Buffer), Memory, nullptr, 0, false);
    }
    return D->BindBufferMemory(Device, Buffer, Memory, Offset);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindImageMemory(VkDevice Device, VkImage Image, VkDeviceMemory Memory, VkDeviceSize Offset)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        ModelOnBind(*D, H(Image), Memory, nullptr, 0, false);
    }
    return D->BindImageMemory(Device, Image, Memory, Offset);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateImageView(VkDevice Device, const VkImageViewCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->CreateImageView(Device, pInfo, pAllocator, pView);
    if (Result == VK_SUCCESS && pInfo != nullptr)
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->Model->Views[H(*pView)] = ViewInfo{H(pInfo->image), pInfo->subresourceRange};
    }
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyImageView(VkDevice Device, VkImageView View, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->Model->Views.erase(H(View));
    }
    D->DestroyImageView(Device, View, pAllocator);
}

template <typename AttachmentDesc>
void StoreRenderPass(DeviceData& D, VkRenderPass RenderPass, uint32_t Count, const AttachmentDesc* pAttachments)
{
    std::vector<AttachmentOps> Ops;
    for (uint32_t i = 0; pAttachments != nullptr && i < Count; ++i)
        Ops.push_back(AttachmentOps{pAttachments[i].loadOp, pAttachments[i].stencilLoadOp});
    std::lock_guard<std::mutex> Lock{D.Mutex};
    D.Model->RenderPasses[H(RenderPass)] = std::move(Ops);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateRenderPass(VkDevice Device, const VkRenderPassCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->CreateRenderPass(Device, pInfo, pAllocator, pRenderPass);
    if (Result == VK_SUCCESS && pInfo != nullptr)
        StoreRenderPass(*D, *pRenderPass, pInfo->attachmentCount, pInfo->pAttachments);
    return Result;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateRenderPass2(VkDevice Device, const VkRenderPassCreateInfo2* pInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->CreateRenderPass2(Device, pInfo, pAllocator, pRenderPass);
    if (Result == VK_SUCCESS && pInfo != nullptr)
        StoreRenderPass(*D, *pRenderPass, pInfo->attachmentCount, pInfo->pAttachments);
    return Result;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateRenderPass2KHR(VkDevice Device, const VkRenderPassCreateInfo2* pInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->CreateRenderPass2KHR(Device, pInfo, pAllocator, pRenderPass);
    if (Result == VK_SUCCESS && pInfo != nullptr)
        StoreRenderPass(*D, *pRenderPass, pInfo->attachmentCount, pInfo->pAttachments);
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyRenderPass(VkDevice Device, VkRenderPass RenderPass, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->Model->RenderPasses.erase(H(RenderPass));
    }
    D->DestroyRenderPass(Device, RenderPass, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateFramebuffer(VkDevice Device, const VkFramebufferCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->CreateFramebuffer(Device, pInfo, pAllocator, pFramebuffer);
    if (Result == VK_SUCCESS && pInfo != nullptr)
    {
        std::vector<uint64_t> Views;
        if ((pInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) == 0)
            for (uint32_t i = 0; pInfo->pAttachments != nullptr && i < pInfo->attachmentCount; ++i)
                Views.push_back(H(pInfo->pAttachments[i]));
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->Model->Framebuffers[H(*pFramebuffer)] = std::move(Views);
    }
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyFramebuffer(VkDevice Device, VkFramebuffer Framebuffer, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->Model->Framebuffers.erase(H(Framebuffer));
    }
    D->DestroyFramebuffer(Device, Framebuffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AllocateCommandBuffers(VkDevice Device, const VkCommandBufferAllocateInfo* pInfo, VkCommandBuffer* pCmdBufs)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->AllocateCommandBuffers(Device, pInfo, pCmdBufs);
    if (Result == VK_SUCCESS && pInfo != nullptr)
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        auto&                       Pool = D->Model->PoolBuffers[H(pInfo->commandPool)];
        Pool.insert(Pool.end(), pCmdBufs, pCmdBufs + pInfo->commandBufferCount);
    }
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyCommandPool(VkDevice Device, VkCommandPool Pool, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        auto                        It = D->Model->PoolBuffers.find(H(Pool));
        if (It != D->Model->PoolBuffers.end())
        {
            for (VkCommandBuffer Cmd : It->second)
            {
                D->Model->Commands.erase(Cmd);
                D->BeginMasks.erase(Cmd);
            }
            D->Model->PoolBuffers.erase(It);
        }
    }
    D->DestroyCommandPool(Device, Pool, pAllocator);
}

// ---- Memory-instance model: commands ---------------------------------------

VKAPI_ATTR void VKAPI_CALL Hook_CmdExecuteCommands(VkCommandBuffer CmdBuf, uint32_t Count, const VkCommandBuffer* pCmdBufs)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdExecuteCommands";
        O.Secondaries.assign(pCmdBufs, pCmdBufs + Count);
    });
    if (DeviceData* D = FindDeviceByKey(CmdBuf))
        D->CmdExecuteCommands(CmdBuf, Count, pCmdBufs);
}

void AddBufferAccess(Op& O, VkBuffer Buffer, VkDeviceSize Offset, VkDeviceSize Size, ACCESS_TYPE Type)
{
    O.Accesses.push_back(Access{H(Buffer), false, BufferRegion(Offset, Size), Type});
}

void AddImageAccess(Op& O, VkImage Image, const Region& R, ACCESS_TYPE Type)
{
    O.Accesses.push_back(Access{H(Image), true, R, Type});
}

// Image region from two blit offsets (in any order)
Region BlitRegion(const VkImageSubresourceLayers& Sub, const VkOffset3D Offsets[2])
{
    VkOffset3D Min{(std::min)(Offsets[0].x, Offsets[1].x), (std::min)(Offsets[0].y, Offsets[1].y), (std::min)(Offsets[0].z, Offsets[1].z)};
    VkExtent3D Extent{static_cast<uint32_t>(std::abs(Offsets[1].x - Offsets[0].x)), static_cast<uint32_t>(std::abs(Offsets[1].y - Offsets[0].y)),
                      static_cast<uint32_t>(std::abs(Offsets[1].z - Offsets[0].z))};
    return ImageRegion(Sub, Min, Extent);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyBuffer(VkCommandBuffer CmdBuf, VkBuffer Src, VkBuffer Dst, uint32_t Count, const VkBufferCopy* pRegions)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyBuffer";
        for (uint32_t i = 0; i < Count; ++i)
        {
            AddBufferAccess(O, Src, pRegions[i].srcOffset, pRegions[i].size, ACCESS_COPY_READ);
            AddBufferAccess(O, Dst, pRegions[i].dstOffset, pRegions[i].size, ACCESS_COPY_WRITE);
        }
    });
    FindDeviceByKey(CmdBuf)->CmdCopyBuffer(CmdBuf, Src, Dst, Count, pRegions);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImage(VkCommandBuffer CmdBuf, VkImage Src, VkImageLayout SrcLayout, VkImage Dst, VkImageLayout DstLayout, uint32_t Count, const VkImageCopy* pRegions)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyImage";
        for (uint32_t i = 0; i < Count; ++i)
        {
            AddImageAccess(O, Src, ImageRegion(pRegions[i].srcSubresource, pRegions[i].srcOffset, pRegions[i].extent), ACCESS_COPY_READ);
            AddImageAccess(O, Dst, ImageRegion(pRegions[i].dstSubresource, pRegions[i].dstOffset, pRegions[i].extent), ACCESS_COPY_WRITE);
        }
    });
    FindDeviceByKey(CmdBuf)->CmdCopyImage(CmdBuf, Src, SrcLayout, Dst, DstLayout, Count, pRegions);
}

// Buffer side of a buffer-image copy: from the offset to the end of the buffer (texel sizes are not modelled)
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyBufferToImage(VkCommandBuffer CmdBuf, VkBuffer Src, VkImage Dst, VkImageLayout Layout, uint32_t Count, const VkBufferImageCopy* pRegions)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyBufferToImage";
        for (uint32_t i = 0; i < Count; ++i)
        {
            AddBufferAccess(O, Src, pRegions[i].bufferOffset, VK_WHOLE_SIZE, ACCESS_COPY_READ);
            AddImageAccess(O, Dst, ImageRegion(pRegions[i].imageSubresource, pRegions[i].imageOffset, pRegions[i].imageExtent), ACCESS_COPY_WRITE);
        }
    });
    FindDeviceByKey(CmdBuf)->CmdCopyBufferToImage(CmdBuf, Src, Dst, Layout, Count, pRegions);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImageToBuffer(VkCommandBuffer CmdBuf, VkImage Src, VkImageLayout Layout, VkBuffer Dst, uint32_t Count, const VkBufferImageCopy* pRegions)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyImageToBuffer";
        for (uint32_t i = 0; i < Count; ++i)
        {
            AddImageAccess(O, Src, ImageRegion(pRegions[i].imageSubresource, pRegions[i].imageOffset, pRegions[i].imageExtent), ACCESS_COPY_READ);
            AddBufferAccess(O, Dst, pRegions[i].bufferOffset, VK_WHOLE_SIZE, ACCESS_COPY_WRITE);
        }
    });
    FindDeviceByKey(CmdBuf)->CmdCopyImageToBuffer(CmdBuf, Src, Layout, Dst, Count, pRegions);
}

void RecordCopyBuffer2(VkCommandBuffer CmdBuf, const VkCopyBufferInfo2* pInfo)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyBuffer2";
        for (uint32_t i = 0; pInfo != nullptr && i < pInfo->regionCount; ++i)
        {
            AddBufferAccess(O, pInfo->srcBuffer, pInfo->pRegions[i].srcOffset, pInfo->pRegions[i].size, ACCESS_COPY_READ);
            AddBufferAccess(O, pInfo->dstBuffer, pInfo->pRegions[i].dstOffset, pInfo->pRegions[i].size, ACCESS_COPY_WRITE);
        }
    });
}

void RecordCopyImage2(VkCommandBuffer CmdBuf, const VkCopyImageInfo2* pInfo)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyImage2";
        for (uint32_t i = 0; pInfo != nullptr && i < pInfo->regionCount; ++i)
        {
            const VkImageCopy2& R = pInfo->pRegions[i];
            AddImageAccess(O, pInfo->srcImage, ImageRegion(R.srcSubresource, R.srcOffset, R.extent), ACCESS_COPY_READ);
            AddImageAccess(O, pInfo->dstImage, ImageRegion(R.dstSubresource, R.dstOffset, R.extent), ACCESS_COPY_WRITE);
        }
    });
}

void RecordCopyBufferToImage2(VkCommandBuffer CmdBuf, const VkCopyBufferToImageInfo2* pInfo)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyBufferToImage2";
        for (uint32_t i = 0; pInfo != nullptr && i < pInfo->regionCount; ++i)
        {
            const VkBufferImageCopy2& R = pInfo->pRegions[i];
            AddBufferAccess(O, pInfo->srcBuffer, R.bufferOffset, VK_WHOLE_SIZE, ACCESS_COPY_READ);
            AddImageAccess(O, pInfo->dstImage, ImageRegion(R.imageSubresource, R.imageOffset, R.imageExtent), ACCESS_COPY_WRITE);
        }
    });
}

void RecordCopyImageToBuffer2(VkCommandBuffer CmdBuf, const VkCopyImageToBufferInfo2* pInfo)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyImageToBuffer2";
        for (uint32_t i = 0; pInfo != nullptr && i < pInfo->regionCount; ++i)
        {
            const VkBufferImageCopy2& R = pInfo->pRegions[i];
            AddImageAccess(O, pInfo->srcImage, ImageRegion(R.imageSubresource, R.imageOffset, R.imageExtent), ACCESS_COPY_READ);
            AddBufferAccess(O, pInfo->dstBuffer, R.bufferOffset, VK_WHOLE_SIZE, ACCESS_COPY_WRITE);
        }
    });
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyBuffer2(VkCommandBuffer CmdBuf, const VkCopyBufferInfo2* pInfo)
{
    RecordCopyBuffer2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyBuffer2(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyBuffer2KHR(VkCommandBuffer CmdBuf, const VkCopyBufferInfo2* pInfo)
{
    RecordCopyBuffer2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyBuffer2KHR(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImage2(VkCommandBuffer CmdBuf, const VkCopyImageInfo2* pInfo)
{
    RecordCopyImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyImage2(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImage2KHR(VkCommandBuffer CmdBuf, const VkCopyImageInfo2* pInfo)
{
    RecordCopyImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyImage2KHR(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyBufferToImage2(VkCommandBuffer CmdBuf, const VkCopyBufferToImageInfo2* pInfo)
{
    RecordCopyBufferToImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyBufferToImage2(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyBufferToImage2KHR(VkCommandBuffer CmdBuf, const VkCopyBufferToImageInfo2* pInfo)
{
    RecordCopyBufferToImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyBufferToImage2KHR(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImageToBuffer2(VkCommandBuffer CmdBuf, const VkCopyImageToBufferInfo2* pInfo)
{
    RecordCopyImageToBuffer2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyImageToBuffer2(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyImageToBuffer2KHR(VkCommandBuffer CmdBuf, const VkCopyImageToBufferInfo2* pInfo)
{
    RecordCopyImageToBuffer2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdCopyImageToBuffer2KHR(CmdBuf, pInfo);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBlitImage(VkCommandBuffer CmdBuf, VkImage Src, VkImageLayout SrcLayout, VkImage Dst, VkImageLayout DstLayout, uint32_t Count, const VkImageBlit* pRegions,
                                             VkFilter Filter)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdBlitImage";
        for (uint32_t i = 0; i < Count; ++i)
        {
            AddImageAccess(O, Src, BlitRegion(pRegions[i].srcSubresource, pRegions[i].srcOffsets), ACCESS_COPY_READ);
            AddImageAccess(O, Dst, BlitRegion(pRegions[i].dstSubresource, pRegions[i].dstOffsets), ACCESS_COPY_WRITE);
        }
    });
    FindDeviceByKey(CmdBuf)->CmdBlitImage(CmdBuf, Src, SrcLayout, Dst, DstLayout, Count, pRegions, Filter);
}

void RecordBlitImage2(VkCommandBuffer CmdBuf, const VkBlitImageInfo2* pInfo)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdBlitImage2";
        for (uint32_t i = 0; pInfo != nullptr && i < pInfo->regionCount; ++i)
        {
            AddImageAccess(O, pInfo->srcImage, BlitRegion(pInfo->pRegions[i].srcSubresource, pInfo->pRegions[i].srcOffsets), ACCESS_COPY_READ);
            AddImageAccess(O, pInfo->dstImage, BlitRegion(pInfo->pRegions[i].dstSubresource, pInfo->pRegions[i].dstOffsets), ACCESS_COPY_WRITE);
        }
    });
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdBlitImage2(VkCommandBuffer CmdBuf, const VkBlitImageInfo2* pInfo)
{
    RecordBlitImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdBlitImage2(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdBlitImage2KHR(VkCommandBuffer CmdBuf, const VkBlitImageInfo2* pInfo)
{
    RecordBlitImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdBlitImage2KHR(CmdBuf, pInfo);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdResolveImage(VkCommandBuffer CmdBuf, VkImage Src, VkImageLayout SrcLayout, VkImage Dst, VkImageLayout DstLayout, uint32_t Count, const VkImageResolve* pRegions)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdResolveImage";
        for (uint32_t i = 0; i < Count; ++i)
        {
            AddImageAccess(O, Src, ImageRegion(pRegions[i].srcSubresource, pRegions[i].srcOffset, pRegions[i].extent), ACCESS_COPY_READ);
            AddImageAccess(O, Dst, ImageRegion(pRegions[i].dstSubresource, pRegions[i].dstOffset, pRegions[i].extent), ACCESS_COPY_WRITE);
        }
    });
    FindDeviceByKey(CmdBuf)->CmdResolveImage(CmdBuf, Src, SrcLayout, Dst, DstLayout, Count, pRegions);
}

void RecordResolveImage2(VkCommandBuffer CmdBuf, const VkResolveImageInfo2* pInfo)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdResolveImage2";
        for (uint32_t i = 0; pInfo != nullptr && i < pInfo->regionCount; ++i)
        {
            const VkImageResolve2& R = pInfo->pRegions[i];
            AddImageAccess(O, pInfo->srcImage, ImageRegion(R.srcSubresource, R.srcOffset, R.extent), ACCESS_COPY_READ);
            AddImageAccess(O, pInfo->dstImage, ImageRegion(R.dstSubresource, R.dstOffset, R.extent), ACCESS_COPY_WRITE);
        }
    });
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdResolveImage2(VkCommandBuffer CmdBuf, const VkResolveImageInfo2* pInfo)
{
    RecordResolveImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdResolveImage2(CmdBuf, pInfo);
}
VKAPI_ATTR void VKAPI_CALL Hook_CmdResolveImage2KHR(VkCommandBuffer CmdBuf, const VkResolveImageInfo2* pInfo)
{
    RecordResolveImage2(CmdBuf, pInfo);
    FindDeviceByKey(CmdBuf)->CmdResolveImage2KHR(CmdBuf, pInfo);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdClearColorImage(VkCommandBuffer CmdBuf, VkImage Image, VkImageLayout Layout, const VkClearColorValue* pColor, uint32_t Count, const VkImageSubresourceRange* pRanges)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdClearColorImage";
        for (uint32_t i = 0; i < Count; ++i)
            AddImageAccess(O, Image, ImageRange(pRanges[i]), ACCESS_COPY_WRITE);
    });
    FindDeviceByKey(CmdBuf)->CmdClearColorImage(CmdBuf, Image, Layout, pColor, Count, pRanges);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdClearDepthStencilImage(VkCommandBuffer CmdBuf, VkImage Image, VkImageLayout Layout, const VkClearDepthStencilValue* pValue, uint32_t Count,
                                                          const VkImageSubresourceRange* pRanges)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdClearDepthStencilImage";
        for (uint32_t i = 0; i < Count; ++i)
            AddImageAccess(O, Image, ImageRange(pRanges[i]), ACCESS_COPY_WRITE);
    });
    FindDeviceByKey(CmdBuf)->CmdClearDepthStencilImage(CmdBuf, Image, Layout, pValue, Count, pRanges);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdFillBuffer(VkCommandBuffer CmdBuf, VkBuffer Dst, VkDeviceSize Offset, VkDeviceSize Size, uint32_t Data)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdFillBuffer";
        AddBufferAccess(O, Dst, Offset, Size, ACCESS_COPY_WRITE);
    });
    FindDeviceByKey(CmdBuf)->CmdFillBuffer(CmdBuf, Dst, Offset, Size, Data);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdUpdateBuffer(VkCommandBuffer CmdBuf, VkBuffer Dst, VkDeviceSize Offset, VkDeviceSize Size, const void* pData)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdUpdateBuffer";
        AddBufferAccess(O, Dst, Offset, Size, ACCESS_COPY_WRITE);
    });
    FindDeviceByKey(CmdBuf)->CmdUpdateBuffer(CmdBuf, Dst, Offset, Size, pData);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdCopyQueryPoolResults(VkCommandBuffer CmdBuf, VkQueryPool Pool, uint32_t First, uint32_t Count, VkBuffer Dst, VkDeviceSize Offset,
                                                        VkDeviceSize Stride, VkQueryResultFlags Flags)
{
    Record(CmdBuf, [&](DeviceData&, Op& O) {
        O.Api = "vkCmdCopyQueryPoolResults";
        AddBufferAccess(O, Dst, Offset, Count > 0 ? Stride * (Count - 1) + 16 : 0, ACCESS_COPY_WRITE);
    });
    FindDeviceByKey(CmdBuf)->CmdCopyQueryPoolResults(CmdBuf, Pool, First, Count, Dst, Offset, Stride, Flags);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBindVertexBuffers(VkCommandBuffer CmdBuf, uint32_t First, uint32_t Count, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        if (CommandRecord* pRec = D->Model ? ModelRecord(*D, CmdBuf) : nullptr)
        {
            if (pRec->VertexBuffers.size() < size_t{First} + Count)
                pRec->VertexBuffers.resize(size_t{First} + Count);
            for (uint32_t i = 0; i < Count; ++i)
                pRec->VertexBuffers[First + i] = {H(pBuffers[i]), pOffsets != nullptr ? pOffsets[i] : 0};
        }
    }
    D->CmdBindVertexBuffers(CmdBuf, First, Count, pBuffers, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBindIndexBuffer(VkCommandBuffer CmdBuf, VkBuffer Buffer, VkDeviceSize Offset, VkIndexType Type)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        if (CommandRecord* pRec = D->Model ? ModelRecord(*D, CmdBuf) : nullptr)
            pRec->IndexBuffer = {H(Buffer), Offset};
    }
    D->CmdBindIndexBuffer(CmdBuf, Buffer, Offset, Type);
}

// A draw reads the bound vertex (and index) buffers, and an indirect argument buffer
void RecordDraw(VkCommandBuffer CmdBuf, const char* Api, bool Indexed, VkBuffer Indirect, VkDeviceSize IndirectOffset)
{
    Record(CmdBuf, [&](DeviceData& D, Op& O) {
        O.Api               = Api;
        CommandRecord* pRec = ModelRecord(D, CmdBuf);
        if (pRec == nullptr)
            return;
        for (const auto& VB : pRec->VertexBuffers)
            if (VB.first != 0)
                O.Accesses.push_back(Access{VB.first, false, BufferRegion(VB.second, VK_WHOLE_SIZE), ACCESS_GENERIC_READ});
        if (Indexed && pRec->IndexBuffer.first != 0)
            O.Accesses.push_back(Access{pRec->IndexBuffer.first, false, BufferRegion(pRec->IndexBuffer.second, VK_WHOLE_SIZE), ACCESS_GENERIC_READ});
        if (Indirect != VK_NULL_HANDLE)
            O.Accesses.push_back(Access{H(Indirect), false, BufferRegion(IndirectOffset, VK_WHOLE_SIZE), ACCESS_GENERIC_READ});
    });
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDraw(VkCommandBuffer CmdBuf, uint32_t Vertices, uint32_t Instances, uint32_t FirstVertex, uint32_t FirstInstance)
{
    RecordDraw(CmdBuf, "vkCmdDraw", false, VK_NULL_HANDLE, 0);
    FindDeviceByKey(CmdBuf)->CmdDraw(CmdBuf, Vertices, Instances, FirstVertex, FirstInstance);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndexed(VkCommandBuffer CmdBuf, uint32_t Indices, uint32_t Instances, uint32_t FirstIndex, int32_t VertexOffset, uint32_t FirstInstance)
{
    RecordDraw(CmdBuf, "vkCmdDrawIndexed", true, VK_NULL_HANDLE, 0);
    FindDeviceByKey(CmdBuf)->CmdDrawIndexed(CmdBuf, Indices, Instances, FirstIndex, VertexOffset, FirstInstance);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndirect(VkCommandBuffer CmdBuf, VkBuffer Buffer, VkDeviceSize Offset, uint32_t Count, uint32_t Stride)
{
    RecordDraw(CmdBuf, "vkCmdDrawIndirect", false, Buffer, Offset);
    FindDeviceByKey(CmdBuf)->CmdDrawIndirect(CmdBuf, Buffer, Offset, Count, Stride);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDrawIndexedIndirect(VkCommandBuffer CmdBuf, VkBuffer Buffer, VkDeviceSize Offset, uint32_t Count, uint32_t Stride)
{
    RecordDraw(CmdBuf, "vkCmdDrawIndexedIndirect", true, Buffer, Offset);
    FindDeviceByKey(CmdBuf)->CmdDrawIndexedIndirect(CmdBuf, Buffer, Offset, Count, Stride);
}

// Attachments of a render pass instance (framebuffer, or imageless begin info)
void RecordRenderPassAttachments(DeviceData& D, VkCommandBuffer CmdBuf, const char* Api, const VkRenderPassBeginInfo* pInfo)
{
    if (pInfo == nullptr)
        return;
    std::lock_guard<std::mutex> Lock{D.Mutex};
    if (!D.Model)
        return;
    auto Pass = D.Model->RenderPasses.find(H(pInfo->renderPass));
    if (Pass == D.Model->RenderPasses.end())
        return;
    std::vector<uint64_t> Views;
    if (const auto* pAttachments = FindInChain<VkRenderPassAttachmentBeginInfo>(pInfo->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO))
    {
        for (uint32_t i = 0; i < pAttachments->attachmentCount; ++i)
            Views.push_back(H(pAttachments->pAttachments[i]));
    }
    else
    {
        auto Fb = D.Model->Framebuffers.find(H(pInfo->framebuffer));
        if (Fb == D.Model->Framebuffers.end())
            return;
        Views = Fb->second;
    }
    std::vector<std::pair<uint64_t, AttachmentOps>> List;
    for (size_t i = 0; i < Views.size() && i < Pass->second.size(); ++i)
        List.emplace_back(Views[i], Pass->second[i]);
    ModelAddAttachments(D, CmdBuf, Api, List, pInfo->renderArea);
}

void RecordRenderingAttachments(DeviceData& D, VkCommandBuffer CmdBuf, const char* Api, const VkRenderingInfo* pInfo)
{
    if (pInfo == nullptr)
        return;
    std::vector<std::pair<uint64_t, AttachmentOps>> List;
    auto Add = [&](const VkRenderingAttachmentInfo* pAttachment) {
        if (pAttachment == nullptr)
            return;
        if (pAttachment->imageView != VK_NULL_HANDLE)
            List.emplace_back(H(pAttachment->imageView), AttachmentOps{pAttachment->loadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE});
        if (pAttachment->resolveImageView != VK_NULL_HANDLE)
            List.emplace_back(H(pAttachment->resolveImageView), AttachmentOps{});
    };
    for (uint32_t i = 0; i < pInfo->colorAttachmentCount; ++i)
        Add(&pInfo->pColorAttachments[i]);
    Add(pInfo->pDepthAttachment);
    Add(pInfo->pStencilAttachment);
    std::lock_guard<std::mutex> Lock{D.Mutex};
    if (D.Model)
        ModelAddAttachments(D, CmdBuf, Api, List, pInfo->renderArea);
}

// ---- Presentation: device 0 owns the presentation engine ------------------

constexpr VkDeviceGroupPresentModeFlagsKHR kGroupPresentModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR | VK_DEVICE_GROUP_PRESENT_MODE_REMOTE_BIT_KHR;

VKAPI_ATTR VkResult VKAPI_CALL Hook_GetDeviceGroupPresentCapabilitiesKHR(VkDevice Device, VkDeviceGroupPresentCapabilitiesKHR* pCaps)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    if (pCaps == nullptr)
        return VK_SUCCESS;
    // Like a linked adapter with the display on its first GPU: device 0 presents, from its own
    // instance (local) or from any other device's instance (remote)
    std::memset(pCaps->presentMask, 0, sizeof(pCaps->presentMask));
    pCaps->presentMask[0] = AllNodesMask(D->NodeCount);
    pCaps->modes          = kGroupPresentModes;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_GetDeviceGroupSurfacePresentModesKHR(VkDevice Device, VkSurfaceKHR Surface, VkDeviceGroupPresentModeFlagsKHR* pModes)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    const VkResult Result = D->GetDeviceGroupSurfacePresentModesKHR(Device, Surface, pModes);
    if (Result == VK_SUCCESS && pModes != nullptr)
        *pModes = kGroupPresentModes;
    return Result;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice Device, const VkSwapchainCreateInfoKHR* pInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    PatchList                        Patch;
    VkDeviceGroupPresentModeFlagsKHR Modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR; // without the structure
    if (pInfo != nullptr)
    {
        if (const auto* pGroup = FindInChain<VkDeviceGroupSwapchainCreateInfoKHR>(pInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR))
        {
            Modes = pGroup->modes;
            if (Validating() && (Modes & ~kGroupPresentModes) != 0)
                ReportValidationError("vkCreateSwapchainKHR: VkDeviceGroupSwapchainCreateInfoKHR::modes 0x%X includes modes the group does not support (0x%X)", Modes, kGroupPresentModes);
            Patch.Set(pGroup->modes, static_cast<VkDeviceGroupPresentModeFlagsKHR>(VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR));
        }
    }
    const VkResult Result = D->CreateSwapchainKHR(Device, pInfo, pAllocator, pSwapchain);
    if (Result == VK_SUCCESS && pSwapchain != nullptr)
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->Model->SwapchainModes[H(*pSwapchain)] = Modes;
    }
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice Device, VkSwapchainKHR Swapchain, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->Model->SwapchainModes.erase(H(Swapchain));
    }
    D->DestroySwapchainKHR(Device, Swapchain, pAllocator);
}


uint32_t GetBeginMask(DeviceData& D, VkCommandBuffer CmdBuf)
{
    std::lock_guard<std::mutex> Lock{D.Mutex};
    auto                        It = D.BeginMasks.find(CmdBuf);
    return It != D.BeginMasks.end() ? It->second : AllNodesMask(D.NodeCount);
}

void CheckDeviceMask(const DeviceData& D, const char* Api, const char* Field, uint32_t Mask)
{
    if (Validating() && !IsValidDeviceMask(Mask, D.NodeCount))
        ReportValidationError("%s: %s 0x%X is not a valid device mask for a group of %u devices", Api, Field, Mask, D.NodeCount);
}

void CheckDeviceIndex(const DeviceData& D, const char* Api, const char* Field, uint32_t Index)
{
    if (Validating() && Index >= D.NodeCount)
        ReportValidationError("%s: %s %u is not a device of the group (%u devices)", Api, Field, Index, D.NodeCount);
}

// ---- Submission ----------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo* pSubmits, VkFence Fence)
{
    DeviceData* D = FindDeviceByKey(Queue);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    PatchList Patch;
    for (uint32_t s = 0; pSubmits != nullptr && s < SubmitCount; ++s)
    {
        const VkSubmitInfo& Submit = pSubmits[s];
        const auto*         pGroup = FindInChain<VkDeviceGroupSubmitInfo>(Submit.pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
        if (pGroup == nullptr)
        {
            // Without the structure command buffers execute on every device
            std::lock_guard<std::mutex> Lock{D->Mutex};
            for (uint32_t i = 0; i < Submit.commandBufferCount; ++i)
                ModelSubmit(*D, Submit.pCommandBuffers[i], AllNodesMask(D->NodeCount));
            continue;
        }
        for (uint32_t i = 0; i < pGroup->commandBufferCount && pGroup->pCommandBufferDeviceMasks != nullptr; ++i)
        {
            const uint32_t Mask = pGroup->pCommandBufferDeviceMasks[i];
            CheckDeviceMask(*D, "vkQueueSubmit", "VkDeviceGroupSubmitInfo::pCommandBufferDeviceMasks[]", Mask);
            if (Validating() && i < Submit.commandBufferCount && (Mask & ~GetBeginMask(*D, Submit.pCommandBuffers[i])) != 0)
                ReportValidationError("vkQueueSubmit: device mask 0x%X of command buffer %u includes devices that were not in its VkDeviceGroupCommandBufferBeginInfo::deviceMask 0x%X",
                                      Mask, i, GetBeginMask(*D, Submit.pCommandBuffers[i]));
        }
        for (uint32_t i = 0; i < pGroup->waitSemaphoreCount && pGroup->pWaitSemaphoreDeviceIndices != nullptr; ++i)
            CheckDeviceIndex(*D, "vkQueueSubmit", "VkDeviceGroupSubmitInfo::pWaitSemaphoreDeviceIndices[]", pGroup->pWaitSemaphoreDeviceIndices[i]);
        for (uint32_t i = 0; i < pGroup->signalSemaphoreCount && pGroup->pSignalSemaphoreDeviceIndices != nullptr; ++i)
            CheckDeviceIndex(*D, "vkQueueSubmit", "VkDeviceGroupSubmitInfo::pSignalSemaphoreDeviceIndices[]", pGroup->pSignalSemaphoreDeviceIndices[i]);

        // Execution on the simulated devices: memory instances
        {
            std::lock_guard<std::mutex> Lock{D->Mutex};
            for (uint32_t i = 0; i < Submit.commandBufferCount; ++i)
                ModelSubmit(*D, Submit.pCommandBuffers[i], pGroup->pCommandBufferDeviceMasks != nullptr && i < pGroup->commandBufferCount ? pGroup->pCommandBufferDeviceMasks[i] : AllNodesMask(D->NodeCount));
        }
        if (pGroup->pCommandBufferDeviceMasks != nullptr)
            Patch.Set(pGroup->pCommandBufferDeviceMasks, Patch.MakeArray(pGroup->commandBufferCount, 1u));
        if (pGroup->pWaitSemaphoreDeviceIndices != nullptr)
            Patch.Set(pGroup->pWaitSemaphoreDeviceIndices, Patch.MakeArray(pGroup->waitSemaphoreCount, 0u));
        if (pGroup->pSignalSemaphoreDeviceIndices != nullptr)
            Patch.Set(pGroup->pSignalSemaphoreDeviceIndices, Patch.MakeArray(pGroup->signalSemaphoreCount, 0u));
    }
    return D->QueueSubmit(Queue, SubmitCount, pSubmits, Fence);
}

VkResult QueueSubmit2Common(PFN_vkQueueSubmit2 pfnNext, DeviceData& D, VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo2* pSubmits, VkFence Fence)
{
    PatchList Patch;
    for (uint32_t s = 0; pSubmits != nullptr && s < SubmitCount; ++s)
    {
        const VkSubmitInfo2& Submit = pSubmits[s];
        for (uint32_t i = 0; i < Submit.waitSemaphoreInfoCount; ++i)
        {
            CheckDeviceIndex(D, "vkQueueSubmit2", "VkSemaphoreSubmitInfo::deviceIndex", Submit.pWaitSemaphoreInfos[i].deviceIndex);
            Patch.Set(Submit.pWaitSemaphoreInfos[i].deviceIndex, 0u);
        }
        for (uint32_t i = 0; i < Submit.signalSemaphoreInfoCount; ++i)
        {
            CheckDeviceIndex(D, "vkQueueSubmit2", "VkSemaphoreSubmitInfo::deviceIndex", Submit.pSignalSemaphoreInfos[i].deviceIndex);
            Patch.Set(Submit.pSignalSemaphoreInfos[i].deviceIndex, 0u);
        }
        for (uint32_t i = 0; i < Submit.commandBufferInfoCount; ++i)
        {
            const VkCommandBufferSubmitInfo& Info = Submit.pCommandBufferInfos[i];
            {
                std::lock_guard<std::mutex> Lock{D.Mutex};
                ModelSubmit(D, Info.commandBuffer, Info.deviceMask != 0 ? Info.deviceMask : AllNodesMask(D.NodeCount));
            }
            if (Info.deviceMask == 0)
                continue; // 0: all devices of the command buffer's begin mask
            CheckDeviceMask(D, "vkQueueSubmit2", "VkCommandBufferSubmitInfo::deviceMask", Info.deviceMask);
            if (Validating() && (Info.deviceMask & ~GetBeginMask(D, Info.commandBuffer)) != 0)
                ReportValidationError("vkQueueSubmit2: device mask 0x%X of a command buffer includes devices that were not in its begin device mask 0x%X",
                                      Info.deviceMask, GetBeginMask(D, Info.commandBuffer));
            Patch.Set(Info.deviceMask, 1u);
        }
    }
    return pfnNext(Queue, SubmitCount, pSubmits, Fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo2* pSubmits, VkFence Fence)
{
    DeviceData* D = FindDeviceByKey(Queue);
    return D != nullptr ? QueueSubmit2Common(D->QueueSubmit2, *D, Queue, SubmitCount, pSubmits, Fence) : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2KHR(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo2* pSubmits, VkFence Fence)
{
    DeviceData* D = FindDeviceByKey(Queue);
    return D != nullptr ? QueueSubmit2Common(D->QueueSubmit2KHR, *D, Queue, SubmitCount, pSubmits, Fence) : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueBindSparse(VkQueue Queue, uint32_t Count, const VkBindSparseInfo* pInfos, VkFence Fence)
{
    DeviceData* D = FindDeviceByKey(Queue);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    PatchList Patch;
    for (uint32_t i = 0; pInfos != nullptr && i < Count; ++i)
    {
        if (const auto* pGroup = FindInChain<VkDeviceGroupBindSparseInfo>(pInfos[i].pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO))
        {
            CheckDeviceIndex(*D, "vkQueueBindSparse", "resourceDeviceIndex", pGroup->resourceDeviceIndex);
            CheckDeviceIndex(*D, "vkQueueBindSparse", "memoryDeviceIndex", pGroup->memoryDeviceIndex);
            Patch.Set(pGroup->resourceDeviceIndex, 0u);
            Patch.Set(pGroup->memoryDeviceIndex, 0u);
        }
    }
    return D->QueueBindSparse(Queue, Count, pInfos, Fence);
}

// ---- Command buffers -----------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Hook_BeginCommandBuffer(VkCommandBuffer CmdBuf, const VkCommandBufferBeginInfo* pBeginInfo)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    PatchList Patch;
    uint32_t  BeginMask = AllNodesMask(D->NodeCount); // without the structure: all devices
    if (pBeginInfo != nullptr)
    {
        if (const auto* pGroup = FindInChain<VkDeviceGroupCommandBufferBeginInfo>(pBeginInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO))
        {
            CheckDeviceMask(*D, "vkBeginCommandBuffer", "VkDeviceGroupCommandBufferBeginInfo::deviceMask", pGroup->deviceMask);
            BeginMask = pGroup->deviceMask;
            Patch.Set(pGroup->deviceMask, 1u);
        }
    }
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->BeginMasks[CmdBuf] = BeginMask;
        if (D->Model)
        {
            CommandRecord& Rec = D->Model->Commands[CmdBuf]; // beginning resets the recording
            Rec                = CommandRecord{};
            Rec.DeviceMask     = BeginMask;
        }
    }
    return D->BeginCommandBuffer(CmdBuf, pBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL Hook_FreeCommandBuffers(VkDevice Device, VkCommandPool Pool, uint32_t Count, const VkCommandBuffer* pCmdBufs)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        for (uint32_t i = 0; pCmdBufs != nullptr && i < Count; ++i)
        {
            D->BeginMasks.erase(pCmdBufs[i]);
            if (D->Model)
                D->Model->Commands.erase(pCmdBufs[i]);
        }
    }
    D->FreeCommandBuffers(Device, Pool, Count, pCmdBufs);
}

void CmdSetDeviceMaskCommon(PFN_vkCmdSetDeviceMask pfnNext, DeviceData& D, VkCommandBuffer CmdBuf, uint32_t Mask)
{
    CheckDeviceMask(D, "vkCmdSetDeviceMask", "deviceMask", Mask);
    const uint32_t BeginMask = GetBeginMask(D, CmdBuf);
    if (Validating() && (Mask & ~BeginMask) != 0)
        ReportValidationError("vkCmdSetDeviceMask: deviceMask 0x%X includes devices that were not in the begin device mask 0x%X", Mask, BeginMask);
    {
        std::lock_guard<std::mutex> Lock{D.Mutex};
        if (D.Model)
            if (CommandRecord* pRec = ModelRecord(D, CmdBuf))
                pRec->DeviceMask = Mask; // the following commands execute on these devices
    }
    pfnNext(CmdBuf, 1u);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdSetDeviceMask(VkCommandBuffer CmdBuf, uint32_t Mask)
{
    if (DeviceData* D = FindDeviceByKey(CmdBuf))
        CmdSetDeviceMaskCommon(D->CmdSetDeviceMask, *D, CmdBuf, Mask);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdSetDeviceMaskKHR(VkCommandBuffer CmdBuf, uint32_t Mask)
{
    if (DeviceData* D = FindDeviceByKey(CmdBuf))
        CmdSetDeviceMaskCommon(D->CmdSetDeviceMaskKHR, *D, CmdBuf, Mask);
}

// VkDeviceGroupRenderPassBeginInfo: one render area per device, or none
void PatchRenderPassGroup(DeviceData& D, const char* Api, const void* pNext, PatchList& Patch)
{
    const auto* pGroup = FindInChain<VkDeviceGroupRenderPassBeginInfo>(pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO);
    if (pGroup == nullptr)
        return;
    CheckDeviceMask(D, Api, "VkDeviceGroupRenderPassBeginInfo::deviceMask", pGroup->deviceMask);
    if (Validating() && pGroup->deviceRenderAreaCount != 0 && pGroup->deviceRenderAreaCount != D.NodeCount)
        ReportValidationError("%s: deviceRenderAreaCount %u must be 0 or the number of devices (%u)", Api, pGroup->deviceRenderAreaCount, D.NodeCount);
    Patch.Set(pGroup->deviceMask, 1u);
    if (pGroup->deviceRenderAreaCount > 1)
        Patch.Set(pGroup->deviceRenderAreaCount, 1u); // the area of device 0
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRenderPass(VkCommandBuffer CmdBuf, const VkRenderPassBeginInfo* pInfo, VkSubpassContents Contents)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    PatchList Patch;
    if (pInfo != nullptr)
        PatchRenderPassGroup(*D, "vkCmdBeginRenderPass", pInfo->pNext, Patch);
    RecordRenderPassAttachments(*D, CmdBuf, "vkCmdBeginRenderPass", pInfo);
    D->CmdBeginRenderPass(CmdBuf, pInfo, Contents);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRenderPass2(VkCommandBuffer CmdBuf, const VkRenderPassBeginInfo* pInfo, const VkSubpassBeginInfo* pSubpass)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    PatchList Patch;
    if (pInfo != nullptr)
        PatchRenderPassGroup(*D, "vkCmdBeginRenderPass2", pInfo->pNext, Patch);
    RecordRenderPassAttachments(*D, CmdBuf, "vkCmdBeginRenderPass2", pInfo);
    D->CmdBeginRenderPass2(CmdBuf, pInfo, pSubpass);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRenderPass2KHR(VkCommandBuffer CmdBuf, const VkRenderPassBeginInfo* pInfo, const VkSubpassBeginInfo* pSubpass)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    PatchList Patch;
    if (pInfo != nullptr)
        PatchRenderPassGroup(*D, "vkCmdBeginRenderPass2KHR", pInfo->pNext, Patch);
    RecordRenderPassAttachments(*D, CmdBuf, "vkCmdBeginRenderPass2KHR", pInfo);
    D->CmdBeginRenderPass2KHR(CmdBuf, pInfo, pSubpass);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRendering(VkCommandBuffer CmdBuf, const VkRenderingInfo* pInfo)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    PatchList Patch;
    if (pInfo != nullptr)
        PatchRenderPassGroup(*D, "vkCmdBeginRendering", pInfo->pNext, Patch);
    RecordRenderingAttachments(*D, CmdBuf, "vkCmdBeginRendering", pInfo);
    D->CmdBeginRendering(CmdBuf, pInfo);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBeginRenderingKHR(VkCommandBuffer CmdBuf, const VkRenderingInfo* pInfo)
{
    DeviceData* D = FindDeviceByKey(CmdBuf);
    if (D == nullptr)
        return;
    PatchList Patch;
    if (pInfo != nullptr)
        PatchRenderPassGroup(*D, "vkCmdBeginRenderingKHR", pInfo->pNext, Patch);
    RecordRenderingAttachments(*D, CmdBuf, "vkCmdBeginRenderingKHR", pInfo);
    D->CmdBeginRenderingKHR(CmdBuf, pInfo);
}

// ---- Memory --------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Hook_AllocateMemory(VkDevice Device, const VkMemoryAllocateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    PatchList Patch;
    uint32_t  DeviceMask = AllNodesMask(D->NodeCount); // without VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT: every device
    if (pInfo != nullptr)
    {
        const auto* pFlags = FindInChain<VkMemoryAllocateFlagsInfo>(pInfo->pNext, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        if (pFlags != nullptr && (pFlags->flags & VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT) != 0)
        {
            CheckDeviceMask(*D, "vkAllocateMemory", "VkMemoryAllocateFlagsInfo::deviceMask", pFlags->deviceMask);
            DeviceMask = pFlags->deviceMask;
            Patch.Set(pFlags->deviceMask, 1u);
        }
    }
    const VkResult Result = D->AllocateMemory(Device, pInfo, pAllocator, pMemory);
    if (Result == VK_SUCCESS && pInfo != nullptr && pMemory != nullptr)
    {
        const bool MultiInstanceHeap = pInfo->memoryTypeIndex < D->MultiInstanceType.size() && D->MultiInstanceType[pInfo->memoryTypeIndex];
        const bool SeveralDevices    = (DeviceMask & (DeviceMask - 1)) != 0;
        std::lock_guard<std::mutex> Lock{D->Mutex};
        // Without subset allocation the device mask is ignored: an instance on every device
        D->MultiInstance[*pMemory] = MultiInstanceHeap && (SeveralDevices || !GetConfig().SubsetAllocation);
        if (D->Model)
            ModelOnAllocate(*D, *pMemory, pInfo->memoryTypeIndex, DeviceMask);
    }
    return Result;
}

VKAPI_ATTR void VKAPI_CALL Hook_FreeMemory(VkDevice Device, VkDeviceMemory Memory, const VkAllocationCallbacks* pAllocator)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{D->Mutex};
        D->MultiInstance.erase(Memory);
        if (D->Model)
            D->Model->Memory.erase(reinterpret_cast<uint64_t>(Memory));
    }
    D->FreeMemory(Device, Memory, pAllocator);
}

void CheckMappable(DeviceData& D, const char* Api, VkDeviceMemory Memory)
{
    if (!Validating())
        return;
    bool MultiInstance = false;
    {
        std::lock_guard<std::mutex> Lock{D.Mutex};
        auto                        It = D.MultiInstance.find(Memory);
        MultiInstance                  = It != D.MultiInstance.end() && It->second;
    }
    if (MultiInstance)
        ReportValidationError("%s: memory %p has an instance on each of %u devices and cannot be mapped (VUID-vkMapMemory-memory-00683); "
                              "allocate it with VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT and a single-device mask",
                              Api, reinterpret_cast<void*>(Memory), D.NodeCount);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_MapMemory(VkDevice Device, VkDeviceMemory Memory, VkDeviceSize Offset, VkDeviceSize Size, VkMemoryMapFlags Flags, void** ppData)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    CheckMappable(*D, "vkMapMemory", Memory);
    return D->MapMemory(Device, Memory, Offset, Size, Flags, ppData);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_MapMemory2(VkDevice Device, const VkMemoryMapInfo* pInfo, void** ppData)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    if (pInfo != nullptr)
        CheckMappable(*D, "vkMapMemory2", pInfo->memory);
    return D->MapMemory2(Device, pInfo, ppData);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_MapMemory2KHR(VkDevice Device, const VkMemoryMapInfo* pInfo, void** ppData)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    if (pInfo != nullptr)
        CheckMappable(*D, "vkMapMemory2KHR", pInfo->memory);
    return D->MapMemory2KHR(Device, pInfo, ppData);
}

void PatchBindIndices(DeviceData& D, const char* Api, uint32_t Count, const uint32_t* pIndices, const uint32_t& CountField, const uint32_t* const& IndicesField,
                      PatchList& Patch)
{
    if (Validating() && Count != 0 && Count != D.NodeCount)
        ReportValidationError("%s: deviceIndexCount %u must be 0 or the number of devices (%u)", Api, Count, D.NodeCount);
    for (uint32_t i = 0; pIndices != nullptr && i < Count; ++i)
        CheckDeviceIndex(D, Api, "pDeviceIndices[]", pIndices[i]);
    // One memory instance: every device binds it
    Patch.Set(CountField, 0u);
    Patch.Set(IndicesField, static_cast<const uint32_t*>(nullptr));
}

VkResult BindBufferMemory2Common(PFN_vkBindBufferMemory2 pfnNext, DeviceData& D, VkDevice Device, uint32_t Count, const VkBindBufferMemoryInfo* pInfos)
{
    PatchList Patch;
    for (uint32_t i = 0; pInfos != nullptr && i < Count; ++i)
    {
        const auto* pGroup = FindInChain<VkBindBufferMemoryDeviceGroupInfo>(pInfos[i].pNext, VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO);
        {
            std::lock_guard<std::mutex> Lock{D.Mutex};
            if (D.Model)
                ModelOnBind(D, reinterpret_cast<uint64_t>(pInfos[i].buffer), pInfos[i].memory, pGroup != nullptr ? pGroup->pDeviceIndices : nullptr,
                            pGroup != nullptr ? pGroup->deviceIndexCount : 0, false);
        }
        if (pGroup != nullptr)
            PatchBindIndices(D, "vkBindBufferMemory2", pGroup->deviceIndexCount, pGroup->pDeviceIndices, pGroup->deviceIndexCount, pGroup->pDeviceIndices, Patch);
    }
    return pfnNext(Device, Count, pInfos);
}

VkResult BindImageMemory2Common(PFN_vkBindImageMemory2 pfnNext, DeviceData& D, VkDevice Device, uint32_t Count, const VkBindImageMemoryInfo* pInfos)
{
    PatchList Patch;
    for (uint32_t i = 0; pInfos != nullptr && i < Count; ++i)
    {
        const auto* pGroup = FindInChain<VkBindImageMemoryDeviceGroupInfo>(pInfos[i].pNext, VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO);
        {
            std::lock_guard<std::mutex> Lock{D.Mutex};
            if (D.Model)
                ModelOnBind(D, reinterpret_cast<uint64_t>(pInfos[i].image), pInfos[i].memory, pGroup != nullptr ? pGroup->pDeviceIndices : nullptr,
                            pGroup != nullptr ? pGroup->deviceIndexCount : 0, pGroup != nullptr && pGroup->splitInstanceBindRegionCount != 0);
        }
        if (pGroup != nullptr)
        {
            PatchBindIndices(D, "vkBindImageMemory2", pGroup->deviceIndexCount, pGroup->pDeviceIndices, pGroup->deviceIndexCount, pGroup->pDeviceIndices, Patch);
            Patch.Set(pGroup->splitInstanceBindRegionCount, 0u);
        }
    }
    return pfnNext(Device, Count, pInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindBufferMemory2(VkDevice Device, uint32_t Count, const VkBindBufferMemoryInfo* pInfos)
{
    DeviceData* D = FindDeviceByKey(Device);
    return D != nullptr ? BindBufferMemory2Common(D->BindBufferMemory2, *D, Device, Count, pInfos) : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindBufferMemory2KHR(VkDevice Device, uint32_t Count, const VkBindBufferMemoryInfo* pInfos)
{
    DeviceData* D = FindDeviceByKey(Device);
    return D != nullptr ? BindBufferMemory2Common(D->BindBufferMemory2KHR, *D, Device, Count, pInfos) : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindImageMemory2(VkDevice Device, uint32_t Count, const VkBindImageMemoryInfo* pInfos)
{
    DeviceData* D = FindDeviceByKey(Device);
    return D != nullptr ? BindImageMemory2Common(D->BindImageMemory2, *D, Device, Count, pInfos) : VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindImageMemory2KHR(VkDevice Device, uint32_t Count, const VkBindImageMemoryInfo* pInfos)
{
    DeviceData* D = FindDeviceByKey(Device);
    return D != nullptr ? BindImageMemory2Common(D->BindImageMemory2KHR, *D, Device, Count, pInfos) : VK_ERROR_DEVICE_LOST;
}

// Peer memory features of the group
VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceGroupPeerMemoryFeatures(VkDevice Device, uint32_t HeapIndex, uint32_t LocalDeviceIndex, uint32_t RemoteDeviceIndex,
                                                                 VkPeerMemoryFeatureFlags* pFeatures)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D != nullptr)
    {
        CheckDeviceIndex(*D, "vkGetDeviceGroupPeerMemoryFeatures", "localDeviceIndex", LocalDeviceIndex);
        CheckDeviceIndex(*D, "vkGetDeviceGroupPeerMemoryFeatures", "remoteDeviceIndex", RemoteDeviceIndex);
        if (Validating() && LocalDeviceIndex == RemoteDeviceIndex)
            ReportValidationError("vkGetDeviceGroupPeerMemoryFeatures: localDeviceIndex and remoteDeviceIndex must differ (both %u)", LocalDeviceIndex);
    }
    if (pFeatures != nullptr)
    {
        // Device-local heaps: what the group's link supports (configurable); other heaps have one
        // instance that every device accesses directly
        const bool DeviceLocal = D != nullptr && HeapIndex < D->DeviceLocalHeap.size() && D->DeviceLocalHeap[HeapIndex];
        *pFeatures = DeviceLocal ? GetConfig().PeerMemoryFeatures :
                                   VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT | VK_PEER_MEMORY_FEATURE_COPY_DST_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
    }
}

// ---- Presentation --------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue Queue, const VkPresentInfoKHR* pInfo)
{
    DeviceData* D = FindDeviceByKey(Queue);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    PatchList Patch;
    if (pInfo != nullptr)
    {
        if (const auto* pGroup = FindInChain<VkDeviceGroupPresentInfoKHR>(pInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR))
        {
            const VkDeviceGroupPresentModeFlagBitsKHR Mode = pGroup->mode;
            if (Validating())
            {
                if ((Mode & kGroupPresentModes) == 0 || (Mode & (Mode - 1)) != 0)
                    ReportValidationError("vkQueuePresentKHR: present mode 0x%X is not one of the group's modes (local, remote)", Mode);
                std::lock_guard<std::mutex> Lock{D->Mutex};
                for (uint32_t i = 0; i < pInfo->swapchainCount && D->Model; ++i)
                {
                    auto It = D->Model->SwapchainModes.find(H(pInfo->pSwapchains[i]));
                    if (It != D->Model->SwapchainModes.end() && (It->second & Mode) == 0)
                        ReportValidationError("vkQueuePresentKHR: swapchain %u was not created for present mode 0x%X (its modes 0x%X)", i, Mode, It->second);
                }
            }
            for (uint32_t i = 0; pGroup->pDeviceMasks != nullptr && i < pGroup->swapchainCount; ++i)
            {
                const uint32_t Mask = pGroup->pDeviceMasks[i];
                CheckDeviceMask(*D, "vkQueuePresentKHR", "VkDeviceGroupPresentInfoKHR::pDeviceMasks[]", Mask);
                // Local and remote presentation name one device; locally only device 0 can present
                if (Validating() && (Mask & (Mask - 1)) != 0)
                    ReportValidationError("vkQueuePresentKHR: device mask 0x%X must name one device in local or remote present mode", Mask);
                else if (Validating() && Mode == VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR && Mask != 1u)
                    ReportValidationError("vkQueuePresentKHR: device mask 0x%X: in local present mode only device 0 has a presentation engine (vkGetDeviceGroupPresentCapabilitiesKHR)", Mask);
            }
            if (pGroup->pDeviceMasks != nullptr)
                Patch.Set(pGroup->pDeviceMasks, Patch.MakeArray(pGroup->swapchainCount, 1u));
            Patch.Set(pGroup->mode, VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR);
        }
    }
    return D->QueuePresentKHR(Queue, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice Device, const VkAcquireNextImageInfoKHR* pInfo, uint32_t* pImageIndex)
{
    DeviceData* D = FindDeviceByKey(Device);
    if (D == nullptr)
        return VK_ERROR_DEVICE_LOST;
    PatchList Patch;
    if (pInfo != nullptr)
    {
        CheckDeviceMask(*D, "vkAcquireNextImage2KHR", "VkAcquireNextImageInfoKHR::deviceMask", pInfo->deviceMask);
        Patch.Set(pInfo->deviceMask, 1u);
    }
    return D->AcquireNextImage2KHR(Device, pInfo, pImageIndex);
}

template <typename PFN>
void Load(PFN& pfn, PFN_vkGetDeviceProcAddr Gdpa, VkDevice Device, const char* Name)
{
    pfn = reinterpret_cast<PFN>(Gdpa(Device, Name));
}

} // namespace

void LoadDeviceFunctions(DeviceData& D)
{
    const PFN_vkGetDeviceProcAddr G   = D.GetDeviceProcAddr;
    const VkDevice                Dev = D.Device;
    Load(D.QueueSubmit, G, Dev, "vkQueueSubmit");
    Load(D.QueueSubmit2, G, Dev, "vkQueueSubmit2");
    Load(D.QueueSubmit2KHR, G, Dev, "vkQueueSubmit2KHR");
    Load(D.QueueBindSparse, G, Dev, "vkQueueBindSparse");
    Load(D.BeginCommandBuffer, G, Dev, "vkBeginCommandBuffer");
    Load(D.FreeCommandBuffers, G, Dev, "vkFreeCommandBuffers");
    Load(D.CmdSetDeviceMask, G, Dev, "vkCmdSetDeviceMask");
    Load(D.CmdSetDeviceMaskKHR, G, Dev, "vkCmdSetDeviceMaskKHR");
    Load(D.CmdBeginRenderPass, G, Dev, "vkCmdBeginRenderPass");
    Load(D.CmdBeginRenderPass2, G, Dev, "vkCmdBeginRenderPass2");
    Load(D.CmdBeginRenderPass2KHR, G, Dev, "vkCmdBeginRenderPass2KHR");
    Load(D.CmdBeginRendering, G, Dev, "vkCmdBeginRendering");
    Load(D.CmdBeginRenderingKHR, G, Dev, "vkCmdBeginRenderingKHR");
    Load(D.AllocateMemory, G, Dev, "vkAllocateMemory");
    Load(D.FreeMemory, G, Dev, "vkFreeMemory");
    Load(D.MapMemory, G, Dev, "vkMapMemory");
    Load(D.MapMemory2, G, Dev, "vkMapMemory2");
    Load(D.MapMemory2KHR, G, Dev, "vkMapMemory2KHR");
    Load(D.BindBufferMemory2, G, Dev, "vkBindBufferMemory2");
    Load(D.BindBufferMemory2KHR, G, Dev, "vkBindBufferMemory2KHR");
    Load(D.BindImageMemory2, G, Dev, "vkBindImageMemory2");
    Load(D.BindImageMemory2KHR, G, Dev, "vkBindImageMemory2KHR");
    Load(D.QueuePresentKHR, G, Dev, "vkQueuePresentKHR");
    Load(D.AcquireNextImage2KHR, G, Dev, "vkAcquireNextImage2KHR");
    Load(D.CreateBuffer, G, Dev, "vkCreateBuffer");
    Load(D.DestroyBuffer, G, Dev, "vkDestroyBuffer");
    Load(D.CreateImage, G, Dev, "vkCreateImage");
    Load(D.DestroyImage, G, Dev, "vkDestroyImage");
    Load(D.BindBufferMemory, G, Dev, "vkBindBufferMemory");
    Load(D.BindImageMemory, G, Dev, "vkBindImageMemory");
    Load(D.CreateImageView, G, Dev, "vkCreateImageView");
    Load(D.DestroyImageView, G, Dev, "vkDestroyImageView");
    Load(D.CreateRenderPass, G, Dev, "vkCreateRenderPass");
    Load(D.CreateRenderPass2, G, Dev, "vkCreateRenderPass2");
    Load(D.CreateRenderPass2KHR, G, Dev, "vkCreateRenderPass2KHR");
    Load(D.DestroyRenderPass, G, Dev, "vkDestroyRenderPass");
    Load(D.CreateFramebuffer, G, Dev, "vkCreateFramebuffer");
    Load(D.DestroyFramebuffer, G, Dev, "vkDestroyFramebuffer");
    Load(D.AllocateCommandBuffers, G, Dev, "vkAllocateCommandBuffers");
    Load(D.DestroyCommandPool, G, Dev, "vkDestroyCommandPool");
    Load(D.CmdExecuteCommands, G, Dev, "vkCmdExecuteCommands");
    Load(D.CmdCopyBuffer, G, Dev, "vkCmdCopyBuffer");
    Load(D.CmdCopyImage, G, Dev, "vkCmdCopyImage");
    Load(D.CmdCopyBufferToImage, G, Dev, "vkCmdCopyBufferToImage");
    Load(D.CmdCopyImageToBuffer, G, Dev, "vkCmdCopyImageToBuffer");
    Load(D.CmdCopyBuffer2, G, Dev, "vkCmdCopyBuffer2");
    Load(D.CmdCopyImage2, G, Dev, "vkCmdCopyImage2");
    Load(D.CmdCopyBufferToImage2, G, Dev, "vkCmdCopyBufferToImage2");
    Load(D.CmdCopyImageToBuffer2, G, Dev, "vkCmdCopyImageToBuffer2");
    Load(D.CmdCopyBuffer2KHR, G, Dev, "vkCmdCopyBuffer2KHR");
    Load(D.CmdCopyImage2KHR, G, Dev, "vkCmdCopyImage2KHR");
    Load(D.CmdCopyBufferToImage2KHR, G, Dev, "vkCmdCopyBufferToImage2KHR");
    Load(D.CmdCopyImageToBuffer2KHR, G, Dev, "vkCmdCopyImageToBuffer2KHR");
    Load(D.CmdBlitImage, G, Dev, "vkCmdBlitImage");
    Load(D.CmdBlitImage2, G, Dev, "vkCmdBlitImage2");
    Load(D.CmdBlitImage2KHR, G, Dev, "vkCmdBlitImage2KHR");
    Load(D.CmdResolveImage, G, Dev, "vkCmdResolveImage");
    Load(D.CmdResolveImage2, G, Dev, "vkCmdResolveImage2");
    Load(D.CmdResolveImage2KHR, G, Dev, "vkCmdResolveImage2KHR");
    Load(D.CmdClearColorImage, G, Dev, "vkCmdClearColorImage");
    Load(D.CmdClearDepthStencilImage, G, Dev, "vkCmdClearDepthStencilImage");
    Load(D.CmdFillBuffer, G, Dev, "vkCmdFillBuffer");
    Load(D.CmdUpdateBuffer, G, Dev, "vkCmdUpdateBuffer");
    Load(D.CmdCopyQueryPoolResults, G, Dev, "vkCmdCopyQueryPoolResults");
    Load(D.CmdBindVertexBuffers, G, Dev, "vkCmdBindVertexBuffers");
    Load(D.CmdBindIndexBuffer, G, Dev, "vkCmdBindIndexBuffer");
    Load(D.CmdDraw, G, Dev, "vkCmdDraw");
    Load(D.CmdDrawIndexed, G, Dev, "vkCmdDrawIndexed");
    Load(D.CmdDrawIndirect, G, Dev, "vkCmdDrawIndirect");
    Load(D.CmdDrawIndexedIndirect, G, Dev, "vkCmdDrawIndexedIndirect");
    Load(D.GetDeviceGroupPresentCapabilitiesKHR, G, Dev, "vkGetDeviceGroupPresentCapabilitiesKHR");
    Load(D.GetDeviceGroupSurfacePresentModesKHR, G, Dev, "vkGetDeviceGroupSurfacePresentModesKHR");
    Load(D.CreateSwapchainKHR, G, Dev, "vkCreateSwapchainKHR");
    Load(D.DestroySwapchainKHR, G, Dev, "vkDestroySwapchainKHR");
}

PFN_vkVoidFunction GetDeviceGroupHook(const DeviceData& D, const char* pName)
{
    // A hook is only returned where the next layer has the function
#define ROUTE_DEV(Name, Field)                                                       \
    if (std::strcmp(pName, Name) == 0)                                               \
        return D.Field != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(&Hook_##Field) : nullptr

    ROUTE_DEV("vkQueueSubmit", QueueSubmit);
    ROUTE_DEV("vkQueueSubmit2", QueueSubmit2);
    ROUTE_DEV("vkQueueSubmit2KHR", QueueSubmit2KHR);
    ROUTE_DEV("vkQueueBindSparse", QueueBindSparse);
    ROUTE_DEV("vkBeginCommandBuffer", BeginCommandBuffer);
    ROUTE_DEV("vkFreeCommandBuffers", FreeCommandBuffers);
    ROUTE_DEV("vkCmdSetDeviceMask", CmdSetDeviceMask);
    ROUTE_DEV("vkCmdSetDeviceMaskKHR", CmdSetDeviceMaskKHR);
    ROUTE_DEV("vkCmdBeginRenderPass", CmdBeginRenderPass);
    ROUTE_DEV("vkCmdBeginRenderPass2", CmdBeginRenderPass2);
    ROUTE_DEV("vkCmdBeginRenderPass2KHR", CmdBeginRenderPass2KHR);
    ROUTE_DEV("vkCmdBeginRendering", CmdBeginRendering);
    ROUTE_DEV("vkCmdBeginRenderingKHR", CmdBeginRenderingKHR);
    ROUTE_DEV("vkAllocateMemory", AllocateMemory);
    ROUTE_DEV("vkFreeMemory", FreeMemory);
    ROUTE_DEV("vkMapMemory", MapMemory);
    ROUTE_DEV("vkMapMemory2", MapMemory2);
    ROUTE_DEV("vkMapMemory2KHR", MapMemory2KHR);
    ROUTE_DEV("vkBindBufferMemory2", BindBufferMemory2);
    ROUTE_DEV("vkBindBufferMemory2KHR", BindBufferMemory2KHR);
    ROUTE_DEV("vkBindImageMemory2", BindImageMemory2);
    ROUTE_DEV("vkBindImageMemory2KHR", BindImageMemory2KHR);
    ROUTE_DEV("vkQueuePresentKHR", QueuePresentKHR);
    ROUTE_DEV("vkAcquireNextImage2KHR", AcquireNextImage2KHR);
    ROUTE_DEV("vkCreateBuffer", CreateBuffer);
    ROUTE_DEV("vkDestroyBuffer", DestroyBuffer);
    ROUTE_DEV("vkCreateImage", CreateImage);
    ROUTE_DEV("vkDestroyImage", DestroyImage);
    ROUTE_DEV("vkBindBufferMemory", BindBufferMemory);
    ROUTE_DEV("vkBindImageMemory", BindImageMemory);
    ROUTE_DEV("vkCreateImageView", CreateImageView);
    ROUTE_DEV("vkDestroyImageView", DestroyImageView);
    ROUTE_DEV("vkCreateRenderPass", CreateRenderPass);
    ROUTE_DEV("vkCreateRenderPass2", CreateRenderPass2);
    ROUTE_DEV("vkCreateRenderPass2KHR", CreateRenderPass2KHR);
    ROUTE_DEV("vkDestroyRenderPass", DestroyRenderPass);
    ROUTE_DEV("vkCreateFramebuffer", CreateFramebuffer);
    ROUTE_DEV("vkDestroyFramebuffer", DestroyFramebuffer);
    ROUTE_DEV("vkAllocateCommandBuffers", AllocateCommandBuffers);
    ROUTE_DEV("vkDestroyCommandPool", DestroyCommandPool);
    ROUTE_DEV("vkCmdExecuteCommands", CmdExecuteCommands);
    ROUTE_DEV("vkCmdCopyBuffer", CmdCopyBuffer);
    ROUTE_DEV("vkCmdCopyImage", CmdCopyImage);
    ROUTE_DEV("vkCmdCopyBufferToImage", CmdCopyBufferToImage);
    ROUTE_DEV("vkCmdCopyImageToBuffer", CmdCopyImageToBuffer);
    ROUTE_DEV("vkCmdCopyBuffer2", CmdCopyBuffer2);
    ROUTE_DEV("vkCmdCopyImage2", CmdCopyImage2);
    ROUTE_DEV("vkCmdCopyBufferToImage2", CmdCopyBufferToImage2);
    ROUTE_DEV("vkCmdCopyImageToBuffer2", CmdCopyImageToBuffer2);
    ROUTE_DEV("vkCmdCopyBuffer2KHR", CmdCopyBuffer2KHR);
    ROUTE_DEV("vkCmdCopyImage2KHR", CmdCopyImage2KHR);
    ROUTE_DEV("vkCmdCopyBufferToImage2KHR", CmdCopyBufferToImage2KHR);
    ROUTE_DEV("vkCmdCopyImageToBuffer2KHR", CmdCopyImageToBuffer2KHR);
    ROUTE_DEV("vkCmdBlitImage", CmdBlitImage);
    ROUTE_DEV("vkCmdBlitImage2", CmdBlitImage2);
    ROUTE_DEV("vkCmdBlitImage2KHR", CmdBlitImage2KHR);
    ROUTE_DEV("vkCmdResolveImage", CmdResolveImage);
    ROUTE_DEV("vkCmdResolveImage2", CmdResolveImage2);
    ROUTE_DEV("vkCmdResolveImage2KHR", CmdResolveImage2KHR);
    ROUTE_DEV("vkCmdClearColorImage", CmdClearColorImage);
    ROUTE_DEV("vkCmdClearDepthStencilImage", CmdClearDepthStencilImage);
    ROUTE_DEV("vkCmdFillBuffer", CmdFillBuffer);
    ROUTE_DEV("vkCmdUpdateBuffer", CmdUpdateBuffer);
    ROUTE_DEV("vkCmdCopyQueryPoolResults", CmdCopyQueryPoolResults);
    ROUTE_DEV("vkCmdBindVertexBuffers", CmdBindVertexBuffers);
    ROUTE_DEV("vkCmdBindIndexBuffer", CmdBindIndexBuffer);
    ROUTE_DEV("vkCmdDraw", CmdDraw);
    ROUTE_DEV("vkCmdDrawIndexed", CmdDrawIndexed);
    ROUTE_DEV("vkCmdDrawIndirect", CmdDrawIndirect);
    ROUTE_DEV("vkCmdDrawIndexedIndirect", CmdDrawIndexedIndirect);
    ROUTE_DEV("vkGetDeviceGroupSurfacePresentModesKHR", GetDeviceGroupSurfacePresentModesKHR);
    ROUTE_DEV("vkCreateSwapchainKHR", CreateSwapchainKHR);
    ROUTE_DEV("vkDestroySwapchainKHR", DestroySwapchainKHR);
    if (std::strcmp(pName, "vkGetDeviceGroupPresentCapabilitiesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceGroupPresentCapabilitiesKHR);
#undef ROUTE_DEV

    if (std::strcmp(pName, "vkGetDeviceGroupPeerMemoryFeatures") == 0 || std::strcmp(pName, "vkGetDeviceGroupPeerMemoryFeaturesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceGroupPeerMemoryFeatures);
    return nullptr;
}

} // namespace VkSim
