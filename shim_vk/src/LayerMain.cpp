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

#include <algorithm>
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

#include "LayerConfig.hpp"
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

static uint32_t GetSimulatedNodeCount()
{
    return GetConfig().NodeCount;
}

// LayerDevice.cpp
void               LoadDeviceFunctions(DeviceData& D);
PFN_vkVoidFunction GetDeviceGroupHook(const DeviceData& D, const char* pName);


std::mutex                                                g_Mutex;
std::unordered_map<VkInstance, InstanceData>              g_Instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>>    g_Devices; // by loader dispatch key

// Cache of vkEnumeratePhysicalDevices results per instance (real handles) so
// we can pick the primary when synthesizing groups.
std::unordered_map<VkInstance, std::vector<VkPhysicalDevice>> g_RealPhysicalDevices;

InstanceData* FindInstance(VkInstance Inst)
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    auto It = g_Instances.find(Inst);
    return It == g_Instances.end() ? nullptr : &It->second;
}

DeviceData* FindDeviceByKey(const void* Handle)
{
    if (Handle == nullptr)
        return nullptr;
    std::lock_guard<std::mutex> Lock(g_Mutex);
    auto It = g_Devices.find(GetDispatchKey(Handle));
    return It == g_Devices.end() ? nullptr : It->second.get();
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

VkPhysicalDevice GetHost(VkInstance Instance, InstanceData& Inst);

// Every device of a real group is also a physical device of its own (each GPU
// can be used alone), so the simulated devices 1..N-1 follow the host here
VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumeratePhysicalDevices(
    VkInstance                          Instance,
    uint32_t*                           pPhysicalDeviceCount,
    VkPhysicalDevice*                   pPhysicalDevices)
{
    InstanceData* pInst = FindInstance(Instance);
    if (pInst == nullptr || pInst->EnumeratePhysicalDevices == nullptr || pPhysicalDeviceCount == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkPhysicalDevice        Host = GetHost(Instance, *pInst);
    std::vector<VkPhysicalDevice> All;
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        for (VkPhysicalDevice Pd : g_RealPhysicalDevices[Instance])
        {
            All.push_back(Pd);
            if (Pd == Host)
                All.insert(All.end(), pInst->NodeWrappers.begin(), pInst->NodeWrappers.end());
        }
    }
    if (pPhysicalDevices == nullptr)
    {
        *pPhysicalDeviceCount = static_cast<uint32_t>(All.size());
        return VK_SUCCESS;
    }
    const uint32_t Written = (std::min)(*pPhysicalDeviceCount, static_cast<uint32_t>(All.size()));
    std::copy(All.begin(), All.begin() + Written, pPhysicalDevices);
    *pPhysicalDeviceCount = Written;
    return Written < All.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// The physical device the simulated group is built on: the adapter whose LUID
// SimulationApp passed (the one chosen as host), else the first one.
VkPhysicalDevice GetHost(VkInstance Instance, InstanceData& Inst)
{
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        if (Inst.HostResolved)
            return Inst.Host;
    }
    uint32_t Count = 0;
    Inst.EnumeratePhysicalDevices(Instance, &Count, nullptr);
    std::vector<VkPhysicalDevice> Devices(Count);
    if (Count > 0)
        Inst.EnumeratePhysicalDevices(Instance, &Count, Devices.data());
    Devices.resize(Count);

    VkPhysicalDevice Host     = Devices.empty() ? VK_NULL_HANDLE : Devices.front();
    const auto&      Cfg      = GetConfig();
    auto             GetProps = Inst.GetPhysicalDeviceProperties2 ? Inst.GetPhysicalDeviceProperties2 : Inst.GetPhysicalDeviceProperties2KHR;
    if (Cfg.HasHostLuid && GetProps != nullptr)
    {
        bool Found = false;
        for (VkPhysicalDevice Pd : Devices)
        {
            VkPhysicalDeviceIDProperties Id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2  Props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &Id};
            GetProps(Pd, &Props);
            if (Id.deviceLUIDValid && std::memcmp(Id.deviceLUID, Cfg.HostLuid, VK_LUID_SIZE) == 0)
            {
                Host  = Pd;
                Found = true;
                break;
            }
        }
        if (!Found)
            LogWarn("No Vulkan physical device has the host adapter LUID; the first one hosts the simulated group");
    }

    std::lock_guard<std::mutex> Lock(g_Mutex);
    if (!Inst.HostResolved)
    {
        Inst.Host = Host;
        // Wrappers for nodes 1..N-1, created once so that every enumeration returns the same handles
        const uint32_t N = (std::min)(GetSimulatedNodeCount(), static_cast<uint32_t>(VK_MAX_DEVICE_GROUP_SIZE));
        for (uint32_t k = 1; Host != VK_NULL_HANDLE && k < N; ++k)
            Inst.NodeWrappers.push_back(WrapPhysicalDevice(Host, Instance, k, N));
        Inst.HostResolved = true;
        g_RealPhysicalDevices[Instance] = Devices;
    }
    return Inst.Host;
}

// Every physical device is in exactly one group. The host's group becomes the
// simulated group [host, node 1, ...]; the groups of the other devices are
// returned unchanged.
VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumeratePhysicalDeviceGroups(
    VkInstance                          Instance,
    uint32_t*                           pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*    pPhysicalDeviceGroupProperties)
{
    if (pPhysicalDeviceGroupCount == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    InstanceData* pInst = FindInstance(Instance);
    if (pInst == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    auto pfnReal = pInst->EnumeratePhysicalDeviceGroups
                       ? pInst->EnumeratePhysicalDeviceGroups
                       : pInst->EnumeratePhysicalDeviceGroupsKHR;
    if (pfnReal == nullptr)
        return VK_ERROR_EXTENSION_NOT_PRESENT;

    const VkPhysicalDevice Host = GetHost(Instance, *pInst);

    uint32_t RealCount = 0;
    VkResult r         = pfnReal(Instance, &RealCount, nullptr);
    if (r != VK_SUCCESS)
        return r;
    std::vector<VkPhysicalDeviceGroupProperties> Real(RealCount, VkPhysicalDeviceGroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES});
    r = pfnReal(Instance, &RealCount, Real.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return r;
    Real.resize(RealCount);

    for (auto& G : Real)
    {
        if (G.physicalDeviceCount == 1 && G.physicalDevices[0] == Host && !pInst->NodeWrappers.empty())
        {
            // Index 0 stays the real handle, so pointer comparisons with the result of
            // vkEnumeratePhysicalDevices (Diligent's adapter matching) still work
            for (size_t k = 0; k < pInst->NodeWrappers.size(); ++k)
                G.physicalDevices[k + 1] = pInst->NodeWrappers[k];
            G.physicalDeviceCount = static_cast<uint32_t>(pInst->NodeWrappers.size() + 1);
            G.subsetAllocation    = GetConfig().SubsetAllocation ? VK_TRUE : VK_FALSE;
        }
    }

    if (pPhysicalDeviceGroupProperties == nullptr)
    {
        *pPhysicalDeviceGroupCount = static_cast<uint32_t>(Real.size());
        return VK_SUCCESS;
    }
    const uint32_t Written = (std::min)(*pPhysicalDeviceGroupCount, static_cast<uint32_t>(Real.size()));
    for (uint32_t i = 0; i < Written; ++i)
    {
        // Keep the caller's sType/pNext
        VkPhysicalDeviceGroupProperties& G = pPhysicalDeviceGroupProperties[i];
        G.physicalDeviceCount              = Real[i].physicalDeviceCount;
        std::memcpy(G.physicalDevices, Real[i].physicalDevices, sizeof(G.physicalDevices));
        G.subsetAllocation = Real[i].subsetAllocation;
    }
    *pPhysicalDeviceGroupCount = Written;
    LogVerbose("vkEnumeratePhysicalDeviceGroups: %u group(s); host %p in a simulated group of %u devices",
               Written, static_cast<void*>(Host), static_cast<unsigned>(pInst->NodeWrappers.size() + 1));
    return Written < Real.size() ? VK_INCOMPLETE : VK_SUCCESS;
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

    VkPhysicalDevice RealPhys = UnwrapOr(PhysicalDevice);

    // A device of the simulated group: VkDeviceGroupDeviceCreateInfo lists the
    // host and wrappers of its other nodes. The driver gets an ordinary device
    // (physicalDeviceCount 1); LayerDevice.cpp maps the group's device masks and
    // indices onto it. The structure is const caller memory that other
    // structures may point to, so it is patched in place and restored.
    const VkDeviceGroupDeviceCreateInfo* pGroup = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); p != nullptr; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)
            pGroup = reinterpret_cast<const VkDeviceGroupDeviceCreateInfo*>(p);

    uint32_t SimNodeCount = 1;
    if (pGroup != nullptr && pGroup->physicalDeviceCount > 1 && pGroup->pPhysicalDevices != nullptr)
    {
        bool AllSimulated = true;
        for (uint32_t i = 0; i < pGroup->physicalDeviceCount; ++i)
            AllSimulated = AllSimulated && UnwrapOr(pGroup->pPhysicalDevices[i]) == RealPhys;
        if (AllSimulated)
        {
            if (GetConfig().Validate)
            {
                for (uint32_t i = 0; i < pGroup->physicalDeviceCount; ++i)
                    for (uint32_t j = i + 1; j < pGroup->physicalDeviceCount; ++j)
                        if (pGroup->pPhysicalDevices[i] == pGroup->pPhysicalDevices[j])
                            ReportValidationError("vkCreateDevice: VkDeviceGroupDeviceCreateInfo lists physical device %u twice (VUID-VkDeviceGroupDeviceCreateInfo-pPhysicalDevices-00375)", i);
            }
            SimNodeCount = pGroup->physicalDeviceCount;
        }
    }

    auto* pGroupMutable = const_cast<VkDeviceGroupDeviceCreateInfo*>(pGroup);
    const uint32_t                SavedCount   = pGroup != nullptr ? pGroup->physicalDeviceCount : 0;
    const VkPhysicalDevice* const SavedDevices = pGroup != nullptr ? pGroup->pPhysicalDevices : nullptr;
    if (SimNodeCount > 1)
    {
        pGroupMutable->physicalDeviceCount = 1;
        pGroupMutable->pPhysicalDevices    = &RealPhys;
    }
    const VkResult r = pfnNextCreateDevice(RealPhys, pCreateInfo, pAllocator, pDevice);
    if (SimNodeCount > 1)
    {
        pGroupMutable->physicalDeviceCount = SavedCount;
        pGroupMutable->pPhysicalDevices    = SavedDevices;
    }
    if (r != VK_SUCCESS)
    {
        LogError("vkCreateDevice failed: %d", static_cast<int>(r));
        return r;
    }

    auto dd               = std::make_unique<DeviceData>();
    dd->Device            = *pDevice;
    dd->GetDeviceProcAddr = pfnNextGdpa;
    dd->DestroyDevice     = reinterpret_cast<PFN_vkDestroyDevice>(pfnNextGdpa(*pDevice, "vkDestroyDevice"));
    dd->NodeCount         = SimNodeCount;
    if (SimNodeCount > 1)
    {
        LoadDeviceFunctions(*dd);
        // Device-local heaps have one instance per device of a real group
        if (InstanceData* pInst = GetInstanceForPD(PhysicalDevice); pInst != nullptr && pInst->GetPhysicalDeviceMemoryProperties != nullptr)
        {
            VkPhysicalDeviceMemoryProperties Mem{};
            pInst->GetPhysicalDeviceMemoryProperties(RealPhys, &Mem);
            for (uint32_t i = 0; i < Mem.memoryTypeCount; ++i)
            {
                dd->MultiInstanceType.push_back((Mem.memoryHeaps[Mem.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0);
                dd->TypeHeap.push_back(Mem.memoryTypes[i].heapIndex);
            }
            for (uint32_t h = 0; h < Mem.memoryHeapCount; ++h)
                dd->DeviceLocalHeap.push_back((Mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0);
        }
        dd->Model = std::make_unique<MemoryModel>(); // memory instances of the group (LayerMemoryModel.hpp)
    }
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        g_Devices[GetDispatchKey(*pDevice)] = std::move(dd);
    }
    if (SimNodeCount > 1)
        LogInfo("vkCreateDevice: device %p is a simulated group of %u devices on physical device %p.", *pDevice, SimNodeCount, RealPhys);
    else
        LogVerbose("vkCreateDevice: device %p (physical %p), not simulated.", *pDevice, RealPhys);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Layer_vkDestroyDevice(VkDevice Device, const VkAllocationCallbacks* pAllocator)
{
    std::unique_ptr<DeviceData> dd;
    {
        std::lock_guard<std::mutex> Lock(g_Mutex);
        auto It = g_Devices.find(GetDispatchKey(Device));
        if (It != g_Devices.end())
        {
            dd = std::move(It->second);
            g_Devices.erase(It);
        }
    }
    LogVerbose("vkDestroyDevice: %p", Device);
    if (dd && dd->DestroyDevice)
        dd->DestroyDevice(Device, pAllocator);
}

// ---------------------------------------------------------------------------
// vkGetPhysicalDeviceProperties[2] - unwrap + append "[Simulated Node k]"
// ---------------------------------------------------------------------------

static bool IsSimulatedGroupMember(VkPhysicalDevice Pd, const InstanceData& Inst);

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
    if (w != nullptr && pProperties != nullptr && GetConfig().VirtualIdentities)
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

    if (pProperties == nullptr)
        return;
    if (w != nullptr && GetConfig().VirtualIdentities)
        ApplyNodeSuffixToDeviceName(pProperties->properties.deviceName, w->NodeIndex);
    // A linked adapter: the devices share the adapter LUID and tell their node by deviceNodeMask;
    // as separate GPUs they have distinct UUIDs
    if (IsSimulatedGroupMember(PhysicalDevice, *Inst))
    {
        const uint32_t NodeIndex = w != nullptr ? w->NodeIndex : 0;
        auto           PatchIds  = [&](uint8_t* pUuid, uint32_t& NodeMask, VkBool32 LuidValid) {
            if (LuidValid)
                NodeMask = 1u << NodeIndex;
            if (NodeIndex != 0)
                pUuid[VK_UUID_SIZE - 1] ^= static_cast<uint8_t>(0x80u | NodeIndex);
        };
        for (auto* p = static_cast<VkBaseOutStructure*>(pProperties->pNext); p != nullptr; p = p->pNext)
        {
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES)
            {
                auto* pId = reinterpret_cast<VkPhysicalDeviceIDProperties*>(p);
                PatchIds(pId->deviceUUID, pId->deviceNodeMask, pId->deviceLUIDValid);
            }
            else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES)
            {
                auto* p11 = reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(p);
                PatchIds(p11->deviceUUID, p11->deviceNodeMask, p11->deviceLUIDValid);
            }
        }
    }
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

// In a logical device made of several physical devices, device-local heaps have
// one instance per device; real group-capable drivers report that on every
// member, and allocations from such heaps cannot be mapped with several instances
static void MarkMultiInstanceHeaps(VkPhysicalDeviceMemoryProperties* pMem)
{
    for (uint32_t i = 0; pMem != nullptr && i < pMem->memoryHeapCount; ++i)
        if (pMem->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            pMem->memoryHeaps[i].flags |= VK_MEMORY_HEAP_MULTI_INSTANCE_BIT;
}

static bool IsSimulatedGroupMember(VkPhysicalDevice Pd, const InstanceData& Inst)
{
    if (TryUnwrap(Pd) != nullptr)
        return true;
    std::lock_guard<std::mutex> Lock(g_Mutex);
    return Inst.HostResolved && Pd == Inst.Host && !Inst.NodeWrappers.empty();
}

VKAPI_ATTR void VKAPI_CALL Layer_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceMemoryProperties* pMem)
{
    WrappedPhysicalDevice* w    = TryUnwrap(PhysicalDevice);
    VkPhysicalDevice       Real = w ? w->Real : PhysicalDevice;
    InstanceData*          Inst = GetInstanceForPD(PhysicalDevice);
    if (Inst == nullptr || Inst->GetPhysicalDeviceMemoryProperties == nullptr) return;

    Inst->GetPhysicalDeviceMemoryProperties(Real, pMem);
    if (w != nullptr && GetConfig().VirtualIdentities) SplitMemoryHeapSizes(pMem, w->NodeCount);
    if (IsSimulatedGroupMember(PhysicalDevice, *Inst)) MarkMultiInstanceHeaps(pMem);
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

    if (w != nullptr && pMem != nullptr && GetConfig().VirtualIdentities)
        SplitMemoryHeapSizes(&pMem->memoryProperties, w->NodeCount);
    if (pMem != nullptr && IsSimulatedGroupMember(PhysicalDevice, *Inst))
        MarkMultiInstanceHeaps(&pMem->memoryProperties);
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
    DeviceData* pDev = FindDeviceByKey(Device);
    if (pDev == nullptr || pDev->GetDeviceProcAddr == nullptr) return nullptr;
    // Devices of the simulated group: device masks and indices are mapped (LayerDevice.cpp)
    if (pDev->NodeCount > 1)
        if (PFN_vkVoidFunction pfn = GetDeviceGroupHook(*pDev, pName))
            return pfn;
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
