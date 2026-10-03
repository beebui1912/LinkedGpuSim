/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "DXGIFactoryWrapper.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "D3D12Tracking.hpp"
#include "D3D12Vtable.hpp"
#include "NodeMasks.hpp"
#include "ShimConfig.hpp"
#include "ShimLog.hpp"
#include "VirtualDXGIAdapter.hpp"
#include "VtableShadow.hpp"

namespace D3D12Sim
{

namespace
{

void PatchFactoryVtable(void** Slots, size_t NumSlots);
void PatchAdapterVtable(void** Slots, size_t NumSlots);
void PatchSwapChainVtable(void** Slots, size_t NumSlots);

VtableShadowSet g_Factories{"IDXGIFactory", &PatchFactoryVtable};
VtableShadowSet g_Adapters{"IDXGIAdapter3", &PatchAdapterVtable};
VtableShadowSet g_SwapChains{"IDXGISwapChain", &PatchSwapChainVtable};

// {7A1C2E40-3B5D-4C6E-8F70-91A2B3C4D520}: lifetime of a swap chain's tracked state
constexpr GUID kSwapChainLifetime = {0x7a1c2e40, 0x3b5d, 0x4c6e, {0x8f, 0x70, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0x20}};

constexpr UINT kMaxNodes = 32;

template <typename PFN>
PFN Orig(const void* This, int Slot)
{
    return reinterpret_cast<PFN>(VtableShadowSet::GetOrigVtbl(This)[Slot]);
}

// ---- Adapters: per-node video memory ---------------------------------------

std::mutex g_ReservationMutex;
UINT64     g_Reservations[kMaxNodes][2] = {}; // [node][local]

bool IsHostAdapter(IUnknown* pAdapter)
{
    IDXGIAdapter1* pAdapter1 = nullptr;
    if (pAdapter == nullptr || FAILED(pAdapter->QueryInterface(IID_PPV_ARGS(&pAdapter1))))
        return false;
    DXGI_ADAPTER_DESC1 Desc{};
    pAdapter1->GetDesc1(&Desc);
    pAdapter1->Release();
    const auto& Cfg = GetConfig();
    if (Cfg.HasHostLuid)
        return Desc.AdapterLuid.HighPart == Cfg.HostLuid.HighPart && Desc.AdapterLuid.LowPart == Cfg.HostLuid.LowPart;
    return (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0; // no host given: every hardware adapter, like the D3D12 hook
}

void WrapAdapter(IUnknown* pAdapter)
{
    IDXGIAdapter3* pAdapter3 = nullptr;
    if (!IsHostAdapter(pAdapter) || FAILED(pAdapter->QueryInterface(IID_PPV_ARGS(&pAdapter3))))
        return;
    g_Adapters.Wrap(pAdapter3);
    pAdapter3->Release();
}

HRESULT STDMETHODCALLTYPE Hook_Adapter_QueryVideoMemoryInfo(IDXGIAdapter3* This, UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP Group, DXGI_QUERY_VIDEO_MEMORY_INFO* pInfo)
{
    using PFN    = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*);
    const UINT N = SimNodeCount();
    if (NodeIndex >= N)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = Orig<PFN>(This, kAdapterSlot_QueryVideoMemoryInfo)(This, 0, Group, pInfo);
    if (FAILED(hr) || pInfo == nullptr || N <= 1)
        return hr;
    const bool   Local      = Group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
    const UINT64 Tracked    = GetTrackedTotalBytes(Local);
    const UINT64 Untracked  = pInfo->CurrentUsage > Tracked ? pInfo->CurrentUsage - Tracked : 0;
    pInfo->CurrentUsage     = GetTrackedNodeBytes(NodeIndex, Local) + Untracked / N;
    pInfo->Budget           = pInfo->Budget / N;
    pInfo->AvailableForReservation /= N;
    std::lock_guard<std::mutex> Lock{g_ReservationMutex};
    pInfo->CurrentReservation = g_Reservations[NodeIndex][Local ? 1 : 0];
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Adapter_SetVideoMemoryReservation(IDXGIAdapter3* This, UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP Group, UINT64 Reservation)
{
    using PFN    = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, UINT64);
    const UINT N = SimNodeCount();
    if (NodeIndex >= N)
        return DXGI_ERROR_INVALID_CALL;
    const int Local = Group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL ? 1 : 0;
    UINT64    Total = 0;
    {
        std::lock_guard<std::mutex> Lock{g_ReservationMutex};
        g_Reservations[NodeIndex][Local] = Reservation;
        for (UINT n = 0; n < N; ++n)
            Total += g_Reservations[n][Local];
    }
    return Orig<PFN>(This, kAdapterSlot_SetVideoMemoryReservation)(This, 0, Group, Total);
}

void PatchAdapterVtable(void** Slots, size_t NumSlots)
{
    if (kAdapterSlot_SetVideoMemoryReservation < static_cast<int>(NumSlots))
    {
        Slots[kAdapterSlot_QueryVideoMemoryInfo]      = reinterpret_cast<void*>(&Hook_Adapter_QueryVideoMemoryInfo);
        Slots[kAdapterSlot_SetVideoMemoryReservation] = reinterpret_cast<void*>(&Hook_Adapter_SetVideoMemoryReservation);
    }
}

// ---- Swap chains: buffers live on a node -------------------------------------

struct SwapChainState
{
    UINT              QueueNodeMask = 1;
    std::vector<UINT> BufferNodeMasks; // ResizeBuffers1; empty: every buffer on the queue's node
};

std::mutex                                      g_SwapChainMutex;
std::unordered_map<const void*, SwapChainState> g_SwapChainStates;

void OnSwapChainDestroyed(const void* pSwapChain)
{
    std::lock_guard<std::mutex> Lock{g_SwapChainMutex};
    g_SwapChainStates.erase(pSwapChain);
}

void WrapSwapChain(IUnknown* pSwapChain, IUnknown* pQueue)
{
    UINT QueueNode = 0;
    if (pSwapChain == nullptr || pQueue == nullptr || !GetObjectNodeMask(pQueue, QueueNode))
        return; // not created on a queue of a simulated device
    {
        std::lock_guard<std::mutex> Lock{g_SwapChainMutex};
        g_SwapChainStates[pSwapChain] = SwapChainState{QueueNode, {}};
    }
    OnObjectDestroyed(pSwapChain, kSwapChainLifetime, &OnSwapChainDestroyed);
    g_SwapChains.Wrap(pSwapChain);
}

HRESULT STDMETHODCALLTYPE Hook_SwapChain_GetBuffer(IDXGISwapChain* This, UINT Buffer, REFIID riid, void** ppSurface)
{
    using PFN        = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
    const HRESULT hr = Orig<PFN>(This, kSwapChainSlot_GetBuffer)(This, Buffer, riid, ppSurface);
    if (FAILED(hr) || ppSurface == nullptr || *ppSurface == nullptr)
        return hr;
    UINT NodeMask = 0;
    {
        std::lock_guard<std::mutex> Lock{g_SwapChainMutex};
        auto                        It = g_SwapChainStates.find(This);
        if (It == g_SwapChainStates.end())
            return hr;
        NodeMask = Buffer < It->second.BufferNodeMasks.size() ? It->second.BufferNodeMasks[Buffer] : It->second.QueueNodeMask;
    }
    ID3D12Resource* pResource = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*ppSurface)->QueryInterface(IID_PPV_ARGS(&pResource))))
    {
        ResourceInfo Info;
        if (!FindResource(pResource, Info))
        {
            Info.Creation = NodeMask;
            Info.Visible  = NodeMask;
            RegisterResource(pResource, Info);
        }
        pResource->Release();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SwapChain_ResizeBuffers(IDXGISwapChain* This, UINT Count, UINT Width, UINT Height, DXGI_FORMAT Format, UINT Flags)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    {
        std::lock_guard<std::mutex> Lock{g_SwapChainMutex};
        auto                        It = g_SwapChainStates.find(This);
        if (It != g_SwapChainStates.end())
            It->second.BufferNodeMasks.clear();
    }
    return Orig<PFN>(This, kSwapChainSlot_ResizeBuffers)(This, Count, Width, Height, Format, Flags);
}

HRESULT STDMETHODCALLTYPE Hook_SwapChain_ResizeBuffers1(IDXGISwapChain3* This, UINT Count, UINT Width, UINT Height, DXGI_FORMAT Format, UINT Flags,
                                                        const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    UINT NumBuffers = Count;
    if (NumBuffers == 0)
    {
        DXGI_SWAP_CHAIN_DESC Desc{};
        This->GetDesc(&Desc);
        NumBuffers = Desc.BufferCount;
    }
    std::vector<UINT> NodeMasks;
    std::vector<UINT> Physical;
    for (UINT i = 0; pCreationNodeMask != nullptr && i < NumBuffers; ++i)
    {
        if (!CheckSingleNodeMask("ResizeBuffers1", "pCreationNodeMask[]", pCreationNodeMask[i]))
            return DXGI_ERROR_INVALID_CALL;
        const UINT Mask = NormalizeSingleNode(pCreationNodeMask[i]);
        UINT       QueueNode = 0;
        if (ValidationEnabled() && ppPresentQueue != nullptr && ppPresentQueue[i] != nullptr && GetObjectNodeMask(ppPresentQueue[i], QueueNode) && QueueNode != Mask)
            ReportValidationError("ResizeBuffers1: buffer %u is created on node mask 0x%X but presented by a queue of node mask 0x%X", i, Mask, QueueNode);
        NodeMasks.push_back(Mask);
        Physical.push_back(1u);
    }
    {
        std::lock_guard<std::mutex> Lock{g_SwapChainMutex};
        auto                        It = g_SwapChainStates.find(This);
        if (It != g_SwapChainStates.end())
            It->second.BufferNodeMasks = NodeMasks;
    }
    return Orig<PFN>(This, kSwapChainSlot_ResizeBuffers1)(This, Count, Width, Height, Format, Flags, pCreationNodeMask != nullptr ? Physical.data() : nullptr, ppPresentQueue);
}

void PatchSwapChainVtable(void** Slots, size_t NumSlots)
{
    if (kSwapChainSlot_ResizeBuffers < static_cast<int>(NumSlots))
    {
        Slots[kSwapChainSlot_GetBuffer]     = reinterpret_cast<void*>(&Hook_SwapChain_GetBuffer);
        Slots[kSwapChainSlot_ResizeBuffers] = reinterpret_cast<void*>(&Hook_SwapChain_ResizeBuffers);
    }
    if (kSwapChainSlot_ResizeBuffers1 < static_cast<int>(NumSlots))
        Slots[kSwapChainSlot_ResizeBuffers1] = reinterpret_cast<void*>(&Hook_SwapChain_ResizeBuffers1);
}

// ---- Factories ---------------------------------------------------------------

// The host hardware adapter through the original EnumAdapters1 (virtual adapters)
Microsoft::WRL::ComPtr<IDXGIAdapter1> FindHostAdapter(IDXGIFactory1* pFactory)
{
    using PFN    = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
    auto pfnReal = Orig<PFN>(pFactory, kFactorySlot_EnumAdapters1);
    Microsoft::WRL::ComPtr<IDXGIAdapter1> First;
    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> A;
        if (pfnReal(pFactory, i, A.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;
        if (IsHostAdapter(A.Get()))
            return A;
        if (!First)
            First = A;
    }
    return First;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_EnumAdapters1(IDXGIFactory1* This, UINT Index, IDXGIAdapter1** ppAdapter)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
    if (ppAdapter == nullptr)
        return DXGI_ERROR_INVALID_CALL;
    UINT RealIndex = Index;
    if (GetConfig().VirtualAdapters)
    {
        // Virtual adapters 0..N-1 (one per node), then the real list
        const UINT N = SimNodeCount();
        if (Index < N)
        {
            auto pHost = FindHostAdapter(This);
            if (!pHost)
                return DXGI_ERROR_NOT_FOUND;
            WrapAdapter(pHost.Get()); // the per-node memory figures come from the real adapter
            *ppAdapter = VirtualDXGIAdapter::Create(pHost.Get(), Index, N);
            return *ppAdapter != nullptr ? S_OK : E_OUTOFMEMORY;
        }
        RealIndex = Index - N;
    }
    const HRESULT hr = Orig<PFN>(This, kFactorySlot_EnumAdapters1)(This, RealIndex, ppAdapter);
    if (SUCCEEDED(hr))
        WrapAdapter(*ppAdapter);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_EnumAdapters(IDXGIFactory1* This, UINT Index, IDXGIAdapter** ppAdapter)
{
    if (ppAdapter == nullptr)
        return DXGI_ERROR_INVALID_CALL;
    *ppAdapter = nullptr;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter1;
    const HRESULT                         hr = Hook_Factory_EnumAdapters1(This, Index, Adapter1.GetAddressOf());
    return SUCCEEDED(hr) ? Adapter1->QueryInterface(IID_PPV_ARGS(ppAdapter)) : hr;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_EnumAdapterByLuid(IDXGIFactory4* This, LUID Luid, REFIID riid, void** ppAdapter)
{
    using PFN        = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory4*, LUID, REFIID, void**);
    const HRESULT hr = Orig<PFN>(This, kFactorySlot_EnumAdapterByLuid)(This, Luid, riid, ppAdapter);
    if (SUCCEEDED(hr) && ppAdapter != nullptr)
        WrapAdapter(static_cast<IUnknown*>(*ppAdapter));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_EnumAdapterByGpuPreference(IDXGIFactory6* This, UINT Index, DXGI_GPU_PREFERENCE Preference, REFIID riid, void** ppAdapter)
{
    using PFN        = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory6*, UINT, DXGI_GPU_PREFERENCE, REFIID, void**);
    const HRESULT hr = Orig<PFN>(This, kFactorySlot_EnumAdapterByGpuPreference)(This, Index, Preference, riid, ppAdapter);
    if (SUCCEEDED(hr) && ppAdapter != nullptr)
        WrapAdapter(static_cast<IUnknown*>(*ppAdapter));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_CreateSwapChain(IDXGIFactory* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    using PFN        = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    const HRESULT hr = Orig<PFN>(This, kFactorySlot_CreateSwapChain)(This, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain != nullptr)
        WrapSwapChain(*ppSwapChain, pDevice);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_CreateSwapChainForHwnd(IDXGIFactory2* This, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pOutput, IDXGISwapChain1** ppSwapChain)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                            IDXGISwapChain1**);
    const HRESULT hr = Orig<PFN>(This, kFactorySlot_CreateSwapChainForHwnd)(This, pDevice, hWnd, pDesc, pFullscreenDesc, pOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain != nullptr)
        WrapSwapChain(*ppSwapChain, pDevice);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_CreateSwapChainForCoreWindow(IDXGIFactory2* This, IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    IDXGIOutput* pOutput, IDXGISwapChain1** ppSwapChain)
{
    using PFN        = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    const HRESULT hr = Orig<PFN>(This, kFactorySlot_CreateSwapChainForCoreWindow)(This, pDevice, pWindow, pDesc, pOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain != nullptr)
        WrapSwapChain(*ppSwapChain, pDevice);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Factory_CreateSwapChainForComposition(IDXGIFactory2* This, IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pOutput,
                                                                     IDXGISwapChain1** ppSwapChain)
{
    using PFN        = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    const HRESULT hr = Orig<PFN>(This, kFactorySlot_CreateSwapChainForComposition)(This, pDevice, pDesc, pOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain != nullptr)
        WrapSwapChain(*ppSwapChain, pDevice);
    return hr;
}

void PatchFactoryVtable(void** Slots, size_t NumSlots)
{
    auto Set = [&](int Slot, auto* pHook) {
        if (static_cast<size_t>(Slot) < NumSlots)
            Slots[Slot] = reinterpret_cast<void*>(pHook);
    };
    Set(kFactorySlot_EnumAdapters, &Hook_Factory_EnumAdapters);
    Set(kFactorySlot_CreateSwapChain, &Hook_Factory_CreateSwapChain);
    Set(kFactorySlot_EnumAdapters1, &Hook_Factory_EnumAdapters1);
    // Newer factory interfaces: only reachable on factories that implement them
    Set(kFactorySlot_CreateSwapChainForHwnd, &Hook_Factory_CreateSwapChainForHwnd);
    Set(kFactorySlot_CreateSwapChainForCoreWindow, &Hook_Factory_CreateSwapChainForCoreWindow);
    Set(kFactorySlot_CreateSwapChainForComposition, &Hook_Factory_CreateSwapChainForComposition);
    Set(kFactorySlot_EnumAdapterByLuid, &Hook_Factory_EnumAdapterByLuid);
    Set(kFactorySlot_EnumAdapterByGpuPreference, &Hook_Factory_EnumAdapterByGpuPreference);
}

} // namespace

bool WrapFactory(IUnknown* pFactory)
{
    IDXGIFactory1* pFactory1 = nullptr;
    if (pFactory == nullptr || FAILED(pFactory->QueryInterface(IID_PPV_ARGS(&pFactory1))))
        return false;
    const bool Ok = g_Factories.Wrap(pFactory1);
    pFactory1->Release();
    return Ok;
}

} // namespace D3D12Sim
