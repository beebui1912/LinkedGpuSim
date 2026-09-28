/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "D3D12DeviceWrapper.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "D3D12Vtable.hpp"
#include "ShimConfig.hpp"
#include "ShimLog.hpp"

namespace D3D12Sim
{

// ---------------------------------------------------------------------------
// Per-orig-vtable shadow.
//
// D3D12CreateDevice can return devices with DIFFERENT vtables in the same
// process (e.g. the D3D12 debug layer in d3d12sdklayers.dll wraps the real
// device in its own class with its own vtable).  So we cannot assume a
// single "canonical" orig vtable.  Instead, we cache one shadow per unique
// orig vtable pointer we observe.
//
// Each shadow is laid out as [ orig vtbl* | slot 0 | slot 1 | ... ] so that
// the hook stubs can find the correct orig vtable through the object's own
// vtable pointer with a simple [-1] indirection:
//
//   void** ourVtbl = *(void***)pDevice;           // -> Shadow.Slots
//   void** origVtbl = (void**)ourVtbl[-1];        // -> the recorded original
//
// This is safe because the shadow is heap-allocated so index -1 is inside
// the allocation.  There is no per-call locking and no map lookup.
// ---------------------------------------------------------------------------
namespace
{

struct Shadow
{
    void* Header;                  // Original vtable pointer (accessed via slots[-1]).
    void* Slots[kVtblCopySlots];   // Patched copy of the original vtable.
};

std::mutex                                              g_Mutex;
std::unordered_map<void**, std::unique_ptr<Shadow>>     g_ShadowsByOrig; // key = original vtable pointer
std::unordered_set<ID3D12Device*>                       g_WrappedDevices;
std::atomic<unsigned>                                   g_SimNodeCount{2};

inline UINT RemapNodeMask(UINT Mask)
{
    return (Mask == 0) ? 0u : 1u;
}

// Retrieves the original vtable stored in the -1 header of the shadow whose
// slot 0 the device is currently pointing at.
inline void** GetOrigVtbl(ID3D12Device* This)
{
    void** Slots = *reinterpret_cast<void***>(This);
    return static_cast<void**>(Slots[-1]);
}

} // namespace


// ---------------------------------------------------------------------------
// Vtable stubs
// ---------------------------------------------------------------------------

static UINT STDMETHODCALLTYPE Hook_GetNodeCount(ID3D12Device* /*This*/)
{
    const unsigned N = g_SimNodeCount.load(std::memory_order_relaxed);
    LogVerbose("GetNodeCount -> %u (simulated)", N);
    return N;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(
    ID3D12Device*                    This,
    const D3D12_COMMAND_QUEUE_DESC*  pDesc,
    REFIID                           riid,
    void**                           ppCommandQueue)
{
    D3D12_COMMAND_QUEUE_DESC Local = *pDesc;
    const UINT               Orig  = Local.NodeMask;
    Local.NodeMask                 = RemapNodeMask(Local.NodeMask);
    if (Orig != Local.NodeMask)
        LogVerbose("CreateCommandQueue: NodeMask 0x%X -> 0x%X", Orig, Local.NodeMask);

    using pfn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_CreateCommandQueue])(This, &Local, riid, ppCommandQueue);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(
    ID3D12Device*             This,
    UINT                      nodeMask,
    D3D12_COMMAND_LIST_TYPE   type,
    ID3D12CommandAllocator*   pCommandAllocator,
    ID3D12PipelineState*      pInitialState,
    REFIID                    riid,
    void**                    ppCommandList)
{
    const UINT Remapped = RemapNodeMask(nodeMask);
    if (Remapped != nodeMask)
        LogVerbose("CreateCommandList: NodeMask 0x%X -> 0x%X", nodeMask, Remapped);

    using pfn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_CreateCommandList])(
        This, Remapped, type, pCommandAllocator, pInitialState, riid, ppCommandList);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDescriptorHeap(
    ID3D12Device*                     This,
    const D3D12_DESCRIPTOR_HEAP_DESC* pDesc,
    REFIID                            riid,
    void**                            ppvHeap)
{
    D3D12_DESCRIPTOR_HEAP_DESC Local = *pDesc;
    const UINT                 Orig  = Local.NodeMask;
    Local.NodeMask                   = RemapNodeMask(Local.NodeMask);
    if (Orig != Local.NodeMask)
        LogVerbose("CreateDescriptorHeap: NodeMask 0x%X -> 0x%X", Orig, Local.NodeMask);

    using pfn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_CreateDescriptorHeap])(This, &Local, riid, ppvHeap);
}

static D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE Hook_GetResourceAllocationInfo(
    ID3D12Device*              This,
    UINT                       visibleMask,
    UINT                       numResourceDescs,
    const D3D12_RESOURCE_DESC* pResourceDescs)
{
    const UINT Remapped = RemapNodeMask(visibleMask);
    if (Remapped != visibleMask)
        LogVerbose("GetResourceAllocationInfo: visibleMask 0x%X -> 0x%X", visibleMask, Remapped);

    using pfn = D3D12_RESOURCE_ALLOCATION_INFO(STDMETHODCALLTYPE*)(
        ID3D12Device*, UINT, UINT, const D3D12_RESOURCE_DESC*);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_GetResourceAllocationInfo])(
        This, Remapped, numResourceDescs, pResourceDescs);
}

static D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE Hook_GetCustomHeapProperties(
    ID3D12Device*   This,
    UINT            nodeMask,
    D3D12_HEAP_TYPE heapType)
{
    const UINT Remapped = RemapNodeMask(nodeMask);
    if (Remapped != nodeMask)
        LogVerbose("GetCustomHeapProperties: NodeMask 0x%X -> 0x%X", nodeMask, Remapped);

    using pfn = D3D12_HEAP_PROPERTIES(STDMETHODCALLTYPE*)(
        ID3D12Device*, UINT, D3D12_HEAP_TYPE);
    D3D12_HEAP_PROPERTIES Result = reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_GetCustomHeapProperties])(
        This, Remapped, heapType);
    // Re-expose the simulated node bits in the returned properties so callers
    // that echo them back into CreateCommittedResource still see the same group.
    if (nodeMask != 0)
    {
        Result.CreationNodeMask = nodeMask;
        Result.VisibleNodeMask  = nodeMask;
    }
    return Result;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(
    ID3D12Device*                This,
    const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS             HeapFlags,
    const D3D12_RESOURCE_DESC*   pDesc,
    D3D12_RESOURCE_STATES        InitialResourceState,
    const D3D12_CLEAR_VALUE*     pOptimizedClearValue,
    REFIID                       riidResource,
    void**                       ppvResource)
{
    D3D12_HEAP_PROPERTIES Local = *pHeapProperties;
    const UINT            OrigC = Local.CreationNodeMask;
    const UINT            OrigV = Local.VisibleNodeMask;
    Local.CreationNodeMask      = RemapNodeMask(Local.CreationNodeMask);
    Local.VisibleNodeMask       = RemapNodeMask(Local.VisibleNodeMask);
    if (OrigC != Local.CreationNodeMask || OrigV != Local.VisibleNodeMask)
        LogVerbose("CreateCommittedResource: Creation 0x%X -> 0x%X, Visible 0x%X -> 0x%X",
                   OrigC, Local.CreationNodeMask, OrigV, Local.VisibleNodeMask);

    using pfn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_CreateCommittedResource])(
        This, &Local, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateHeap(
    ID3D12Device*           This,
    const D3D12_HEAP_DESC*  pDesc,
    REFIID                  riid,
    void**                  ppvHeap)
{
    D3D12_HEAP_DESC Local = *pDesc;
    const UINT      OrigC = Local.Properties.CreationNodeMask;
    const UINT      OrigV = Local.Properties.VisibleNodeMask;
    Local.Properties.CreationNodeMask = RemapNodeMask(Local.Properties.CreationNodeMask);
    Local.Properties.VisibleNodeMask  = RemapNodeMask(Local.Properties.VisibleNodeMask);
    if (OrigC != Local.Properties.CreationNodeMask || OrigV != Local.Properties.VisibleNodeMask)
        LogVerbose("CreateHeap: Creation 0x%X -> 0x%X, Visible 0x%X -> 0x%X",
                   OrigC, Local.Properties.CreationNodeMask, OrigV, Local.Properties.VisibleNodeMask);

    using pfn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_CreateHeap])(This, &Local, riid, ppvHeap);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateQueryHeap(
    ID3D12Device*                 This,
    const D3D12_QUERY_HEAP_DESC*  pDesc,
    REFIID                        riid,
    void**                        ppvHeap)
{
    D3D12_QUERY_HEAP_DESC Local = *pDesc;
    const UINT            Orig  = Local.NodeMask;
    Local.NodeMask              = RemapNodeMask(Local.NodeMask);
    if (Orig != Local.NodeMask)
        LogVerbose("CreateQueryHeap: NodeMask 0x%X -> 0x%X", Orig, Local.NodeMask);

    using pfn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void**);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_CreateQueryHeap])(This, &Local, riid, ppvHeap);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandSignature(
    ID3D12Device*                       This,
    const D3D12_COMMAND_SIGNATURE_DESC* pDesc,
    ID3D12RootSignature*                pRootSignature,
    REFIID                              riid,
    void**                              ppvCommandSignature)
{
    D3D12_COMMAND_SIGNATURE_DESC Local = *pDesc;
    const UINT                   Orig  = Local.NodeMask;
    Local.NodeMask                     = RemapNodeMask(Local.NodeMask);
    if (Orig != Local.NodeMask)
        LogVerbose("CreateCommandSignature: NodeMask 0x%X -> 0x%X", Orig, Local.NodeMask);

    using pfn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**);
    return reinterpret_cast<pfn>(GetOrigVtbl(This)[kSlot_CreateCommandSignature])(
        This, &Local, pRootSignature, riid, ppvCommandSignature);
}


// ---------------------------------------------------------------------------
// Shadow builder + WrapDevice / ReleaseWrappedState
// ---------------------------------------------------------------------------
namespace
{

// Builds (or fetches from cache) a shadow for the given original vtable.
Shadow* GetOrBuildShadow_locked(void** OrigVtbl)
{
    auto It = g_ShadowsByOrig.find(OrigVtbl);
    if (It != g_ShadowsByOrig.end())
        return It->second.get();

    // Determine how many slots we can safely copy from the original vtable.
    MEMORY_BASIC_INFORMATION Mbi{};
    SIZE_T                   MaxSlots = kVtblCopySlots;
    if (::VirtualQuery(OrigVtbl, &Mbi, sizeof(Mbi)) == sizeof(Mbi))
    {
        const auto*  Base = reinterpret_cast<const char*>(Mbi.BaseAddress);
        const auto*  Ptr  = reinterpret_cast<const char*>(OrigVtbl);
        const SIZE_T Room = (Base != nullptr && Ptr >= Base)
                                ? (Mbi.RegionSize - static_cast<SIZE_T>(Ptr - Base))
                                : 0;
        const SIZE_T RoomSlots = Room / sizeof(void*);
        if (RoomSlots > 0 && RoomSlots < MaxSlots)
            MaxSlots = RoomSlots;
    }

    auto S = std::make_unique<Shadow>();
    S->Header = OrigVtbl; // stored at Slots[-1]
    std::memset(S->Slots, 0, sizeof(S->Slots));
    std::memcpy(S->Slots, OrigVtbl, MaxSlots * sizeof(void*));

    // Install the patched slots.
    S->Slots[kSlot_GetNodeCount]              = reinterpret_cast<void*>(&Hook_GetNodeCount);
    S->Slots[kSlot_CreateCommandQueue]        = reinterpret_cast<void*>(&Hook_CreateCommandQueue);
    S->Slots[kSlot_CreateCommandList]         = reinterpret_cast<void*>(&Hook_CreateCommandList);
    S->Slots[kSlot_CreateDescriptorHeap]      = reinterpret_cast<void*>(&Hook_CreateDescriptorHeap);
    S->Slots[kSlot_GetResourceAllocationInfo] = reinterpret_cast<void*>(&Hook_GetResourceAllocationInfo);
    S->Slots[kSlot_GetCustomHeapProperties]   = reinterpret_cast<void*>(&Hook_GetCustomHeapProperties);
    S->Slots[kSlot_CreateCommittedResource]   = reinterpret_cast<void*>(&Hook_CreateCommittedResource);
    S->Slots[kSlot_CreateHeap]                = reinterpret_cast<void*>(&Hook_CreateHeap);
    S->Slots[kSlot_CreateQueryHeap]           = reinterpret_cast<void*>(&Hook_CreateQueryHeap);
    S->Slots[kSlot_CreateCommandSignature]    = reinterpret_cast<void*>(&Hook_CreateCommandSignature);

    Shadow* pRaw = S.get();
    g_ShadowsByOrig.emplace(OrigVtbl, std::move(S));
    LogInfo("Built shadow #%zu for orig vtable %p (%zu slots copied)",
            g_ShadowsByOrig.size(), OrigVtbl, MaxSlots);
    return pRaw;
}

} // namespace


bool WrapDevice(ID3D12Device* pDevice)
{
    if (pDevice == nullptr)
        return false;

    std::lock_guard<std::mutex> Lock(g_Mutex);

    if (g_WrappedDevices.count(pDevice) != 0)
    {
        LogVerbose("WrapDevice: device %p already wrapped - skipping", pDevice);
        return true;
    }

    g_SimNodeCount.store(GetConfig().SimNodeCount, std::memory_order_relaxed);

    void** OrigVtbl = *reinterpret_cast<void***>(pDevice);
    if (OrigVtbl == nullptr)
    {
        LogError("WrapDevice: device %p has NULL vtable pointer", pDevice);
        return false;
    }

    Shadow* pShadow = GetOrBuildShadow_locked(OrigVtbl);
    if (pShadow == nullptr)
        return false;

    // Swap this instance's vtable pointer to point at our shadow's slots.
    // pShadow->Slots[-1] is Shadow::Header, containing OrigVtbl.
    *reinterpret_cast<void***>(pDevice) = pShadow->Slots;
    g_WrappedDevices.insert(pDevice);

    LogInfo("D3D12 device %p wrapped: %u simulated linked nodes; %zu devices wrapped, %zu distinct vtables",
            pDevice, GetConfig().SimNodeCount, g_WrappedDevices.size(), g_ShadowsByOrig.size());
    return true;
}

void ReleaseWrappedState()
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    // Best-effort: restore each wrapped device to its original vtable so any
    // final cleanup by the D3D12 runtime dispatches through real methods.
    for (ID3D12Device* pDevice : g_WrappedDevices)
    {
        void** Slots = *reinterpret_cast<void***>(pDevice);
        if (Slots != nullptr)
        {
            void** Orig = static_cast<void**>(Slots[-1]);
            if (Orig != nullptr)
                *reinterpret_cast<void***>(pDevice) = Orig;
        }
    }
    g_WrappedDevices.clear();
    g_ShadowsByOrig.clear();
}

} // namespace D3D12Sim
