/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "D3D12Hook.hpp"

#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "MinHook.h"

#include "D3D12DeviceWrapper.hpp"
#include "ShimConfig.hpp"
#include "ShimLog.hpp"
#include "VirtualDXGIAdapter.hpp"

namespace D3D12Sim
{

namespace
{

// Trampoline set by MinHook so we can call the real driver function.
using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PFN_D3D12CreateDevice g_TrueD3D12CreateDevice = nullptr;

std::atomic<bool>     g_Installed{false};

HRESULT WINAPI HookedD3D12CreateDevice(
    IUnknown*          pAdapter,
    D3D_FEATURE_LEVEL  MinimumFeatureLevel,
    REFIID             riid,
    void**             ppDevice)
{
    // Probe call (ppDevice == nullptr): D3D12CreateDevice is used with
    // ppDevice==nullptr solely to test if a device can be created.  Just
    // forward without wrapping.
    if (g_TrueD3D12CreateDevice == nullptr)
    {
        // Shouldn't happen (hook installed => trampoline set), but be safe.
        return E_UNEXPECTED;
    }

    // If pAdapter is one of our VirtualDXGIAdapter proxies (from EnumAdapters1),
    // hand the real backing adapter to the driver.  Real adapters simply
    // return E_NOINTERFACE for our private IID and stay unchanged.
    IUnknown*                            pAdapterForDriver = pAdapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> pRealBacking;
    if (pAdapter != nullptr)
    {
        HRESULT qi = pAdapter->QueryInterface(
            VirtualDXGIAdapter::IID_RevealReal,
            reinterpret_cast<void**>(pRealBacking.GetAddressOf()));
        if (SUCCEEDED(qi) && pRealBacking)
        {
            LogVerbose("D3D12CreateDevice: unwrapping virtual proxy %p -> real adapter %p",
                       pAdapter, pRealBacking.Get());
            pAdapterForDriver = pRealBacking.Get();
        }
    }

    const HRESULT hr = g_TrueD3D12CreateDevice(pAdapterForDriver, MinimumFeatureLevel, riid, ppDevice);

    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr)
    {
        // Every ID3D12Device[N] extension shares the base ID3D12Device vtable
        // (Microsoft's implementation returns the same instance pointer for
        // every ID3D12Device* interface via QueryInterface), so a cast to the
        // base is safe here regardless of the riid the caller requested.
        auto*       pDevice = static_cast<ID3D12Device*>(*ppDevice);
        const auto& Cfg     = GetConfig();
        const LUID  Luid    = pDevice->GetAdapterLuid();
        if (Cfg.HasHostLuid && (Luid.HighPart != Cfg.HostLuid.HighPart || Luid.LowPart != Cfg.HostLuid.LowPart))
        {
            // Only the linked adapter has several nodes; other adapters keep their real behaviour
            LogVerbose("D3D12CreateDevice: device %p is on adapter 0x%08X_0x%08X, not the host adapter - not wrapped",
                       pDevice, static_cast<unsigned>(Luid.HighPart), static_cast<unsigned>(Luid.LowPart));
        }
        else
        {
            WrapDevice(pDevice);
        }
    }
    else if (FAILED(hr))
    {
        LogVerbose("D3D12CreateDevice returned HRESULT 0x%08X (probe or failure - not wrapping)",
                   static_cast<unsigned>(hr));
    }

    return hr;
}

} // namespace


bool InstallD3D12Hooks()
{
    if (g_Installed.load(std::memory_order_acquire))
        return true;

    // d3d12.dll must be loaded so MinHook can resolve the export address.
    // Force-load it here: it's a Windows-known DLL, safe to load from any
    // context including DllMain.
    HMODULE hD3D12 = ::GetModuleHandleW(L"d3d12.dll");
    if (hD3D12 == nullptr)
        hD3D12 = ::LoadLibraryW(L"d3d12.dll");
    if (hD3D12 == nullptr)
    {
        LogError("InstallD3D12Hooks: could not load d3d12.dll (GetLastError=%lu).",
                 ::GetLastError());
        return false;
    }

    MH_STATUS Status = MH_Initialize();
    if (Status != MH_OK && Status != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogError("InstallD3D12Hooks: MH_Initialize failed (status=%d).", Status);
        return false;
    }

    Status = MH_CreateHookApi(
        L"d3d12.dll",
        "D3D12CreateDevice",
        reinterpret_cast<LPVOID>(&HookedD3D12CreateDevice),
        reinterpret_cast<LPVOID*>(&g_TrueD3D12CreateDevice));
    if (Status != MH_OK)
    {
        LogError("InstallD3D12Hooks: MH_CreateHookApi(D3D12CreateDevice) failed (status=%d).", Status);
        MH_Uninitialize();
        return false;
    }

    Status = MH_EnableHook(MH_ALL_HOOKS);
    if (Status != MH_OK)
    {
        LogError("InstallD3D12Hooks: MH_EnableHook failed (status=%d).", Status);
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_TrueD3D12CreateDevice));
        MH_Uninitialize();
        return false;
    }

    g_Installed.store(true, std::memory_order_release);
    const auto& Cfg = GetConfig();
    LogInfo("D3D12CreateDevice hook installed. Simulated NodeCount=%u, ParentPID=%lu.",
            Cfg.SimNodeCount, Cfg.ParentPid);
    return true;
}

void RemoveD3D12Hooks()
{
    if (!g_Installed.load(std::memory_order_acquire))
        return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_TrueD3D12CreateDevice = nullptr;
    g_Installed.store(false, std::memory_order_release);
    LogInfo("D3D12CreateDevice hook removed.");
}

} // namespace D3D12Sim
