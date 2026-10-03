/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  LayerMemoryModel
//  ----------------
//  Memory instances of a device group. In a logical device made of several
//  physical devices, memory allocated from a device-local (multi-instance) heap
//  has one instance per device: a copy executed on device 0 writes device 0's
//  instance only, and device 1 still reads its own, old instance. The simulated
//  group runs on one GPU with one memory, so without this model data written on
//  one device would appear on every device.
//
//  The model keeps, per buffer and image bound to multi-instance memory, the
//  regions written and the instances that received each write, in execution
//  order (commands are recorded with the device mask in effect and evaluated at
//  submission with the devices that execute them). A read of a region whose
//  newest data is not in the reading device's instance is reported. Peer access
//  (VkBind*MemoryDeviceGroupInfo pointing a device at another instance) is
//  checked against the peer memory features the layer reports, and access to a
//  device without an instance (allocation device mask) is reported.
//
//  Covered: transfer commands (copy, blit, resolve, clear, fill, update, query
//  results), render pass and dynamic rendering attachments (load = read,
//  rendering = write), vertex and index buffers at draws. Not covered:
//  accesses through descriptors. Resources that shaders can write (storage
//  usage) are therefore not modelled at all, so that unseen writes cannot cause
//  false reports.

#pragma once

#include <cstdint>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace VkSim
{

struct DeviceData;

// A part of a buffer (Offset/Size) or image (mips, layers, rectangle)
struct Region
{
    VkDeviceSize Offset = 0, Size = 0; // buffers
    uint32_t     MipBase = 0, MipCount = 1, LayerBase = 0, LayerCount = 1;
    bool         FullRect = true;  // the whole extent of the mips
    int32_t      X0 = 0, Y0 = 0, Z0 = 0, X1 = 0, Y1 = 0, Z1 = 0; // when !FullRect (single mip)
};

enum ACCESS_TYPE : uint8_t
{
    ACCESS_COPY_READ,
    ACCESS_COPY_WRITE,
    ACCESS_GENERIC_READ,
    ACCESS_GENERIC_WRITE,
};

struct Access
{
    uint64_t    Handle = 0; // VkBuffer or VkImage
    bool        Image  = false;
    Region      R;
    ACCESS_TYPE Type = ACCESS_COPY_READ;
};

struct Op
{
    const char*                  Api        = "";
    uint32_t                     DeviceMask = 0; // in effect when recorded
    std::vector<Access>          Accesses;
    std::vector<VkCommandBuffer> Secondaries; // vkCmdExecuteCommands
};

struct WriteRecord
{
    Region   R;
    uint32_t Instances = 0;
};

struct ResourceState
{
    bool         Image   = false;
    bool         Storage = false; // shaders can write it: not modelled
    VkDeviceSize Size    = 0;
    VkExtent3D   Extent{};
    uint32_t     Mips = 1, Layers = 1;

    bool           Tracked = false; // bound to multi-instance memory
    VkDeviceMemory Memory  = VK_NULL_HANDLE;
    uint32_t       DeviceInstance[32] = {}; // instance each device accesses (peer binding)
    std::vector<WriteRecord> Writes;
};

struct MemoryAllocation
{
    bool     MultiInstance = false;
    uint32_t Instances     = 0; // devices that have an instance
};

struct ViewInfo
{
    uint64_t                Image = 0;
    VkImageSubresourceRange Range{};
};

struct AttachmentOps
{
    VkAttachmentLoadOp Load = VK_ATTACHMENT_LOAD_OP_DONT_CARE, StencilLoad = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
};

struct CommandRecord
{
    uint32_t                                         DeviceMask = 0;
    std::vector<Op>                                  Ops;
    std::vector<std::pair<uint64_t, VkDeviceSize>> VertexBuffers;
    std::pair<uint64_t, VkDeviceSize>              IndexBuffer{0, 0};
};

struct MemoryModel
{
    std::unordered_map<uint64_t, ResourceState>              Resources;
    std::unordered_map<uint64_t, MemoryAllocation>           Memory;
    std::unordered_map<uint64_t, ViewInfo>                   Views;
    std::unordered_map<uint64_t, std::vector<AttachmentOps>> RenderPasses;
    std::unordered_map<uint64_t, std::vector<uint64_t>>      Framebuffers; // attachment views (empty: imageless)
    std::unordered_map<VkCommandBuffer, CommandRecord>       Commands;
    std::unordered_map<uint64_t, std::vector<VkCommandBuffer>> PoolBuffers;
    std::unordered_map<uint64_t, VkDeviceGroupPresentModeFlagsKHR> SwapchainModes;
    std::set<std::tuple<uint64_t, uint32_t, int>>             Reported; // resource, device, kind
};

// All functions expect DeviceData::Mutex to be held

void ModelOnAllocate(DeviceData& D, VkDeviceMemory Memory, uint32_t MemoryType, uint32_t DeviceMask);
void ModelOnBufferCreated(DeviceData& D, VkBuffer Buffer, const VkBufferCreateInfo& Info);
void ModelOnImageCreated(DeviceData& D, VkImage Image, const VkImageCreateInfo& Info);
void ModelOnDestroyed(DeviceData& D, uint64_t Resource); // buffer or image
// DeviceIndices: VkBind*MemoryDeviceGroupInfo (null: each device its own instance)
void ModelOnBind(DeviceData& D, uint64_t Resource, VkDeviceMemory Memory, const uint32_t* pDeviceIndices, uint32_t NumIndices, bool SplitInstance);

Region WholeResource(const ResourceState& Res);
Region BufferRegion(VkDeviceSize Offset, VkDeviceSize Size);
Region ImageRegion(const VkImageSubresourceLayers& Sub, VkOffset3D Offset, VkExtent3D Extent);
Region ImageRange(const VkImageSubresourceRange& Range);

// Recording (into the command buffer's record, with its current device mask)
CommandRecord* ModelRecord(DeviceData& D, VkCommandBuffer CmdBuf);
void           ModelAddOp(DeviceData& D, VkCommandBuffer CmdBuf, Op&& NewOp);
// Attachments of a render pass instance: views with their load ops, the render area
void ModelAddAttachments(DeviceData& D, VkCommandBuffer CmdBuf, const char* Api, const std::vector<std::pair<uint64_t, AttachmentOps>>& Views, const VkRect2D& Area);

// Executes the recorded operations of a command buffer on the devices of SubmitMask
void ModelSubmit(DeviceData& D, VkCommandBuffer CmdBuf, uint32_t SubmitMask);

} // namespace VkSim
