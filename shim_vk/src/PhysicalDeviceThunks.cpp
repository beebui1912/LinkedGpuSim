/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "PhysicalDeviceThunks.hpp"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include "LayerLog.hpp"
#include "WrappedPhysicalDevice.hpp"

namespace VkSim
{

namespace
{

static_assert(sizeof(void*) == 8, "the thunks are x64 code");
static_assert(offsetof(WrappedPhysicalDevice, Magic) == 8, "the thunk reads the magic at offset 8");
static_assert(offsetof(WrappedPhysicalDevice, Real) == 16, "the thunk reads the real handle at offset 16");

constexpr size_t kThunkSize = 32;
constexpr size_t kPageSize  = 4096;

std::mutex                                       g_Mutex;
std::unordered_map<void*, PFN_vkVoidFunction>    g_Thunks; // next function -> thunk
unsigned char*                                   g_Page     = nullptr;
size_t                                           g_PageUsed = kPageSize;

} // namespace

bool IsPhysicalDeviceFunction(const char* pName)
{
    if (pName == nullptr)
        return false;
    static const char* const kPrefixes[] = {
        "vkGetPhysicalDevice",      // properties, features, formats, surfaces, tools, ...
        "vkGetDisplay",             // VK_KHR_display / display_properties2
        "vkCreateDisplayMode",      // VK_KHR_display
        "vkReleaseDisplay",         // VK_EXT_direct_mode_display
        "vkAcquireXlibDisplay",     // VK_EXT_acquire_xlib_display
        "vkGetRandROutputDisplay",  // VK_EXT_acquire_xlib_display
        "vkAcquireWinrtDisplay",    // VK_NV_acquire_winrt_display
        "vkGetWinrtDisplay",        // VK_NV_acquire_winrt_display
        "vkAcquireDrmDisplay",      // VK_EXT_acquire_drm_display
        "vkGetDrmDisplay",          // VK_EXT_acquire_drm_display
        "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCounters", // VK_KHR_performance_query
    };
    for (const char* pPrefix : kPrefixes)
        if (std::strncmp(pName, pPrefix, std::strlen(pPrefix)) == 0)
            return true;
    return false;
}

PFN_vkVoidFunction MakeUnwrapThunk(PFN_vkVoidFunction Next)
{
    if (Next == nullptr)
        return nullptr;
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_Thunks.find(reinterpret_cast<void*>(Next));
    if (It != g_Thunks.end())
        return It->second;

    if (g_PageUsed + kThunkSize > kPageSize)
    {
        g_Page = static_cast<unsigned char*>(::VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (g_Page == nullptr)
        {
            LogError("MakeUnwrapThunk: VirtualAlloc failed (%lu)", ::GetLastError());
            return Next;
        }
        g_PageUsed = 0;
    }
    unsigned char* p = g_Page + g_PageUsed;
    // mov eax, [rcx+8]; cmp eax, kMagic; jne skip; mov rcx, [rcx+16]; skip: jmp [rip+0]; dq Next
    const unsigned char Code[] = {
        0x8B, 0x41, 0x08,                   // mov eax, dword ptr [rcx+8]
        0x3D, 0, 0, 0, 0,                   // cmp eax, imm32 (magic)
        0x75, 0x04,                         // jne +4
        0x48, 0x8B, 0x49, 0x10,             // mov rcx, qword ptr [rcx+16]
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, // jmp qword ptr [rip+0]
    };
    std::memcpy(p, Code, sizeof(Code));
    const uint32_t Magic = WrappedPhysicalDevice::kMagic;
    std::memcpy(p + 4, &Magic, sizeof(Magic));
    void* Target = reinterpret_cast<void*>(Next);
    std::memcpy(p + sizeof(Code), &Target, sizeof(Target));
    ::FlushInstructionCache(::GetCurrentProcess(), p, kThunkSize);
    g_PageUsed += kThunkSize;

    auto Thunk = reinterpret_cast<PFN_vkVoidFunction>(p);
    g_Thunks.emplace(reinterpret_cast<void*>(Next), Thunk);
    return Thunk;
}

} // namespace VkSim
