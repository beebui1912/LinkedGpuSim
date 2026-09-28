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

#include <vulkan/vulkan.h>

namespace VkSim
{

struct InstanceData
{
    VkInstance                                  Instance                             = VK_NULL_HANDLE;

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

struct DeviceData
{
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice     DestroyDevice     = nullptr;
    VkDevice                Device            = VK_NULL_HANDLE;
};

} // namespace VkSim
