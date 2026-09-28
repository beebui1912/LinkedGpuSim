/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  WrappedPhysicalDevice
//  ---------------------
//  Dispatchable-handle wrapper for VkPhysicalDevice.
//
//  Physical devices are dispatchable Vulkan handles: the very first machine
//  word at the handle's address is the "loader dispatch key" the Vulkan
//  loader uses to route calls through the layer/ICD chain.  When our layer
//  synthesizes multiple virtual "nodes" backed by ONE real physical device,
//  we hand out N distinct handles, each of which:
//
//     - carries a COPY of the real device's dispatch key at offset 0
//       (so the loader can still dispatch calls made on the wrapper),
//     - stores the real handle + our metadata after the dispatch key
//       (which is why our layer MUST intercept every function that takes
//       a VkPhysicalDevice and unwrap before chaining down).
//
//  Layer_vkEnumeratePhysicalDeviceGroups creates the wrappers;
//  Layer_vkDestroyInstance frees them.

#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace VkSim
{

struct WrappedPhysicalDevice
{
    // The Vulkan loader identifies a dispatchable handle by reading
    // *(void**)handle.  We copy this from the real physical device so the
    // loader/ICD/next-layer's own dispatch continues to work.  MUST BE FIRST.
    void* DispatchKey = nullptr;

    // Sentinel used by TryUnwrap() as a defence-in-depth identity check on
    // top of the registry lookup.  Placed early so a stray physical device
    // pointer is unlikely to alias it.
    uint32_t Magic = 0;
    static constexpr uint32_t kMagic = 0xDCE0C0DEu; // "DiligentGpuSim linked node"

    VkPhysicalDevice Real           = VK_NULL_HANDLE; // Backing real device.
    VkInstance       ParentInstance = VK_NULL_HANDLE; // Instance the real device came from.
    uint32_t         NodeIndex      = 0;
    uint32_t         NodeCount      = 1;
};

// Allocates a wrapper backed by 'Real', copies the loader dispatch key from
// it, and registers the wrapper.  Returns the wrapper cast as VkPhysicalDevice.
VkPhysicalDevice WrapPhysicalDevice(VkPhysicalDevice Real,
                                    VkInstance       ParentInstance,
                                    uint32_t         NodeIndex,
                                    uint32_t         NodeCount);

// Returns the wrapper struct if 'pd' is one we allocated, otherwise nullptr.
// Safe to call on any VkPhysicalDevice - real handles will simply return
// nullptr because they aren't in our registry.
WrappedPhysicalDevice* TryUnwrap(VkPhysicalDevice pd);

// Convenience: if pd is a wrapper, returns wrapper->Real; otherwise pd
// unchanged.  Use this at the start of any intercept that forwards to the
// next chain.
inline VkPhysicalDevice UnwrapOr(VkPhysicalDevice pd)
{
    WrappedPhysicalDevice* w = TryUnwrap(pd);
    return w ? w->Real : pd;
}

// Frees every wrapper whose ParentInstance matches; called from
// vkDestroyInstance so state doesn't leak across instances.
void ReleaseWrappersForInstance(VkInstance Instance);

// Frees every wrapper (DllMain(DETACH) safety net).
void ReleaseAllWrappers();

} // namespace VkSim
