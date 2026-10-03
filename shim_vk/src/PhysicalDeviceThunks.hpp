/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  PhysicalDeviceThunks
//  --------------------
//  The simulated devices 1..N-1 of a group are wrapper handles, and they are
//  returned by vkEnumeratePhysicalDevices like every device of a real group.
//  An application may call any physical-device-level function on them,
//  including extension functions the layer does not know. Each such function
//  gets a small x64 thunk: if the first argument (the VkPhysicalDevice, in RCX)
//  is a wrapper, it is replaced by the real handle; then the thunk jumps to the
//  next layer's function. No signature knowledge is needed.

#pragma once

#include <vulkan/vulkan.h>

namespace VkSim
{

// Whether a function takes a VkPhysicalDevice as its first parameter
bool IsPhysicalDeviceFunction(const char* pName);

// A thunk that unwraps the first argument and continues at Next (cached per Next)
PFN_vkVoidFunction MakeUnwrapThunk(PFN_vkVoidFunction Next);

} // namespace VkSim
