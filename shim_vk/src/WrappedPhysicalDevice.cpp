/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "WrappedPhysicalDevice.hpp"

#include <mutex>
#include <unordered_set>

#include "LayerLog.hpp"

namespace VkSim
{

namespace
{

std::mutex                                 g_Mutex;
std::unordered_set<WrappedPhysicalDevice*> g_Wrappers;

} // namespace

VkPhysicalDevice WrapPhysicalDevice(VkPhysicalDevice Real,
                                    VkInstance       ParentInstance,
                                    uint32_t         NodeIndex,
                                    uint32_t         NodeCount)
{
    if (Real == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    auto* w = new WrappedPhysicalDevice;
    // First machine-word must be the loader dispatch key from the real handle.
    w->DispatchKey    = *reinterpret_cast<void**>(Real);
    w->Magic          = WrappedPhysicalDevice::kMagic;
    w->Real           = Real;
    w->ParentInstance = ParentInstance;
    w->NodeIndex      = NodeIndex;
    w->NodeCount      = NodeCount;

    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        g_Wrappers.insert(w);
    }

    LogVerbose("Wrapped VkPhysicalDevice %p -> %p (real=%p node=%u/%u)",
               static_cast<void*>(Real),
               static_cast<void*>(reinterpret_cast<VkPhysicalDevice>(w)),
               static_cast<void*>(Real),
               NodeIndex, NodeCount);
    return reinterpret_cast<VkPhysicalDevice>(w);
}

WrappedPhysicalDevice* TryUnwrap(VkPhysicalDevice pd)
{
    if (pd == VK_NULL_HANDLE) return nullptr;
    auto* Candidate = reinterpret_cast<WrappedPhysicalDevice*>(pd);

    std::lock_guard<std::mutex> Lock(g_Mutex);
    if (g_Wrappers.count(Candidate) == 0)
        return nullptr;

    // Registry says yes; sanity-check the magic (guards against a wrapper we
    // freed and never re-allocated leaving a stale pointer around).
    if (Candidate->Magic != WrappedPhysicalDevice::kMagic)
        return nullptr;

    return Candidate;
}

void ReleaseWrappersForInstance(VkInstance Instance)
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    for (auto It = g_Wrappers.begin(); It != g_Wrappers.end();)
    {
        if ((*It)->ParentInstance == Instance)
        {
            delete *It;
            It = g_Wrappers.erase(It);
        }
        else
        {
            ++It;
        }
    }
}

void ReleaseAllWrappers()
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    for (auto* w : g_Wrappers)
        delete w;
    g_Wrappers.clear();
}

} // namespace VkSim
