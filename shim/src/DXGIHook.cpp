/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "DXGIHook.hpp"

#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "MinHook.h"

#include "DXGIFactoryWrapper.hpp"
#include "ShimLog.hpp"

namespace D3D12Sim
{

namespace
{

using PFN_CreateDXGIFactory  = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

PFN_CreateDXGIFactory  g_TrueCreateDXGIFactory  = nullptr;
PFN_CreateDXGIFactory1 g_TrueCreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 g_TrueCreateDXGIFactory2 = nullptr;

std::atomic<bool> g_Installed{false};

// Wraps the factory object if the returned interface can be QI'd to
// IDXGIFactory1.  The vtable swap applies to the whole factory object, so
// all interface versions (IDXGIFactory..IDXGIFactory7) see the patched slots.
void WrapIfDxgiFactory(void** ppInterface)
{
    if (ppInterface == nullptr || *ppInterface == nullptr) return;

    auto* pUnk = static_cast<IUnknown*>(*ppInterface);
    Microsoft::WRL::ComPtr<IDXGIFactory1> pFactory1;
    if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(pFactory1.GetAddressOf()))))
        WrapFactory(pFactory1.Get());
}

HRESULT WINAPI HookedCreateDXGIFactory(REFIID riid, void** ppFactory)
{
    if (g_TrueCreateDXGIFactory == nullptr) return E_UNEXPECTED;
    const HRESULT hr = g_TrueCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr)) WrapIfDxgiFactory(ppFactory);
    return hr;
}

HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    if (g_TrueCreateDXGIFactory1 == nullptr) return E_UNEXPECTED;
    const HRESULT hr = g_TrueCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr)) WrapIfDxgiFactory(ppFactory);
    return hr;
}

HRESULT WINAPI HookedCreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory)
{
    if (g_TrueCreateDXGIFactory2 == nullptr) return E_UNEXPECTED;
    const HRESULT hr = g_TrueCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr)) WrapIfDxgiFactory(ppFactory);
    return hr;
}

} // namespace


bool InstallDXGIHooks()
{
    if (g_Installed.load(std::memory_order_acquire)) return true;

    HMODULE hDXGI = ::GetModuleHandleW(L"dxgi.dll");
    if (hDXGI == nullptr)
        hDXGI = ::LoadLibraryW(L"dxgi.dll");
    if (hDXGI == nullptr)
    {
        LogError("InstallDXGIHooks: could not load dxgi.dll (GetLastError=%lu).", ::GetLastError());
        return false;
    }

    // MH_Initialize is idempotent enough (we already called it in the D3D12
    // hook installer), but check status to be safe.
    MH_STATUS Init = MH_Initialize();
    if (Init != MH_OK && Init != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogError("InstallDXGIHooks: MH_Initialize failed (status=%d).", Init);
        return false;
    }

    struct HookSpec
    {
        const char* Name;
        void*       Hook;
        void**      TrueOut;
        bool        Optional; // OK if the export is missing on this Windows version.
    };
    HookSpec Hooks[] = {
        {"CreateDXGIFactory",  reinterpret_cast<void*>(&HookedCreateDXGIFactory),
                                reinterpret_cast<void**>(&g_TrueCreateDXGIFactory),  false},
        {"CreateDXGIFactory1", reinterpret_cast<void*>(&HookedCreateDXGIFactory1),
                                reinterpret_cast<void**>(&g_TrueCreateDXGIFactory1), false},
        {"CreateDXGIFactory2", reinterpret_cast<void*>(&HookedCreateDXGIFactory2),
                                reinterpret_cast<void**>(&g_TrueCreateDXGIFactory2), true},
    };

    for (const HookSpec& H : Hooks)
    {
        MH_STATUS S = MH_CreateHookApi(L"dxgi.dll", H.Name, H.Hook, H.TrueOut);
        if (S != MH_OK)
        {
            if (H.Optional && S == MH_ERROR_FUNCTION_NOT_FOUND)
            {
                LogVerbose("InstallDXGIHooks: %s not present on this Windows - skipping.", H.Name);
                continue;
            }
            LogError("InstallDXGIHooks: MH_CreateHookApi(%s) failed (status=%d).", H.Name, S);
            return false;
        }
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        LogError("InstallDXGIHooks: MH_EnableHook failed.");
        return false;
    }

    g_Installed.store(true, std::memory_order_release);
    LogInfo("DXGI hooks installed (CreateDXGIFactory / CreateDXGIFactory1 / CreateDXGIFactory2).");
    return true;
}

void RemoveDXGIHooks()
{
    if (!g_Installed.load(std::memory_order_acquire)) return;
    // We share MinHook state with D3D12Hook; only disable the ones we own.
    if (g_TrueCreateDXGIFactory)  MH_RemoveHook(reinterpret_cast<LPVOID>(g_TrueCreateDXGIFactory));
    if (g_TrueCreateDXGIFactory1) MH_RemoveHook(reinterpret_cast<LPVOID>(g_TrueCreateDXGIFactory1));
    if (g_TrueCreateDXGIFactory2) MH_RemoveHook(reinterpret_cast<LPVOID>(g_TrueCreateDXGIFactory2));
    g_TrueCreateDXGIFactory  = nullptr;
    g_TrueCreateDXGIFactory1 = nullptr;
    g_TrueCreateDXGIFactory2 = nullptr;
    g_Installed.store(false, std::memory_order_release);
    LogInfo("DXGI hooks removed.");
}

} // namespace D3D12Sim
