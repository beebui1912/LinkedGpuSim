/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  LayerDispatch
//  -------------
//  Per-instance / per-device dispatch tables.  For every VkInstance the
//  layer intercepts, we store pointers to the NEXT layer's implementation
//  of each Vulkan entry point we forward through (so vkGetInstanceProcAddr
//  can return them and our unwrap-trampolines can call them).

#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "LayerMemoryModel.hpp"

namespace VkSim
{

struct InstanceData
{
    VkInstance                                  Instance                             = VK_NULL_HANDLE;

    // The physical device the simulated group is built on (chosen by LUID, else
    // the first one), and the wrappers that stand for its other nodes. Created
    // once per instance so that every enumeration returns the same handles.
    bool                          HostResolved = false;
    VkPhysicalDevice              Host         = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> NodeWrappers;

    // Base chain
    PFN_vkGetInstanceProcAddr                   GetInstanceProcAddr                  = nullptr;
    PFN_vkDestroyInstance                       DestroyInstance                      = nullptr;
    PFN_vkEnumeratePhysicalDevices              EnumeratePhysicalDevices             = nullptr;
    PFN_vkEnumeratePhysicalDeviceGroups         EnumeratePhysicalDeviceGroups        = nullptr;
    PFN_vkEnumeratePhysicalDeviceGroupsKHR      EnumeratePhysicalDeviceGroupsKHR     = nullptr;
    PFN_vkCreateDevice                          CreateDevice                         = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties    EnumerateDeviceExtensionProperties   = nullptr;
    PFN_vkEnumerateDeviceLayerProperties        EnumerateDeviceLayerProperties       = nullptr;

    // vkGetPhysicalDevice* family
    PFN_vkGetPhysicalDeviceProperties           GetPhysicalDeviceProperties          = nullptr;
    PFN_vkGetPhysicalDeviceProperties2          GetPhysicalDeviceProperties2         = nullptr;
    PFN_vkGetPhysicalDeviceProperties2KHR       GetPhysicalDeviceProperties2KHR      = nullptr;
    PFN_vkGetPhysicalDeviceFeatures             GetPhysicalDeviceFeatures            = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2            GetPhysicalDeviceFeatures2           = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2KHR         GetPhysicalDeviceFeatures2KHR        = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties     GetPhysicalDeviceMemoryProperties    = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties2    GetPhysicalDeviceMemoryProperties2   = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties2KHR GetPhysicalDeviceMemoryProperties2KHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties      GetPhysicalDeviceQueueFamilyProperties       = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2     GetPhysicalDeviceQueueFamilyProperties2      = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR  GetPhysicalDeviceQueueFamilyProperties2KHR   = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties           GetPhysicalDeviceFormatProperties            = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties2          GetPhysicalDeviceFormatProperties2           = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties2KHR       GetPhysicalDeviceFormatProperties2KHR        = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties      GetPhysicalDeviceImageFormatProperties       = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2     GetPhysicalDeviceImageFormatProperties2      = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2KHR  GetPhysicalDeviceImageFormatProperties2KHR   = nullptr;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties      GetPhysicalDeviceSparseImageFormatProperties   = nullptr;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2     GetPhysicalDeviceSparseImageFormatProperties2  = nullptr;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR  GetPhysicalDeviceSparseImageFormatProperties2KHR = nullptr;
    PFN_vkGetPhysicalDeviceExternalBufferProperties      GetPhysicalDeviceExternalBufferProperties    = nullptr;
    PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR   GetPhysicalDeviceExternalBufferPropertiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceExternalFenceProperties       GetPhysicalDeviceExternalFenceProperties     = nullptr;
    PFN_vkGetPhysicalDeviceExternalFencePropertiesKHR    GetPhysicalDeviceExternalFencePropertiesKHR  = nullptr;
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties      GetPhysicalDeviceExternalSemaphoreProperties     = nullptr;
    PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR   GetPhysicalDeviceExternalSemaphorePropertiesKHR  = nullptr;

    // VK_KHR_surface (needed by GUI apps like Tutorial31)
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR      GetPhysicalDeviceSurfaceSupportKHR      = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      GetPhysicalDeviceSurfaceFormatsKHR      = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
#ifdef VK_USE_PLATFORM_WIN32_KHR
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR GetPhysicalDeviceWin32PresentationSupportKHR = nullptr;
#endif
};

// A logical device. Found from any of its dispatchable handles (device, queue,
// command buffer) by the loader dispatch key they share.
struct DeviceData
{
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice     DestroyDevice     = nullptr;
    VkDevice                Device            = VK_NULL_HANDLE;

    // Devices of the simulated group the application created the device with;
    // 1 for an ordinary device (all calls pass through unchanged)
    uint32_t NodeCount = 1;

    // Per memory type: whether its heap is device-local, i.e. has one instance
    // per device of the group (VK_MEMORY_HEAP_MULTI_INSTANCE_BIT)
    std::vector<bool> MultiInstanceType;

    // Next-layer entry points of the intercepted device functions
    PFN_vkQueueSubmit                      QueueSubmit                      = nullptr;
    PFN_vkQueueSubmit2                     QueueSubmit2                     = nullptr;
    PFN_vkQueueSubmit2                     QueueSubmit2KHR                  = nullptr;
    PFN_vkQueueBindSparse                  QueueBindSparse                  = nullptr;
    PFN_vkBeginCommandBuffer               BeginCommandBuffer               = nullptr;
    PFN_vkFreeCommandBuffers               FreeCommandBuffers               = nullptr;
    PFN_vkCmdSetDeviceMask                 CmdSetDeviceMask                 = nullptr;
    PFN_vkCmdSetDeviceMask                 CmdSetDeviceMaskKHR              = nullptr;
    PFN_vkCmdBeginRenderPass               CmdBeginRenderPass               = nullptr;
    PFN_vkCmdBeginRenderPass2              CmdBeginRenderPass2              = nullptr;
    PFN_vkCmdBeginRenderPass2              CmdBeginRenderPass2KHR           = nullptr;
    PFN_vkCmdBeginRendering                CmdBeginRendering                = nullptr;
    PFN_vkCmdBeginRendering                CmdBeginRenderingKHR             = nullptr;
    PFN_vkAllocateMemory                   AllocateMemory                   = nullptr;
    PFN_vkFreeMemory                       FreeMemory                       = nullptr;
    PFN_vkMapMemory                        MapMemory                        = nullptr;
    PFN_vkMapMemory2                       MapMemory2                       = nullptr;
    PFN_vkMapMemory2                       MapMemory2KHR                    = nullptr;
    PFN_vkBindBufferMemory2                BindBufferMemory2                = nullptr;
    PFN_vkBindBufferMemory2                BindBufferMemory2KHR             = nullptr;
    PFN_vkBindImageMemory2                 BindImageMemory2                 = nullptr;
    PFN_vkBindImageMemory2                 BindImageMemory2KHR              = nullptr;
    PFN_vkQueuePresentKHR                  QueuePresentKHR                  = nullptr;
    PFN_vkAcquireNextImage2KHR             AcquireNextImage2KHR             = nullptr;

    PFN_vkCreateBuffer                     CreateBuffer                     = nullptr;
    PFN_vkDestroyBuffer                    DestroyBuffer                    = nullptr;
    PFN_vkCreateImage                      CreateImage                      = nullptr;
    PFN_vkDestroyImage                     DestroyImage                     = nullptr;
    PFN_vkBindBufferMemory                 BindBufferMemory                 = nullptr;
    PFN_vkBindImageMemory                  BindImageMemory                  = nullptr;
    PFN_vkCreateImageView                  CreateImageView                  = nullptr;
    PFN_vkDestroyImageView                 DestroyImageView                 = nullptr;
    PFN_vkCreateRenderPass                 CreateRenderPass                 = nullptr;
    PFN_vkCreateRenderPass2                CreateRenderPass2                = nullptr;
    PFN_vkCreateRenderPass2                CreateRenderPass2KHR             = nullptr;
    PFN_vkDestroyRenderPass                DestroyRenderPass                = nullptr;
    PFN_vkCreateFramebuffer                CreateFramebuffer                = nullptr;
    PFN_vkDestroyFramebuffer               DestroyFramebuffer               = nullptr;
    PFN_vkAllocateCommandBuffers           AllocateCommandBuffers           = nullptr;
    PFN_vkDestroyCommandPool               DestroyCommandPool               = nullptr;
    PFN_vkCmdExecuteCommands               CmdExecuteCommands               = nullptr;
    PFN_vkCmdCopyBuffer                    CmdCopyBuffer                    = nullptr;
    PFN_vkCmdCopyImage                     CmdCopyImage                     = nullptr;
    PFN_vkCmdCopyBufferToImage             CmdCopyBufferToImage             = nullptr;
    PFN_vkCmdCopyImageToBuffer             CmdCopyImageToBuffer             = nullptr;
    PFN_vkCmdCopyBuffer2                   CmdCopyBuffer2                   = nullptr;
    PFN_vkCmdCopyImage2                    CmdCopyImage2                    = nullptr;
    PFN_vkCmdCopyBufferToImage2            CmdCopyBufferToImage2            = nullptr;
    PFN_vkCmdCopyImageToBuffer2            CmdCopyImageToBuffer2            = nullptr;
    PFN_vkCmdCopyBuffer2                   CmdCopyBuffer2KHR                = nullptr;
    PFN_vkCmdCopyImage2                    CmdCopyImage2KHR                 = nullptr;
    PFN_vkCmdCopyBufferToImage2            CmdCopyBufferToImage2KHR         = nullptr;
    PFN_vkCmdCopyImageToBuffer2            CmdCopyImageToBuffer2KHR         = nullptr;
    PFN_vkCmdBlitImage                     CmdBlitImage                     = nullptr;
    PFN_vkCmdBlitImage2                    CmdBlitImage2                    = nullptr;
    PFN_vkCmdBlitImage2                    CmdBlitImage2KHR                 = nullptr;
    PFN_vkCmdResolveImage                  CmdResolveImage                  = nullptr;
    PFN_vkCmdResolveImage2                 CmdResolveImage2                 = nullptr;
    PFN_vkCmdResolveImage2                 CmdResolveImage2KHR              = nullptr;
    PFN_vkCmdClearColorImage               CmdClearColorImage               = nullptr;
    PFN_vkCmdClearDepthStencilImage        CmdClearDepthStencilImage        = nullptr;
    PFN_vkCmdFillBuffer                    CmdFillBuffer                    = nullptr;
    PFN_vkCmdUpdateBuffer                  CmdUpdateBuffer                  = nullptr;
    PFN_vkCmdCopyQueryPoolResults          CmdCopyQueryPoolResults          = nullptr;
    PFN_vkCmdBindVertexBuffers             CmdBindVertexBuffers             = nullptr;
    PFN_vkCmdBindIndexBuffer               CmdBindIndexBuffer               = nullptr;
    PFN_vkCmdDraw                          CmdDraw                          = nullptr;
    PFN_vkCmdDrawIndexed                   CmdDrawIndexed                   = nullptr;
    PFN_vkCmdDrawIndirect                  CmdDrawIndirect                  = nullptr;
    PFN_vkCmdDrawIndexedIndirect           CmdDrawIndexedIndirect           = nullptr;
    PFN_vkGetDeviceGroupPresentCapabilitiesKHR GetDeviceGroupPresentCapabilitiesKHR = nullptr;
    PFN_vkGetDeviceGroupSurfacePresentModesKHR GetDeviceGroupSurfacePresentModesKHR = nullptr;
    PFN_vkCreateSwapchainKHR               CreateSwapchainKHR               = nullptr;
    PFN_vkDestroySwapchainKHR              DestroySwapchainKHR              = nullptr;

    // Per memory heap: device local (multi-instance in a group of several devices)
    std::vector<bool> DeviceLocalHeap;
    std::vector<uint32_t> TypeHeap; // memory type -> heap

    // State for validation and the memory-instance model (LayerMemoryModel.hpp)
    std::mutex                                    Mutex;
    std::unordered_map<VkCommandBuffer, uint32_t> BeginMasks; // device mask at vkBeginCommandBuffer
    std::unordered_map<VkDeviceMemory, bool>      MultiInstance; // allocations with several instances
    std::unique_ptr<MemoryModel>                  Model;
};

// Loader dispatch key of a dispatchable handle
inline void* GetDispatchKey(const void* Handle)
{
    return *static_cast<void* const*>(Handle);
}

// The device of a dispatchable handle; null if the device is not known
DeviceData* FindDeviceByKey(const void* Handle);

} // namespace VkSim
