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

#include <cstring>
#include <vector>

#include "LayerConfig.hpp"
#include "LayerDispatch.hpp"
#include "LayerLog.hpp"

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
            continue;
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
            D->BeginMasks.erase(pCmdBufs[i]);
    }
    D->FreeCommandBuffers(Device, Pool, Count, pCmdBufs);
}

void CmdSetDeviceMaskCommon(PFN_vkCmdSetDeviceMask pfnNext, DeviceData& D, VkCommandBuffer CmdBuf, uint32_t Mask)
{
    CheckDeviceMask(D, "vkCmdSetDeviceMask", "deviceMask", Mask);
    const uint32_t BeginMask = GetBeginMask(D, CmdBuf);
    if (Validating() && (Mask & ~BeginMask) != 0)
        ReportValidationError("vkCmdSetDeviceMask: deviceMask 0x%X includes devices that were not in the begin device mask 0x%X", Mask, BeginMask);
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
        D->MultiInstance[*pMemory] = MultiInstanceHeap && SeveralDevices;
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
        if (const auto* pGroup = FindInChain<VkBindBufferMemoryDeviceGroupInfo>(pInfos[i].pNext, VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO))
            PatchBindIndices(D, "vkBindBufferMemory2", pGroup->deviceIndexCount, pGroup->pDeviceIndices, pGroup->deviceIndexCount, pGroup->pDeviceIndices, Patch);
    return pfnNext(Device, Count, pInfos);
}

VkResult BindImageMemory2Common(PFN_vkBindImageMemory2 pfnNext, DeviceData& D, VkDevice Device, uint32_t Count, const VkBindImageMemoryInfo* pInfos)
{
    PatchList Patch;
    for (uint32_t i = 0; pInfos != nullptr && i < Count; ++i)
    {
        if (const auto* pGroup = FindInChain<VkBindImageMemoryDeviceGroupInfo>(pInfos[i].pNext, VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO))
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

// All devices of the simulated group use one memory, so every kind of peer access works
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
        *pFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT | VK_PEER_MEMORY_FEATURE_COPY_DST_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
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
            for (uint32_t i = 0; pGroup->pDeviceMasks != nullptr && i < pGroup->swapchainCount; ++i)
                CheckDeviceMask(*D, "vkQueuePresentKHR", "VkDeviceGroupPresentInfoKHR::pDeviceMasks[]", pGroup->pDeviceMasks[i]);
            if (pGroup->pDeviceMasks != nullptr)
                Patch.Set(pGroup->pDeviceMasks, Patch.MakeArray(pGroup->swapchainCount, 1u));
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
#undef ROUTE_DEV

    if (std::strcmp(pName, "vkGetDeviceGroupPeerMemoryFeatures") == 0 || std::strcmp(pName, "vkGetDeviceGroupPeerMemoryFeaturesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceGroupPeerMemoryFeatures);
    return nullptr;
}

} // namespace VkSim
