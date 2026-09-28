/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  VkLayer_DiligentGpuSim - LayerMain.cpp
//  =====================================
//  Full linked-device-group synthesis for the Diligent GPU Simulation.
//
//  vkEnumeratePhysicalDeviceGroups is intercepted to return ONE group with
//  DILIGENT_SIM_LINKED_NODE_COUNT wrapped copies of the primary physical
//  device.  Every function that takes a VkPhysicalDevice is intercepted to
//  unwrap it before chaining down (the ICD would otherwise interpret the
//  wrapper struct's metadata as its own driver fields and crash).
//  Two of those intercepts also alter the returned data:
//
//    * vkGetPhysicalDeviceProperties / Properties2 - appends
//      "[Simulated Node k]" to VkPhysicalDeviceProperties::deviceName so
//      Diligent's samples surface the virtual-node identity in their UI.
//    * vkGetPhysicalDeviceMemoryProperties / MemoryProperties2 - divides the
//      DEVICE_LOCAL heap size by NodeCount so per-node budgets look
//      proportional (mirrors what VirtualDXGIAdapter does on the D3D12 side).
//
//  Diligent's Vulkan factory reads
//    DeviceGroupCI.physicalDeviceCount == NodeCount
//  and, when > 1, sets AdapterInfo.NodeCount = NodeCount + enables linked
//  multi-GPU with N views.  That gets Tutorial31 into the split-strip path.

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "LayerDispatch.hpp"
#include "LayerLog.hpp"
#include "WrappedPhysicalDevice.hpp"

// Exports come from VkLayer_DiligentGpuSim.def; the function signatures below
// match the pre-declarations in <vulkan/vk_layer.h>.

namespace VkSim
{

// ---------------------------------------------------------------------------
// Config + globals
// ---------------------------------------------------------------------------

static uint32_t GetSimulatedNodeCountFromEnv()
{
    wchar_t Buf[16]{};
    const DWORD Len = ::GetEnvironmentVariableW(L"DILIGENT_SIM_LINKED_NODE_COUNT",
                                                Buf, static_cast<DWORD>(std::size(Buf)));
    if (Len == 0 || Len >= std::size(Buf))
        return 2;
    unsigned long V = std::wcstoul(Buf, nullptr, 10);
    if (V < 1) V = 1;
    if (V > 8) V = 8;
    return static_cast<uint32_t>(V);
}

static uint32_t GetSimulatedNodeCount()
{
    static const uint32_t N = GetSimulatedNodeCountFromEnv();
    return N;
}


std::mutex                                          g_Mutex;
std::unordered_map<VkInstance, InstanceData>        g_Instances;
std::unordered_map<VkDevice,   DeviceData>          g_Devices;

// Cache of vkEnumeratePhysicalDevices results per instance (real handles) so
// we can pick the primary when synthesizing groups.
std::unordered_map<VkInstance, std::vector<VkPhysicalDevice>> g_RealPhysicalDevices;

InstanceData* FindInstance(VkInstance Inst)
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    auto It = g_Instances.find(Inst);
    return It == g_Instances.end() ? nullptr : &It->second;
}

DeviceData* FindDevice(VkDevice Dev)
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    auto It = g_Devices.find(Dev);
    return It == g_Devices.end() ? nullptr : &It->second;
}


// Resolve the InstanceData that a physical device (wrapped or real) belongs
// to.  Wrappers know their parent instance directly.  For real handles, we
// scan our cached enumeration results.
InstanceData* GetInstanceForPD(VkPhysicalDevice pd)
{
    if (WrappedPhysicalDevice* w = TryUnwrap(pd))
        return FindInstance(w->ParentInstance);

    std::lock_guard<std::mutex> Lock(g_Mutex);
    for (auto& Kv : g_RealPhysicalDevices)
    {
        for (VkPhysicalDevice p : Kv.second)
        {
            if (p == pd)
            {
                auto It = g_Instances.find(Kv.first);
                return It == g_Instances.end() ? nullptr : &It->second;
            }
        }
    }
    return nullptr;
}


// ---------------------------------------------------------------------------
// Chain-info helpers (walk pCreateInfo->pNext for the VkLayer*CreateInfo)
// ---------------------------------------------------------------------------

VkLayerInstanceCreateInfo* GetInstanceChainInfo(const VkInstanceCreateInfo* pCreateInfo, VkLayerFunction Fn)
{
    auto* p = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
    while (p != nullptr)
    {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
        {
            auto* Ci = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if (Ci->function == Fn)
                return Ci;
        }
        p = p->pNext;
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* GetDeviceChainInfo(const VkDeviceCreateInfo* pCreateInfo, VkLayerFunction Fn)
{
    auto* p = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
    while (p != nullptr)
    {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
        {
            auto* Ci = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(p));
            if (Ci->function == Fn)
                return Ci;
        }
        p = p->pNext;
    }
    return nullptr;
}


// Forward declaration - referenced by the router.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Layer_vkGetDeviceProcAddr(VkDevice Device, const char* pName);


// ---------------------------------------------------------------------------
// vkCreateInstance / vkDestroyInstance
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkCreateInstance(
    const VkInstanceCreateInfo*     pCreateInfo,
    const VkAllocationCallbacks*    pAllocator,
    VkInstance*                     pInstance)
{
    VkLayerInstanceCreateInfo* pLayerCi = GetInstanceChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
    if (pLayerCi == nullptr || pLayerCi->u.pLayerInfo == nullptr)
    {
        LogError("vkCreateInstance: missing layer chain info");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr pfnNextGipa = pLayerCi->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    pLayerCi->u.pLayerInfo = pLayerCi->u.pLayerInfo->pNext;

    auto pfnNextCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        pfnNextGipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (pfnNextCreateInstance == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult r = pfnNextCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS)
        return r;

    // Populate every function pointer we'll ever need on the instance chain.
    InstanceData d{};
    d.Instance            = *pInstance;
    d.GetInstanceProcAddr = pfnNextGipa;
#define GET_INST_FN(field, name) \
    d.field = reinterpret_cast<PFN_vk##field>(pfnNextGipa(*pInstance, "vk" name))

    GET_INST_FN(DestroyInstance,                     "DestroyInstance");
    GET_INST_FN(EnumeratePhysicalDevices,            "EnumeratePhysicalDevices");
    GET_INST_FN(EnumeratePhysicalDeviceGroups,       "EnumeratePhysicalDeviceGroups");
    GET_INST_FN(EnumeratePhysicalDeviceGroupsKHR,    "EnumeratePhysicalDeviceGroupsKHR");
    GET_INST_FN(CreateDevice,                        "CreateDevice");
    GET_INST_FN(EnumerateDeviceExtensionProperties,  "EnumerateDeviceExtensionProperties");
    GET_INST_FN(EnumerateDeviceLayerProperties,      "EnumerateDeviceLayerProperties");

    GET_INST_FN(GetPhysicalDeviceProperties,         "GetPhysicalDeviceProperties");
    GET_INST_FN(GetPhysicalDeviceProperties2,        "GetPhysicalDeviceProperties2");
    GET_INST_FN(GetPhysicalDeviceProperties2KHR,     "GetPhysicalDeviceProperties2KHR");
    GET_INST_FN(GetPhysicalDeviceFeatures,           "GetPhysicalDeviceFeatures");
    GET_INST_FN(GetPhysicalDeviceFeatures2,          "GetPhysicalDeviceFeatures2");
    GET_INST_FN(GetPhysicalDeviceFeatures2KHR,       "GetPhysicalDeviceFeatures2KHR");
    GET_INST_FN(GetPhysicalDeviceMemoryProperties,   "GetPhysicalDeviceMemoryProperties");
    GET_INST_FN(GetPhysicalDeviceMemoryProperties2,  "GetPhysicalDeviceMemoryProperties2");
    GET_INST_FN(GetPhysicalDeviceMemoryProperties2KHR,"GetPhysicalDeviceMemoryProperties2KHR");
    GET_INST_FN(GetPhysicalDeviceQueueFamilyProperties,      "GetPhysicalDeviceQueueFamilyProperties");
    GET_INST_FN(GetPhysicalDeviceQueueFamilyProperties2,     "GetPhysicalDeviceQueueFamilyProperties2");
    GET_INST_FN(GetPhysicalDeviceQueueFamilyProperties2KHR,  "GetPhysicalDeviceQueueFamilyProperties2KHR");
    GET_INST_FN(GetPhysicalDeviceFormatProperties,           "GetPhysicalDeviceFormatProperties");
    GET_INST_FN(GetPhysicalDeviceFormatProperties2,          "GetPhysicalDeviceFormatProperties2");
    GET_INST_FN(GetPhysicalDeviceFormatProperties2KHR,       "GetPhysicalDeviceFormatProperties2KHR");
    GET_INST_FN(GetPhysicalDeviceImageFormatProperties,      "GetPhysicalDeviceImageFormatProperties");
    GET_INST_FN(GetPhysicalDeviceImageFormatProperties2,     "GetPhysicalDeviceImageFormatProperties2");
    GET_INST_FN(GetPhysicalDeviceImageFormatProperties2KHR,  "GetPhysicalDeviceImageFormatProperties2KHR");
    GET_INST_FN(GetPhysicalDeviceSparseImageFormatProperties,     "GetPhysicalDeviceSparseImageFormatProperties");
    GET_INST_FN(GetPhysicalDeviceSparseImageFormatProperties2,    "GetPhysicalDeviceSparseImageFormatProperties2");
    GET_INST_FN(GetPhysicalDeviceSparseImageFormatProperties2KHR, "GetPhysicalDeviceSparseImageFormatProperties2KHR");
    GET_INST_FN(GetPhysicalDeviceExternalBufferProperties,        "GetPhysicalDeviceExternalBufferProperties");
    GET_INST_FN(GetPhysicalDeviceExternalBufferPropertiesKHR,     "GetPhysicalDeviceExternalBufferPropertiesKHR");
    GET_INST_FN(GetPhysicalDeviceExternalFenceProperties,         "GetPhysicalDeviceExternalFenceProperties");
    GET_INST_FN(GetPhysicalDeviceExternalFencePropertiesKHR,      "GetPhysicalDeviceExternalFencePropertiesKHR");
    GET_INST_FN(GetPhysicalDeviceExternalSemaphoreProperties,     "GetPhysicalDeviceExternalSemaphoreProperties");
    GET_INST_FN(GetPhysicalDeviceExternalSemaphorePropertiesKHR,  "GetPhysicalDeviceExternalSemaphorePropertiesKHR");

    GET_INST_FN(GetPhysicalDeviceSurfaceSupportKHR,      "GetPhysicalDeviceSurfaceSupportKHR");
    GET_INST_FN(GetPhysicalDeviceSurfaceCapabilitiesKHR, "GetPhysicalDeviceSurfaceCapabilitiesKHR");
    GET_INST_FN(GetPhysicalDeviceSurfaceFormatsKHR,      "GetPhysicalDeviceSurfaceFormatsKHR");
    GET_INST_FN(GetPhysicalDeviceSurfacePresentModesKHR, "GetPhysicalDeviceSurfacePresentModesKHR");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    GET_INST_FN(GetPhysicalDeviceWin32PresentationSupportKHR, "GetPhysicalDeviceWin32PresentationSupportKHR");
#endif
#undef GET_INST_FN

    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        g_Instances[*pInstance] = d;
    }

    LogInfo("vkCreateInstance: instance %p bound (SimNodeCount=%u).",
            *pInstance, GetSimulatedNodeCount());
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Layer_vkDestroyInstance(VkInstance Instance, const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroyInstance pfn = nullptr;
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        auto It = g_Instances.find(Instance);
        if (It != g_Instances.end())
        {
            pfn = It->second.DestroyInstance;
            g_Instances.erase(It);
        }
        g_RealPhysicalDevices.erase(Instance);
    }
    // Free wrappers whose parent instance is now gone.
    ReleaseWrappersForInstance(Instance);

    LogVerbose("vkDestroyInstance: %p", Instance);
    if (pfn) pfn(Instance, pAllocator);
}


// ---------------------------------------------------------------------------
// Physical device enumeration - both single-list and group flavours
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumeratePhysicalDevices(
    VkInstance                          Instance,
    uint32_t*                           pPhysicalDeviceCount,
    VkPhysicalDevice*                   pPhysicalDevices)
{
    InstanceData* pInst = FindInstance(Instance);
    if (pInst == nullptr || pInst->EnumeratePhysicalDevices == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Forward as-is; we return REAL handles (not wrapped) from this path.
    // Diligent's linked-multi-GPU code path uses EnumeratePhysicalDeviceGroups
    // where our wrappers show up.
    const VkResult r = pInst->EnumeratePhysicalDevices(Instance, pPhysicalDeviceCount, pPhysicalDevices);
    if (r == VK_SUCCESS && pPhysicalDevices != nullptr && pPhysicalDeviceCount != nullptr)
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        auto& Vec = g_RealPhysicalDevices[Instance];
        Vec.assign(pPhysicalDevices, pPhysicalDevices + *pPhysicalDeviceCount);
    }
    return r;
}


VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumeratePhysicalDeviceGroups(
    VkInstance                          Instance,
    uint32_t*                           pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*    pPhysicalDeviceGroupProperties)
{
    if (pPhysicalDeviceGroupCount == nullptr)
        return VK_INCOMPLETE;

    InstanceData* pInst = FindInstance(Instance);
    if (pInst == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    auto pfnReal = pInst->EnumeratePhysicalDeviceGroups
                       ? pInst->EnumeratePhysicalDeviceGroups
                       : pInst->EnumeratePhysicalDeviceGroupsKHR;
    if (pfnReal == nullptr)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    const uint32_t N = GetSimulatedNodeCount();

    // Two-call idiom: probe first with pProperties==nullptr, then fill.
    if (pPhysicalDeviceGroupProperties == nullptr)
    {
        // Report exactly one group - the synthesized linked group.
        *pPhysicalDeviceGroupCount = 1;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceGroupCount < 1)
    {
        *pPhysicalDeviceGroupCount = 0;
        return VK_INCOMPLETE;
    }

    // Find the primary real physical device.  Prefer the cached list; if
    // empty, do a live enumeration via the next-layer function.
    VkPhysicalDevice Primary = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        auto It = g_RealPhysicalDevices.find(Instance);
        if (It != g_RealPhysicalDevices.end() && !It->second.empty())
            Primary = It->second.front();
    }
    if (Primary == VK_NULL_HANDLE)
    {
        uint32_t                       Count = 0;
        pInst->EnumeratePhysicalDevices(Instance, &Count, nullptr);
        if (Count == 0) return VK_ERROR_INITIALIZATION_FAILED;
        std::vector<VkPhysicalDevice> Pds(Count);
        pInst->EnumeratePhysicalDevices(Instance, &Count, Pds.data());
        Primary = Pds.front();

        std::lock_guard<std::mutex> Lock(g_Mutex);
        g_RealPhysicalDevices[Instance] = std::move(Pds);
    }

    // Fill the (single) synthesized group.
    VkPhysicalDeviceGroupProperties& G = pPhysicalDeviceGroupProperties[0];
    std::memset(&G, 0, sizeof(G));
    G.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;

    const uint32_t Capacity = static_cast<uint32_t>(VK_MAX_DEVICE_GROUP_SIZE);
    const uint32_t NClamped = (N > Capacity) ? Capacity : N;
    G.physicalDeviceCount = NClamped;
    // Index 0 is the REAL primary so callers doing pointer equality against
    // whatever they got out of vkEnumeratePhysicalDevices (e.g. Diligent's
    // linked-group membership check inside EnumerateAdapters) still match.
    // Indices 1..N-1 are dispatchable wrappers so the Vulkan spec's
    // "distinct handles" requirement inside a group is honoured.  Every
    // wrapped handle is unwrapped back to the real primary by our
    // Layer_vkCreateDevice intercept before the ICD sees it.
    G.physicalDevices[0] = Primary;
    for (uint32_t k = 1; k < NClamped; ++k)
        G.physicalDevices[k] = WrapPhysicalDevice(Primary, Instance, k, NClamped);
    // Subset allocations aren't supported by our fake group.
    G.subsetAllocation = VK_FALSE;

    *pPhysicalDeviceGroupCount = 1;
    LogInfo("vkEnumeratePhysicalDeviceGroups: synthesized 1 group with %u physical devices "
            "(primary=%p, %u wrapped nodes).", NClamped, static_cast<void*>(Primary), NClamped - 1u);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumeratePhysicalDeviceGroupsKHR(
    VkInstance                          Instance,
    uint32_t*                           pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*    pPhysicalDeviceGroupProperties)
{
    return Layer_vkEnumeratePhysicalDeviceGroups(
        Instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}


// ---------------------------------------------------------------------------
// vkCreateDevice - unwrap the physical device + any wrapped entries in
// VkDeviceGroupDeviceCreateInfo::pPhysicalDevices before chaining.
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkCreateDevice(
    VkPhysicalDevice                PhysicalDevice,
    const VkDeviceCreateInfo*       pCreateInfo,
    const VkAllocationCallbacks*    pAllocator,
    VkDevice*                       pDevice)
{
    VkLayerDeviceCreateInfo* pLayerCi = GetDeviceChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
    if (pLayerCi == nullptr || pLayerCi->u.pLayerInfo == nullptr)
    {
        LogError("vkCreateDevice: missing layer chain info");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_vkGetInstanceProcAddr pfnNextGipa = pLayerCi->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   pfnNextGdpa = pLayerCi->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    pLayerCi->u.pLayerInfo = pLayerCi->u.pLayerInfo->pNext;

    auto pfnNextCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(
        pfnNextGipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (pfnNextCreateDevice == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Unwrap the main physicalDevice argument.
    VkPhysicalDevice RealPhys = UnwrapOr(PhysicalDevice);

    // We want to rewrite VkDeviceGroupDeviceCreateInfo::pPhysicalDevices to
    // hold unwrapped handles.  Since we can't modify the caller's linked
    // list nodes in place (they're const), we take a shallow copy of the
    // outer VkDeviceCreateInfo and, if the group info is found, splice a
    // local copy of it in at the head of the pNext chain - our local group
    // info's pNext points to whatever came AFTER the original group info in
    // the caller's chain, and any nodes BEFORE the original group info stay
    // reachable through their positions in the caller's chain via a small
    // shim: we detect that case and issue a warning, since Diligent
    // typically places the group info at the head.
    VkDeviceCreateInfo             LocalCreateInfo   = *pCreateInfo;
    VkDeviceGroupDeviceCreateInfo  LocalGroupInfo{};
    std::vector<VkPhysicalDevice>  LocalGroupDevices;

    const VkBaseInStructure* pHead        = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
    const VkBaseInStructure* pGroupPrev   = nullptr; // node whose pNext is the group info, or null if head.
    const VkBaseInStructure* pGroupSrc    = nullptr;
    for (auto* p = pHead; p != nullptr; p = p->pNext)
    {
        if (p->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)
        {
            pGroupSrc = p;
            break;
        }
        pGroupPrev = p;
    }

    if (pGroupSrc != nullptr)
    {
        auto* SrcG = reinterpret_cast<const VkDeviceGroupDeviceCreateInfo*>(pGroupSrc);
        LocalGroupInfo = *SrcG;
        LocalGroupDevices.resize(SrcG->physicalDeviceCount);
        uint32_t Wrapped = 0;
        for (uint32_t i = 0; i < SrcG->physicalDeviceCount; ++i)
        {
            LocalGroupDevices[i] = UnwrapOr(SrcG->pPhysicalDevices[i]);
            if (LocalGroupDevices[i] != SrcG->pPhysicalDevices[i]) ++Wrapped;
        }
        LocalGroupInfo.pPhysicalDevices = LocalGroupDevices.data();
        // Skip past the original group info in the caller's chain.
        LocalGroupInfo.pNext = pGroupSrc->pNext;
        LogInfo("vkCreateDevice: rewrote VkDeviceGroupDeviceCreateInfo "
                "(physicalDeviceCount=%u, wrappedEntries=%u).",
                SrcG->physicalDeviceCount, Wrapped);

        if (pGroupPrev == nullptr)
        {
            // Common case: group info was at pNext head.  Point our outer
            // create info's pNext at our local copy.
            LocalCreateInfo.pNext = &LocalGroupInfo;
        }
        else
        {
            // Rare case: group info was deeper in the chain.  We can't
            // modify pGroupPrev->pNext (const caller memory), so fall back
            // to prepending our copy at the head and hoping the driver
            // tolerates a duplicate-looking chain.  Also warn.
            LogWarn("vkCreateDevice: VkDeviceGroupDeviceCreateInfo was not at pNext "
                    "head; prepending unwrapped copy at head (chain may have a "
                    "shadow entry).  If you see this in a real app, add a proper "
                    "deep-copy path.");
            LocalGroupInfo.pNext  = pCreateInfo->pNext; // preserve full incoming chain
            LocalCreateInfo.pNext = &LocalGroupInfo;
        }
    }

    const VkResult r = pfnNextCreateDevice(RealPhys, &LocalCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS)
    {
        LogError("vkCreateDevice failed: %d", static_cast<int>(r));
        return r;
    }

    DeviceData dd{};
    dd.Device            = *pDevice;
    dd.GetDeviceProcAddr = pfnNextGdpa;
    dd.DestroyDevice     = reinterpret_cast<PFN_vkDestroyDevice>(pfnNextGdpa(*pDevice, "vkDestroyDevice"));
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        g_Devices[*pDevice] = dd;
    }
    LogInfo("vkCreateDevice: device %p bound (real physical %p).", *pDevice, RealPhys);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Layer_vkDestroyDevice(VkDevice Device, const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroyDevice pfn = nullptr;
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        auto It = g_Devices.find(Device);
        if (It != g_Devices.end())
        {
            pfn = It->second.DestroyDevice;
            g_Devices.erase(It);
        }
    }
    LogVerbose("vkDestroyDevice: %p", Device);
    if (pfn) pfn(Device, pAllocator);
}


// ---------------------------------------------------------------------------
// vkGetPhysicalDeviceProperties[2] - unwrap + append "[Simulated Node k]"
// ---------------------------------------------------------------------------

static void ApplyNodeSuffixToDeviceName(char DeviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE],
                                        uint32_t NodeIndex)
{
    // Manual NUL-clamped length (strnlen isn't in std:: on MSVC).
    size_t CurLen = 0;
    while (CurLen < VK_MAX_PHYSICAL_DEVICE_NAME_SIZE && DeviceName[CurLen] != '\0')
        ++CurLen;

    char Suffix[48];
    const int SuffixLen = std::snprintf(Suffix, sizeof(Suffix),
                                        " [Simulated Node %u]", NodeIndex);
    if (SuffixLen <= 0) return;
    if (CurLen + 1u >= VK_MAX_PHYSICAL_DEVICE_NAME_SIZE) return;
    const size_t Room = static_cast<size_t>(VK_MAX_PHYSICAL_DEVICE_NAME_SIZE) - CurLen - 1u;
    const size_t Copy = (static_cast<size_t>(SuffixLen) < Room) ? static_cast<size_t>(SuffixLen) : Room;
    if (Copy == 0) return;
    std::memcpy(DeviceName + CurLen, Suffix, Copy);
    DeviceName[CurLen + Copy] = '\0';
}

VKAPI_ATTR void VKAPI_CALL Layer_GetPhysicalDeviceProperties(
    VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceProperties* pProperties)
{
    WrappedPhysicalDevice* w    = TryUnwrap(PhysicalDevice);
    VkPhysicalDevice       Real = w ? w->Real : PhysicalDevice;
    InstanceData*          Inst = GetInstanceForPD(PhysicalDevice);
    if (Inst == nullptr || Inst->GetPhysicalDeviceProperties == nullptr) return;

    Inst->GetPhysicalDeviceProperties(Real, pProperties);
    if (w != nullptr && pProperties != nullptr)
        ApplyNodeSuffixToDeviceName(pProperties->deviceName, w->NodeIndex);
}

VKAPI_ATTR void VKAPI_CALL Layer_GetPhysicalDeviceProperties2(
    VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceProperties2* pProperties)
{
    WrappedPhysicalDevice* w    = TryUnwrap(PhysicalDevice);
    VkPhysicalDevice       Real = w ? w->Real : PhysicalDevice;
    InstanceData*          Inst = GetInstanceForPD(PhysicalDevice);
    if (Inst == nullptr) return;

    if (Inst->GetPhysicalDeviceProperties2 != nullptr)
        Inst->GetPhysicalDeviceProperties2(Real, pProperties);
    else if (Inst->GetPhysicalDeviceProperties2KHR != nullptr)
        Inst->GetPhysicalDeviceProperties2KHR(Real, pProperties);
    else return;

    if (w != nullptr && pProperties != nullptr)
        ApplyNodeSuffixToDeviceName(pProperties->properties.deviceName, w->NodeIndex);
}

VKAPI_ATTR void VKAPI_CALL Layer_GetPhysicalDeviceProperties2KHR(
    VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceProperties2* pProperties)
{
    Layer_GetPhysicalDeviceProperties2(PhysicalDevice, pProperties);
}


// ---------------------------------------------------------------------------
// vkGetPhysicalDeviceMemoryProperties[2] - split DEVICE_LOCAL heap sizes by N.
// ---------------------------------------------------------------------------

static void SplitMemoryHeapSizes(VkPhysicalDeviceMemoryProperties* pMem, uint32_t NodeCount)
{
    if (pMem == nullptr || NodeCount <= 1) return;
    for (uint32_t i = 0; i < pMem->memoryHeapCount; ++i)
    {
        if (pMem->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            pMem->memoryHeaps[i].size /= NodeCount;
    }
}

VKAPI_ATTR void VKAPI_CALL Layer_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceMemoryProperties* pMem)
{
    WrappedPhysicalDevice* w    = TryUnwrap(PhysicalDevice);
    VkPhysicalDevice       Real = w ? w->Real : PhysicalDevice;
    InstanceData*          Inst = GetInstanceForPD(PhysicalDevice);
    if (Inst == nullptr || Inst->GetPhysicalDeviceMemoryProperties == nullptr) return;

    Inst->GetPhysicalDeviceMemoryProperties(Real, pMem);
    if (w != nullptr) SplitMemoryHeapSizes(pMem, w->NodeCount);
}

VKAPI_ATTR void VKAPI_CALL Layer_GetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceMemoryProperties2* pMem)
{
    WrappedPhysicalDevice* w    = TryUnwrap(PhysicalDevice);
    VkPhysicalDevice       Real = w ? w->Real : PhysicalDevice;
    InstanceData*          Inst = GetInstanceForPD(PhysicalDevice);
    if (Inst == nullptr) return;

    if (Inst->GetPhysicalDeviceMemoryProperties2 != nullptr)
        Inst->GetPhysicalDeviceMemoryProperties2(Real, pMem);
    else if (Inst->GetPhysicalDeviceMemoryProperties2KHR != nullptr)
        Inst->GetPhysicalDeviceMemoryProperties2KHR(Real, pMem);
    else return;

    if (w != nullptr && pMem != nullptr)
        SplitMemoryHeapSizes(&pMem->memoryProperties, w->NodeCount);
}

VKAPI_ATTR void VKAPI_CALL Layer_GetPhysicalDeviceMemoryProperties2KHR(
    VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceMemoryProperties2* pMem)
{
    Layer_GetPhysicalDeviceMemoryProperties2(PhysicalDevice, pMem);
}


// ---------------------------------------------------------------------------
// Pass-through unwrap for everything else that takes a VkPhysicalDevice.
// Macro to keep this section compact.
// ---------------------------------------------------------------------------

#define UNWRAP_VOID_CHAIN(Name, ArgsSig, ArgsCall)                                        \
    VKAPI_ATTR void VKAPI_CALL Layer_##Name ArgsSig                                       \
    {                                                                                     \
        VkPhysicalDevice Real = UnwrapOr(PhysicalDevice);                                 \
        InstanceData*    Inst = GetInstanceForPD(PhysicalDevice);                         \
        if (Inst != nullptr && Inst->Name != nullptr) Inst->Name ArgsCall;                \
    }

#define UNWRAP_RESULT_CHAIN(Name, ArgsSig, ArgsCall)                                      \
    VKAPI_ATTR VkResult VKAPI_CALL Layer_##Name ArgsSig                                   \
    {                                                                                     \
        VkPhysicalDevice Real = UnwrapOr(PhysicalDevice);                                 \
        InstanceData*    Inst = GetInstanceForPD(PhysicalDevice);                         \
        if (Inst == nullptr || Inst->Name == nullptr) return VK_ERROR_INITIALIZATION_FAILED; \
        return Inst->Name ArgsCall;                                                       \
    }

// vkGetPhysicalDeviceFeatures / 2 / 2KHR
UNWRAP_VOID_CHAIN(GetPhysicalDeviceFeatures,
    (VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceFeatures* pFeatures),
    (Real, pFeatures))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceFeatures2,
    (VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceFeatures2* pFeatures),
    (Real, pFeatures))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceFeatures2KHR,
    (VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceFeatures2* pFeatures),
    (Real, pFeatures))

// vkGetPhysicalDeviceQueueFamilyProperties / 2 / 2KHR
UNWRAP_VOID_CHAIN(GetPhysicalDeviceQueueFamilyProperties,
    (VkPhysicalDevice PhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties* pProps),
    (Real, pCount, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceQueueFamilyProperties2,
    (VkPhysicalDevice PhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties2* pProps),
    (Real, pCount, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceQueueFamilyProperties2KHR,
    (VkPhysicalDevice PhysicalDevice, uint32_t* pCount, VkQueueFamilyProperties2* pProps),
    (Real, pCount, pProps))

// vkGetPhysicalDeviceFormatProperties / 2 / 2KHR
UNWRAP_VOID_CHAIN(GetPhysicalDeviceFormatProperties,
    (VkPhysicalDevice PhysicalDevice, VkFormat Format, VkFormatProperties* pProps),
    (Real, Format, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceFormatProperties2,
    (VkPhysicalDevice PhysicalDevice, VkFormat Format, VkFormatProperties2* pProps),
    (Real, Format, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceFormatProperties2KHR,
    (VkPhysicalDevice PhysicalDevice, VkFormat Format, VkFormatProperties2* pProps),
    (Real, Format, pProps))

// vkGetPhysicalDeviceImageFormatProperties / 2 / 2KHR
UNWRAP_RESULT_CHAIN(GetPhysicalDeviceImageFormatProperties,
    (VkPhysicalDevice PhysicalDevice, VkFormat Format, VkImageType Type, VkImageTiling Tiling,
     VkImageUsageFlags Usage, VkImageCreateFlags Flags, VkImageFormatProperties* pProps),
    (Real, Format, Type, Tiling, Usage, Flags, pProps))
UNWRAP_RESULT_CHAIN(GetPhysicalDeviceImageFormatProperties2,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceImageFormatInfo2* pInfo,
     VkImageFormatProperties2* pProps),
    (Real, pInfo, pProps))
UNWRAP_RESULT_CHAIN(GetPhysicalDeviceImageFormatProperties2KHR,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceImageFormatInfo2* pInfo,
     VkImageFormatProperties2* pProps),
    (Real, pInfo, pProps))

// vkGetPhysicalDeviceSparseImageFormatProperties / 2 / 2KHR
UNWRAP_VOID_CHAIN(GetPhysicalDeviceSparseImageFormatProperties,
    (VkPhysicalDevice PhysicalDevice, VkFormat Format, VkImageType Type,
     VkSampleCountFlagBits Samples, VkImageUsageFlags Usage, VkImageTiling Tiling,
     uint32_t* pCount, VkSparseImageFormatProperties* pProps),
    (Real, Format, Type, Samples, Usage, Tiling, pCount, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceSparseImageFormatProperties2,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2* pInfo,
     uint32_t* pCount, VkSparseImageFormatProperties2* pProps),
    (Real, pInfo, pCount, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceSparseImageFormatProperties2KHR,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2* pInfo,
     uint32_t* pCount, VkSparseImageFormatProperties2* pProps),
    (Real, pInfo, pCount, pProps))

// External buffer / fence / semaphore properties
UNWRAP_VOID_CHAIN(GetPhysicalDeviceExternalBufferProperties,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceExternalBufferInfo* pInfo,
     VkExternalBufferProperties* pProps),
    (Real, pInfo, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceExternalBufferPropertiesKHR,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceExternalBufferInfo* pInfo,
     VkExternalBufferProperties* pProps),
    (Real, pInfo, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceExternalFenceProperties,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceExternalFenceInfo* pInfo,
     VkExternalFenceProperties* pProps),
    (Real, pInfo, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceExternalFencePropertiesKHR,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceExternalFenceInfo* pInfo,
     VkExternalFenceProperties* pProps),
    (Real, pInfo, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceExternalSemaphoreProperties,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo* pInfo,
     VkExternalSemaphoreProperties* pProps),
    (Real, pInfo, pProps))
UNWRAP_VOID_CHAIN(GetPhysicalDeviceExternalSemaphorePropertiesKHR,
    (VkPhysicalDevice PhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo* pInfo,
     VkExternalSemaphoreProperties* pProps),
    (Real, pInfo, pProps))

// vkEnumerateDeviceExtensionProperties / LayerProperties
UNWRAP_RESULT_CHAIN(EnumerateDeviceExtensionProperties,
    (VkPhysicalDevice PhysicalDevice, const char* pLayerName, uint32_t* pCount,
     VkExtensionProperties* pProps),
    (Real, pLayerName, pCount, pProps))
UNWRAP_RESULT_CHAIN(EnumerateDeviceLayerProperties,
    (VkPhysicalDevice PhysicalDevice, uint32_t* pCount, VkLayerProperties* pProps),
    (Real, pCount, pProps))

// VK_KHR_surface
UNWRAP_RESULT_CHAIN(GetPhysicalDeviceSurfaceSupportKHR,
    (VkPhysicalDevice PhysicalDevice, uint32_t QueueFamilyIndex, VkSurfaceKHR Surface, VkBool32* pSupported),
    (Real, QueueFamilyIndex, Surface, pSupported))
UNWRAP_RESULT_CHAIN(GetPhysicalDeviceSurfaceCapabilitiesKHR,
    (VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, VkSurfaceCapabilitiesKHR* pCaps),
    (Real, Surface, pCaps))
UNWRAP_RESULT_CHAIN(GetPhysicalDeviceSurfaceFormatsKHR,
    (VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, uint32_t* pCount, VkSurfaceFormatKHR* pFormats),
    (Real, Surface, pCount, pFormats))
UNWRAP_RESULT_CHAIN(GetPhysicalDeviceSurfacePresentModesKHR,
    (VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, uint32_t* pCount, VkPresentModeKHR* pModes),
    (Real, Surface, pCount, pModes))
#ifdef VK_USE_PLATFORM_WIN32_KHR
VKAPI_ATTR VkBool32 VKAPI_CALL Layer_GetPhysicalDeviceWin32PresentationSupportKHR(
    VkPhysicalDevice PhysicalDevice, uint32_t QueueFamilyIndex)
{
    VkPhysicalDevice Real = UnwrapOr(PhysicalDevice);
    InstanceData*    Inst = GetInstanceForPD(PhysicalDevice);
    if (Inst == nullptr || Inst->GetPhysicalDeviceWin32PresentationSupportKHR == nullptr)
        return VK_FALSE;
    return Inst->GetPhysicalDeviceWin32PresentationSupportKHR(Real, QueueFamilyIndex);
}
#endif

#undef UNWRAP_VOID_CHAIN
#undef UNWRAP_RESULT_CHAIN


// ---------------------------------------------------------------------------
// vkGetInstanceProcAddr / vkGetDeviceProcAddr router
// ---------------------------------------------------------------------------

#define ROUTE(name) \
    if (std::strcmp(pName, "vk" #name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(Layer_vk##name)

// Same routing helper for functions we intercept without the "vk" prefix on
// the layer function name (all our unwrap-chain helpers use Layer_<Name>).
#define ROUTE_PD(name) \
    if (std::strcmp(pName, "vk" #name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(Layer_##name)

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Layer_vkGetInstanceProcAddr(VkInstance Instance, const char* pName)
{
    if (pName == nullptr) return nullptr;

    // Loader/self-introspection.
    ROUTE(GetInstanceProcAddr);
    ROUTE(CreateInstance);
    ROUTE(DestroyInstance);
    ROUTE(EnumeratePhysicalDevices);
    ROUTE(EnumeratePhysicalDeviceGroups);
    ROUTE(EnumeratePhysicalDeviceGroupsKHR);
    ROUTE(CreateDevice);
    ROUTE(DestroyDevice);
    ROUTE(GetDeviceProcAddr);

    // Physical-device intercepts (with our own Name convention).
    ROUTE_PD(GetPhysicalDeviceProperties);
    ROUTE_PD(GetPhysicalDeviceProperties2);
    ROUTE_PD(GetPhysicalDeviceProperties2KHR);
    ROUTE_PD(GetPhysicalDeviceFeatures);
    ROUTE_PD(GetPhysicalDeviceFeatures2);
    ROUTE_PD(GetPhysicalDeviceFeatures2KHR);
    ROUTE_PD(GetPhysicalDeviceMemoryProperties);
    ROUTE_PD(GetPhysicalDeviceMemoryProperties2);
    ROUTE_PD(GetPhysicalDeviceMemoryProperties2KHR);
    ROUTE_PD(GetPhysicalDeviceQueueFamilyProperties);
    ROUTE_PD(GetPhysicalDeviceQueueFamilyProperties2);
    ROUTE_PD(GetPhysicalDeviceQueueFamilyProperties2KHR);
    ROUTE_PD(GetPhysicalDeviceFormatProperties);
    ROUTE_PD(GetPhysicalDeviceFormatProperties2);
    ROUTE_PD(GetPhysicalDeviceFormatProperties2KHR);
    ROUTE_PD(GetPhysicalDeviceImageFormatProperties);
    ROUTE_PD(GetPhysicalDeviceImageFormatProperties2);
    ROUTE_PD(GetPhysicalDeviceImageFormatProperties2KHR);
    ROUTE_PD(GetPhysicalDeviceSparseImageFormatProperties);
    ROUTE_PD(GetPhysicalDeviceSparseImageFormatProperties2);
    ROUTE_PD(GetPhysicalDeviceSparseImageFormatProperties2KHR);
    ROUTE_PD(GetPhysicalDeviceExternalBufferProperties);
    ROUTE_PD(GetPhysicalDeviceExternalBufferPropertiesKHR);
    ROUTE_PD(GetPhysicalDeviceExternalFenceProperties);
    ROUTE_PD(GetPhysicalDeviceExternalFencePropertiesKHR);
    ROUTE_PD(GetPhysicalDeviceExternalSemaphoreProperties);
    ROUTE_PD(GetPhysicalDeviceExternalSemaphorePropertiesKHR);
    ROUTE_PD(EnumerateDeviceExtensionProperties);
    ROUTE_PD(EnumerateDeviceLayerProperties);
    ROUTE_PD(GetPhysicalDeviceSurfaceSupportKHR);
    ROUTE_PD(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    ROUTE_PD(GetPhysicalDeviceSurfaceFormatsKHR);
    ROUTE_PD(GetPhysicalDeviceSurfacePresentModesKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    ROUTE_PD(GetPhysicalDeviceWin32PresentationSupportKHR);
#endif

    // Everything else - chain to the next layer.
    if (Instance == VK_NULL_HANDLE) return nullptr;
    InstanceData* pInst = FindInstance(Instance);
    if (pInst == nullptr || pInst->GetInstanceProcAddr == nullptr) return nullptr;
    return pInst->GetInstanceProcAddr(Instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Layer_vkGetDeviceProcAddr(VkDevice Device, const char* pName)
{
    if (pName == nullptr) return nullptr;

    ROUTE(GetDeviceProcAddr);
    ROUTE(DestroyDevice);

    if (Device == VK_NULL_HANDLE) return nullptr;
    DeviceData* pDev = FindDevice(Device);
    if (pDev == nullptr || pDev->GetDeviceProcAddr == nullptr) return nullptr;
    return pDev->GetDeviceProcAddr(Device, pName);
}

#undef ROUTE
#undef ROUTE_PD

} // namespace VkSim


// ---------------------------------------------------------------------------
// Loader-facing exports (see VkLayer_DiligentGpuSim.def)
// ---------------------------------------------------------------------------

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct)
{
    if (pVersionStruct == nullptr ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
        pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

    pVersionStruct->pfnGetInstanceProcAddr        = &VkSim::Layer_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr          = &VkSim::Layer_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr  = nullptr;

    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_layerGetInstanceProcAddr(VkInstance Instance, const char* pName)
{
    return VkSim::Layer_vkGetInstanceProcAddr(Instance, pName);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_layerGetDeviceProcAddr(VkDevice Device, const char* pName)
{
    return VkSim::Layer_vkGetDeviceProcAddr(Device, pName);
}


BOOL APIENTRY DllMain(HMODULE hModule, DWORD Reason, LPVOID /*lpReserved*/)
{
    switch (Reason)
    {
        case DLL_PROCESS_ATTACH:
            ::DisableThreadLibraryCalls(hModule);
            VkSim::LogInit();
            VkSim::LogInfo("VkLayer_DiligentGpuSim attached (PID=%lu, SimNodeCount=%u).",
                           ::GetCurrentProcessId(), VkSim::GetSimulatedNodeCount());
            break;
        case DLL_PROCESS_DETACH:
            VkSim::LogInfo("VkLayer_DiligentGpuSim detaching.");
            VkSim::ReleaseAllWrappers();
            VkSim::LogShutdown();
            break;
    }
    return TRUE;
}
