/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "DXGIFactoryWrapper.hpp"

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
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "ShimConfig.hpp"
#include "ShimLog.hpp"
#include "VirtualDXGIAdapter.hpp"

namespace D3D12Sim
{

namespace
{

// IDXGIFactory1 vtable slot indices (in header-declaration order):
//  IUnknown  (3)
//   0: QueryInterface
//   1: AddRef
//   2: Release
//  IDXGIObject (4)
//   3: SetPrivateData
//   4: SetPrivateDataInterface
//   5: GetPrivateData
//   6: GetParent
//  IDXGIFactory (5)
//   7: EnumAdapters              <-- patched
//   8: MakeWindowAssociation
//   9: GetWindowAssociation
//  10: CreateSwapChain
//  11: CreateSoftwareAdapter
//  IDXGIFactory1 (2)
//  12: EnumAdapters1             <-- patched
//  13: IsCurrent
constexpr int kSlot_EnumAdapters  = 7;
constexpr int kSlot_EnumAdapters1 = 12;

// Enough to cover IDXGIFactory1..7 comfortably.
constexpr int kFactoryVtblCopySlots = 128;

struct FactoryShadow
{
    void* Header;                          // Original vtable ptr, accessed via Slots[-1].
    void* Slots[kFactoryVtblCopySlots];    // Patched copy.
};

std::mutex                                                  g_Mutex;
std::unordered_map<void**, std::unique_ptr<FactoryShadow>>  g_ShadowsByOrig;
std::unordered_set<IDXGIFactory1*>                          g_WrappedFactories;

// Locates the primary hardware adapter through the original EnumAdapters1
// slot (i.e. bypassing our patch).  Returns AddRef'd pointer to the first
// non-software adapter, or the first adapter overall.
Microsoft::WRL::ComPtr<IDXGIAdapter1> FindPrimaryRealAdapter_Unlocked(
    IDXGIFactory1* pFactory, void** OrigVtbl)
{
    using pfnEnumAdapters1 = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
    auto pfnReal = reinterpret_cast<pfnEnumAdapters1>(OrigVtbl[kSlot_EnumAdapters1]);

    Microsoft::WRL::ComPtr<IDXGIAdapter1> FirstAny;
    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> A;
        if (pfnReal(pFactory, i, A.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 Desc{};
        A->GetDesc1(&Desc);
        if (!FirstAny) FirstAny = A;
        if ((Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            return A;
    }
    return FirstAny;
}

// ---------------------------------------------------------------------------
// Hook stubs
// ---------------------------------------------------------------------------

// EnumAdapters1: virtual nodes 0..N-1, then real adapters N.. .
static HRESULT STDMETHODCALLTYPE Hook_EnumAdapters1(
    IDXGIFactory1* This, UINT Adapter, IDXGIAdapter1** ppAdapter)
{
    if (ppAdapter == nullptr)
        return DXGI_ERROR_INVALID_CALL;
    *ppAdapter = nullptr;

    const unsigned N = GetConfig().SimNodeCount;

    // Find the shadow this factory is currently pointing at (to reach the
    // original EnumAdapters1 through its Slots[-1] header).
    void** Slots = *reinterpret_cast<void***>(This);
    void** OrigVtbl = static_cast<void**>(Slots[-1]);
    using pfnEnumAdapters1 = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
    auto pfnReal = reinterpret_cast<pfnEnumAdapters1>(OrigVtbl[kSlot_EnumAdapters1]);

    // Adapter < N: virtual linked-node proxy backed by the primary hw adapter.
    if (Adapter < N)
    {
        auto pPrimary = FindPrimaryRealAdapter_Unlocked(This, OrigVtbl);
        if (!pPrimary)
            return DXGI_ERROR_NOT_FOUND;

        IDXGIAdapter1* pProxy = VirtualDXGIAdapter::Create(pPrimary.Get(), Adapter, N);
        if (pProxy == nullptr)
            return E_OUTOFMEMORY;
        *ppAdapter = pProxy;
        LogVerbose("EnumAdapters1(%u) -> virtual node %u", Adapter, Adapter);
        return S_OK;
    }

    // Adapter >= N: pass through to the real enumeration, exposed
    // contiguously after the virtual block starting from real index 0.
    // The primary appears both as the N virtual nodes AND as this real entry
    // - Diligent's GetHardwareAdapter uses this real entry as the fallback if
    // it ever declines to probe our virtual proxies.
    const UINT RealIndex = Adapter - N;
    const HRESULT hr = pfnReal(This, RealIndex, ppAdapter);
    if (SUCCEEDED(hr))
        LogVerbose("EnumAdapters1(%u) -> real adapter %u", Adapter, RealIndex);
    return hr;
}

// EnumAdapters (base, without _1): analogous.  Diligent doesn't use it but
// GpuInfoPanel could in theory, so patch it for completeness.
static HRESULT STDMETHODCALLTYPE Hook_EnumAdapters(
    IDXGIFactory1* This, UINT Adapter, IDXGIAdapter** ppAdapter)
{
    if (ppAdapter == nullptr)
        return DXGI_ERROR_INVALID_CALL;
    *ppAdapter = nullptr;

    // Reuse the EnumAdapters1 hook, then QI up to IDXGIAdapter (base).
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter1;
    const HRESULT hr = Hook_EnumAdapters1(This, Adapter, Adapter1.GetAddressOf());
    if (SUCCEEDED(hr) && Adapter1)
        return Adapter1->QueryInterface(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(ppAdapter));
    return hr;
}

// ---------------------------------------------------------------------------
// Shadow builder
// ---------------------------------------------------------------------------

FactoryShadow* GetOrBuildFactoryShadow_locked(void** OrigVtbl)
{
    auto It = g_ShadowsByOrig.find(OrigVtbl);
    if (It != g_ShadowsByOrig.end())
        return It->second.get();

    MEMORY_BASIC_INFORMATION Mbi{};
    SIZE_T MaxSlots = kFactoryVtblCopySlots;
    if (::VirtualQuery(OrigVtbl, &Mbi, sizeof(Mbi)) == sizeof(Mbi))
    {
        const auto* Base = reinterpret_cast<const char*>(Mbi.BaseAddress);
        const auto* Ptr  = reinterpret_cast<const char*>(OrigVtbl);
        const SIZE_T Room = (Base != nullptr && Ptr >= Base)
                                ? (Mbi.RegionSize - static_cast<SIZE_T>(Ptr - Base))
                                : 0;
        const SIZE_T RoomSlots = Room / sizeof(void*);
        if (RoomSlots > 0 && RoomSlots < MaxSlots)
            MaxSlots = RoomSlots;
    }

    auto S = std::make_unique<FactoryShadow>();
    S->Header = OrigVtbl;
    std::memset(S->Slots, 0, sizeof(S->Slots));
    std::memcpy(S->Slots, OrigVtbl, MaxSlots * sizeof(void*));

    S->Slots[kSlot_EnumAdapters]  = reinterpret_cast<void*>(&Hook_EnumAdapters);
    S->Slots[kSlot_EnumAdapters1] = reinterpret_cast<void*>(&Hook_EnumAdapters1);

    FactoryShadow* pRaw = S.get();
    g_ShadowsByOrig.emplace(OrigVtbl, std::move(S));
    LogInfo("Built DXGI factory shadow #%zu for orig vtable %p (%zu slots copied)",
            g_ShadowsByOrig.size(), OrigVtbl, MaxSlots);
    return pRaw;
}

} // namespace


bool WrapFactory(IDXGIFactory1* pFactory)
{
    if (pFactory == nullptr) return false;

    std::lock_guard<std::mutex> Lock(g_Mutex);
    if (g_WrappedFactories.count(pFactory) != 0)
        return true;

    void** OrigVtbl = *reinterpret_cast<void***>(pFactory);
    if (OrigVtbl == nullptr) return false;

    FactoryShadow* pShadow = GetOrBuildFactoryShadow_locked(OrigVtbl);
    if (pShadow == nullptr) return false;

    *reinterpret_cast<void***>(pFactory) = pShadow->Slots;
    g_WrappedFactories.insert(pFactory);
    LogInfo("DXGI factory %p wrapped (%zu total wrapped, %zu distinct vtables)",
            pFactory, g_WrappedFactories.size(), g_ShadowsByOrig.size());
    return true;
}

void ReleaseFactoryWrappedState()
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    for (IDXGIFactory1* pFactory : g_WrappedFactories)
    {
        void** Slots = *reinterpret_cast<void***>(pFactory);
        if (Slots != nullptr)
        {
            void** Orig = static_cast<void**>(Slots[-1]);
            if (Orig != nullptr)
                *reinterpret_cast<void***>(pFactory) = Orig;
        }
    }
    g_WrappedFactories.clear();
    g_ShadowsByOrig.clear();
}

} // namespace D3D12Sim
