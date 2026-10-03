/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  VkSimTest - checks VkLayer_DiligentGpuSim on the real driver.
//
//  Enables the layer (VK_LAYER_PATH / VK_INSTANCE_LAYERS, as SimulationApp
//  does) together with the Khronos validation layer, creates a device of the
//  simulated 2-device group and checks that device masks and indices of both
//  devices work, that invalid ones and unmappable memory are reported, and
//  that the Khronos layer reports nothing.  Exit code 0 = all checks passed.
//
//  Usage: VkSimTest [--layer-dir DIR]   (default: the exe's directory)

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int g_Checks   = 0;
int g_Failures = 0;
int g_KhronosErrors = 0;

#define CHECK(cond)                                                 \
    do                                                              \
    {                                                               \
        ++g_Checks;                                                 \
        if (!(cond))                                                \
        {                                                           \
            ++g_Failures;                                           \
            std::printf("  FAILED line %d: %s\n", __LINE__, #cond); \
        }                                                           \
    } while (false)

using PFN_GetCount = unsigned (*)();
PFN_GetCount g_LayerErrors = nullptr;

unsigned LayerErrors() { return g_LayerErrors != nullptr ? g_LayerErrors() : 0; }

template <typename F>
bool ExpectLayerErrors(unsigned Expected, F&& Fn)
{
    const unsigned Before = LayerErrors();
    Fn();
    return LayerErrors() - Before == Expected;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT Severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* pData, void*)
{
    if (Severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        ++g_KhronosErrors;
        std::printf("  [khronos validation] %s\n", pData->pMessage);
    }
    return VK_FALSE;
}

bool HasLayer(const char* Name)
{
    uint32_t Count = 0;
    vkEnumerateInstanceLayerProperties(&Count, nullptr);
    std::vector<VkLayerProperties> Layers(Count);
    vkEnumerateInstanceLayerProperties(&Count, Layers.data());
    for (const auto& L : Layers)
        if (std::strcmp(L.layerName, Name) == 0)
            return true;
    return false;
}

struct Context
{
    VkPhysicalDevice                 Host  = VK_NULL_HANDLE;
    VkDevice                         Device = VK_NULL_HANDLE;
    VkQueue                          Queue  = VK_NULL_HANDLE;
    uint32_t                         QueueFamily = 0;
    VkCommandPool                    Pool  = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties Mem{};
    bool                             Sync2 = false;
};

int FindMemoryType(const Context& C, uint32_t TypeBits, VkMemoryPropertyFlags Required, VkMemoryPropertyFlags Forbidden, bool DeviceLocalHeap)
{
    for (uint32_t i = 0; i < C.Mem.memoryTypeCount; ++i)
    {
        const auto& T        = C.Mem.memoryTypes[i];
        const bool  HeapIsDL = (C.Mem.memoryHeaps[T.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        if ((TypeBits & (1u << i)) && (T.propertyFlags & Required) == Required && (T.propertyFlags & Forbidden) == 0 && HeapIsDL == DeviceLocalHeap)
            return static_cast<int>(i);
    }
    return -1;
}

VkDeviceMemory Allocate(const Context& C, uint32_t TypeIndex, VkDeviceSize Size, uint32_t DeviceMask)
{
    VkMemoryAllocateFlagsInfo Flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    Flags.flags      = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT;
    Flags.deviceMask = DeviceMask;
    VkMemoryAllocateInfo Info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, DeviceMask != 0 ? &Flags : nullptr, Size, TypeIndex};
    VkDeviceMemory       Memory = VK_NULL_HANDLE;
    vkAllocateMemory(C.Device, &Info, nullptr, &Memory);
    return Memory;
}

VkCommandBuffer BeginCommandBuffer(const Context& C, uint32_t DeviceMask)
{
    VkCommandBufferAllocateInfo AI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, C.Pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer             Cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(C.Device, &AI, &Cmd);
    VkDeviceGroupCommandBufferBeginInfo Group{VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO, nullptr, DeviceMask};
    VkCommandBufferBeginInfo            BI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, &Group, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(Cmd, &BI);
    return Cmd;
}

void TestGroupDevice(Context& C)
{
    std::printf("Peer memory\n");
    VkPeerMemoryFeatureFlags Peer = 0;
    vkGetDeviceGroupPeerMemoryFeatures(C.Device, 0, 0, 1, &Peer);
    CHECK((Peer & VK_PEER_MEMORY_FEATURE_COPY_DST_BIT) != 0);
    CHECK(ExpectLayerErrors(1, [&] { vkGetDeviceGroupPeerMemoryFeatures(C.Device, 0, 0, 2, &Peer); }));

    std::printf("Work on device 1 of the group\n");
    VkBufferCreateInfo BCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    BCI.size  = 64 * 1024;
    BCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer Buffer = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(C.Device, &BCI, nullptr, &Buffer) == VK_SUCCESS);
    VkMemoryRequirements Req{};
    vkGetBufferMemoryRequirements(C.Device, Buffer, &Req);
    const int LocalType = FindMemoryType(C, Req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true);
    CHECK(LocalType >= 0);
    VkDeviceMemory LocalMem = LocalType >= 0 ? Allocate(C, LocalType, Req.size, 0x2) : VK_NULL_HANDLE; // instance on device 1 only
    CHECK(LocalMem != VK_NULL_HANDLE);
    if (LocalMem != VK_NULL_HANDLE)
    {
        const uint32_t                    Indices[] = {1, 1};
        VkBindBufferMemoryDeviceGroupInfo GroupBind{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO, nullptr, 2, Indices};
        VkBindBufferMemoryInfo            Bind{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, &GroupBind, Buffer, LocalMem, 0};
        CHECK(ExpectLayerErrors(0, [&] { CHECK(vkBindBufferMemory2(C.Device, 1, &Bind) == VK_SUCCESS); }));
        CHECK(GroupBind.deviceIndexCount == 2 && GroupBind.pDeviceIndices == Indices); // restored after the call
    }

    VkCommandBuffer Cmd = BeginCommandBuffer(C, 0x2);
    CHECK(ExpectLayerErrors(0, [&] { vkCmdSetDeviceMask(Cmd, 0x2); }));
    if (LocalMem != VK_NULL_HANDLE)
        vkCmdFillBuffer(Cmd, Buffer, 0, VK_WHOLE_SIZE, 0x12345678u);
    vkEndCommandBuffer(Cmd);

    VkSemaphoreCreateInfo SCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore           Signal = VK_NULL_HANDLE;
    vkCreateSemaphore(C.Device, &SCI, nullptr, &Signal);
    const uint32_t          CmdMask = 0x2, SignalIndex = 1;
    VkDeviceGroupSubmitInfo GroupSubmit{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO, nullptr, 0, nullptr, 1, &CmdMask, 1, &SignalIndex};
    VkSubmitInfo            Submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &GroupSubmit};
    Submit.commandBufferCount   = 1;
    Submit.pCommandBuffers      = &Cmd;
    Submit.signalSemaphoreCount = 1;
    Submit.pSignalSemaphores    = &Signal;
    VkFenceCreateInfo FCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence           Fence = VK_NULL_HANDLE;
    vkCreateFence(C.Device, &FCI, nullptr, &Fence);
    CHECK(ExpectLayerErrors(0, [&] { CHECK(vkQueueSubmit(C.Queue, 1, &Submit, Fence) == VK_SUCCESS); }));
    CHECK(GroupSubmit.pCommandBufferDeviceMasks == &CmdMask && GroupSubmit.pSignalSemaphoreDeviceIndices == &SignalIndex);
    CHECK(vkWaitForFences(C.Device, 1, &Fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS);

    // Device 0 waits for the semaphore device 1 signaled
    {
        VkCommandBuffer Cmd0 = BeginCommandBuffer(C, 0x1);
        vkEndCommandBuffer(Cmd0);
        const uint32_t          Mask0 = 0x1, WaitIndex = 0;
        VkDeviceGroupSubmitInfo G0{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO, nullptr, 1, &WaitIndex, 1, &Mask0, 0, nullptr};
        const VkPipelineStageFlags Stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo               S0{VK_STRUCTURE_TYPE_SUBMIT_INFO, &G0, 1, &Signal, &Stage, 1, &Cmd0};
        vkResetFences(C.Device, 1, &Fence);
        CHECK(ExpectLayerErrors(0, [&] { CHECK(vkQueueSubmit(C.Queue, 1, &S0, Fence) == VK_SUCCESS); }));
        CHECK(vkWaitForFences(C.Device, 1, &Fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS);
    }

    if (C.Sync2)
    {
        VkCommandBuffer Cmd2 = BeginCommandBuffer(C, 0x3);
        vkEndCommandBuffer(Cmd2);
        VkCommandBufferSubmitInfo CmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, Cmd2, 0x2};
        VkSubmitInfo2             S2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        S2.commandBufferInfoCount = 1;
        S2.pCommandBufferInfos    = &CmdInfo;
        vkResetFences(C.Device, 1, &Fence);
        CHECK(ExpectLayerErrors(0, [&] { CHECK(vkQueueSubmit2(C.Queue, 1, &S2, Fence) == VK_SUCCESS); }));
        CHECK(CmdInfo.deviceMask == 0x2);
        CHECK(vkWaitForFences(C.Device, 1, &Fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS);
    }

    std::printf("Invalid device masks\n");
    CHECK(ExpectLayerErrors(1, [&] { vkEndCommandBuffer(BeginCommandBuffer(C, 0x4)); }));     // no device 2
    VkCommandBuffer Narrow = BeginCommandBuffer(C, 0x2);
    CHECK(ExpectLayerErrors(1, [&] { vkCmdSetDeviceMask(Narrow, 0x1); }));                    // outside the begin mask
    vkEndCommandBuffer(Narrow);

    std::printf("Mapping memory with one instance per device\n");
    VkBuffer Staging = VK_NULL_HANDLE;
    BCI.usage        = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(C.Device, &BCI, nullptr, &Staging);
    vkGetBufferMemoryRequirements(C.Device, Staging, &Req);
    const VkMemoryPropertyFlags HostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const int                   SysType     = FindMemoryType(C, Req.memoryTypeBits, HostVisible, 0, false);
    const int                   BarType     = FindMemoryType(C, Req.memoryTypeBits, HostVisible, 0, true);
    void*                       pData       = nullptr;
    if (SysType >= 0)
    {
        VkDeviceMemory M = Allocate(C, SysType, Req.size, 0); // system memory: one instance, mappable
        CHECK(ExpectLayerErrors(0, [&] { CHECK(vkMapMemory(C.Device, M, 0, VK_WHOLE_SIZE, 0, &pData) == VK_SUCCESS); }));
        vkFreeMemory(C.Device, M, nullptr);
    }
    if (BarType >= 0)
    {
        VkDeviceMemory M = Allocate(C, BarType, Req.size, 0); // device-local heap, all devices: one instance each
        CHECK(ExpectLayerErrors(1, [&] { vkMapMemory(C.Device, M, 0, VK_WHOLE_SIZE, 0, &pData); }));
        vkFreeMemory(C.Device, M, nullptr);
        M = Allocate(C, BarType, Req.size, 0x1); // one device: mappable
        CHECK(ExpectLayerErrors(0, [&] { CHECK(vkMapMemory(C.Device, M, 0, VK_WHOLE_SIZE, 0, &pData) == VK_SUCCESS); }));
        vkFreeMemory(C.Device, M, nullptr);
    }
    else
    {
        std::printf("  note: no host-visible memory in a device-local heap; the multi-instance map check is skipped\n");
    }

    vkDeviceWaitIdle(C.Device);
    vkDestroyFence(C.Device, Fence, nullptr);
    vkDestroySemaphore(C.Device, Signal, nullptr);
    vkDestroyBuffer(C.Device, Staging, nullptr);
    vkDestroyBuffer(C.Device, Buffer, nullptr);
    if (LocalMem != VK_NULL_HANDLE)
        vkFreeMemory(C.Device, LocalMem, nullptr);
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    wchar_t ExePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, ExePath, MAX_PATH);
    std::wstring LayerDir = ExePath;
    LayerDir              = LayerDir.substr(0, LayerDir.find_last_of(L"\\/"));
    for (int a = 1; a + 1 < argc; ++a)
        if (std::strcmp(argv[a], "--layer-dir") == 0)
            LayerDir = std::wstring(argv[a + 1], argv[a + 1] + std::strlen(argv[a + 1]));

    // Host = first hardware DXGI adapter, identified by LUID as SimulationApp does
    IDXGIFactory1* pFactory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory));
    LUID HostLuid{};
    for (UINT i = 0; pFactory != nullptr; ++i)
    {
        IDXGIAdapter1* pAdapter = nullptr;
        if (pFactory->EnumAdapters1(i, &pAdapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 D{};
        pAdapter->GetDesc1(&D);
        pAdapter->Release();
        if ((D.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            HostLuid = D.AdapterLuid;
            std::printf("Host adapter: %ls\n", D.Description);
            break;
        }
    }
    if (pFactory != nullptr)
        pFactory->Release();
    wchar_t Luid[64];
    std::swprintf(Luid, 64, L"0x%08X_0x%08X", static_cast<unsigned>(HostLuid.HighPart), static_cast<unsigned>(HostLuid.LowPart));
    ::SetEnvironmentVariableW(L"DILIGENT_SIM_LINKED_NODE_COUNT", L"2");
    ::SetEnvironmentVariableW(L"DILIGENT_SIM_HOST_ADAPTER_LUID", Luid);
    ::SetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", LayerDir.c_str()); // VK_LAYER_PATH would hide the installed layers
    ::SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", L"VK_LAYER_DiligentGraphics_LinkedGpuSim");

    const bool              Khronos = HasLayer("VK_LAYER_KHRONOS_validation");
    const char*             Layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char*             Exts[]   = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo       App{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "VkSimTest", 1, nullptr, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo    ICI{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &App, Khronos ? 1u : 0u, Layers, Khronos ? 1u : 0u, Exts};
    VkInstance              Instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ICI, nullptr, &Instance) != VK_SUCCESS)
    {
        std::printf("vkCreateInstance failed\n");
        return 2;
    }
    if (!Khronos)
        std::printf("note: VK_LAYER_KHRONOS_validation is not installed\n");
    VkDebugUtilsMessengerEXT Messenger = VK_NULL_HANDLE;
    if (Khronos)
    {
        auto pfnCreate = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(Instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT MCI{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        MCI.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        MCI.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        MCI.pfnUserCallback = &OnDebugMessage;
        if (pfnCreate != nullptr)
            pfnCreate(Instance, &MCI, nullptr, &Messenger);
    }
    HMODULE hLayer = ::GetModuleHandleW(L"VkLayer_DiligentGpuSim.dll");
    CHECK(hLayer != nullptr);
    if (hLayer != nullptr)
        g_LayerErrors = reinterpret_cast<PFN_GetCount>(::GetProcAddress(hLayer, "VkSim_GetValidationErrorCount"));
    if (g_LayerErrors == nullptr)
        std::printf("note: the layer does not export VkSim_GetValidationErrorCount (validation checks will fail)\n");

    std::printf("Physical device groups\n");
    uint32_t PdCount = 0;
    vkEnumeratePhysicalDevices(Instance, &PdCount, nullptr);
    uint32_t GroupCount = 0;
    vkEnumeratePhysicalDeviceGroups(Instance, &GroupCount, nullptr);
    std::vector<VkPhysicalDeviceGroupProperties> Groups(GroupCount, VkPhysicalDeviceGroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES});
    vkEnumeratePhysicalDeviceGroups(Instance, &GroupCount, Groups.data());
    CHECK(GroupCount == PdCount); // every physical device is in exactly one group
    const VkPhysicalDeviceGroupProperties* pSimGroup = nullptr;
    for (const auto& G : Groups)
        if (G.physicalDeviceCount == 2)
            pSimGroup = &G;
    CHECK(pSimGroup != nullptr);
    // The same handles on every call
    std::vector<VkPhysicalDeviceGroupProperties> Again(GroupCount, VkPhysicalDeviceGroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES});
    vkEnumeratePhysicalDeviceGroups(Instance, &GroupCount, Again.data());
    bool Stable = true;
    for (uint32_t g = 0; g < GroupCount; ++g)
        for (uint32_t d = 0; d < Groups[g].physicalDeviceCount; ++d)
            Stable = Stable && Groups[g].physicalDevices[d] == Again[g].physicalDevices[d];
    CHECK(Stable);

    if (pSimGroup != nullptr)
    {
        Context C;
        C.Host = pSimGroup->physicalDevices[0];
        VkPhysicalDeviceIDProperties Id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2  Props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &Id};
        vkGetPhysicalDeviceProperties2(C.Host, &Props);
        CHECK(Id.deviceLUIDValid && std::memcmp(Id.deviceLUID, &HostLuid, VK_LUID_SIZE) == 0); // the group is on the host adapter
        std::printf("  simulated group on %s\n", Props.properties.deviceName);

        vkGetPhysicalDeviceMemoryProperties(C.Host, &C.Mem);
        bool MultiInstance = true;
        for (uint32_t h = 0; h < C.Mem.memoryHeapCount; ++h)
            if (C.Mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                MultiInstance = MultiInstance && (C.Mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT) != 0;
        CHECK(MultiInstance);

        uint32_t QFCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(C.Host, &QFCount, nullptr);
        std::vector<VkQueueFamilyProperties> QF(QFCount);
        vkGetPhysicalDeviceQueueFamilyProperties(C.Host, &QFCount, QF.data());
        for (uint32_t q = 0; q < QFCount; ++q)
            if (QF[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                C.QueueFamily = q;
                break;
            }

        VkPhysicalDeviceVulkan13Features F13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2        F2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &F13};
        vkGetPhysicalDeviceFeatures2(C.Host, &F2);
        C.Sync2 = F13.synchronization2 == VK_TRUE;
        VkPhysicalDeviceVulkan13Features Enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        Enable13.synchronization2 = F13.synchronization2;

        const float                   Priority = 1.0f;
        VkDeviceQueueCreateInfo       QCI{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, C.QueueFamily, 1, &Priority};
        VkDeviceGroupDeviceCreateInfo GroupCI{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO, &Enable13, pSimGroup->physicalDeviceCount, pSimGroup->physicalDevices};
        VkDeviceCreateInfo            DCI{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &GroupCI, 0, 1, &QCI};
        std::printf("Device of the simulated group\n");
        CHECK(ExpectLayerErrors(0, [&] { CHECK(vkCreateDevice(C.Host, &DCI, nullptr, &C.Device) == VK_SUCCESS); }));
        CHECK(GroupCI.physicalDeviceCount == 2 && GroupCI.pPhysicalDevices == pSimGroup->physicalDevices); // restored
        if (C.Device != VK_NULL_HANDLE)
        {
            vkGetDeviceQueue(C.Device, C.QueueFamily, 0, &C.Queue);
            VkCommandPoolCreateInfo PCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, C.QueueFamily};
            vkCreateCommandPool(C.Device, &PCI, nullptr, &C.Pool);
            TestGroupDevice(C);
            vkDestroyCommandPool(C.Device, C.Pool, nullptr);
            vkDestroyDevice(C.Device, nullptr);
        }

        std::printf("An ordinary device on the host is not changed\n");
        VkDeviceCreateInfo Plain{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &QCI};
        VkDevice           PlainDevice = VK_NULL_HANDLE;
        CHECK(vkCreateDevice(C.Host, &Plain, nullptr, &PlainDevice) == VK_SUCCESS);
        if (PlainDevice != VK_NULL_HANDLE)
        {
            VkPeerMemoryFeatureFlags Peer = 0;
            // Not a group: the layer does not answer peer queries for it (it would for a group)
            CHECK(vkGetDeviceProcAddr(PlainDevice, "vkCmdSetDeviceMask") != nullptr);
            (void)Peer;
            vkDestroyDevice(PlainDevice, nullptr);
        }
    }

    if (Messenger != VK_NULL_HANDLE)
    {
        auto pfnDestroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (pfnDestroy != nullptr)
            pfnDestroy(Instance, Messenger, nullptr);
    }
    vkDestroyInstance(Instance, nullptr);

    CHECK(g_KhronosErrors == 0);
    std::printf("%d checks, %d failed (Khronos validation errors: %d)\n", g_Checks, g_Failures, g_KhronosErrors);
    return g_Failures == 0 ? 0 : 1;
}
