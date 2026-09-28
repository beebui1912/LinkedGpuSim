/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  DllMain for D3D12Sim.dll.
//  DLL_PROCESS_ATTACH   -> read config, open log, install D3D12 hook.
//  DLL_PROCESS_DETACH   -> remove hook, restore vtable, close log.

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include "D3D12DeviceWrapper.hpp"
#include "D3D12Hook.hpp"
#include "DXGIFactoryWrapper.hpp"
#include "DXGIHook.hpp"
#include "ShimConfig.hpp"
#include "ShimLog.hpp"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ReasonForCall, LPVOID /*lpReserved*/)
{
    switch (ReasonForCall)
    {
        case DLL_PROCESS_ATTACH:
        {
            ::DisableThreadLibraryCalls(hModule);

            // Reading the config first materialises the log-file setting so
            // that even a very early failure is captured on disk.
            (void)D3D12Sim::GetConfig();
            D3D12Sim::LogInit();

            D3D12Sim::LogInfo("D3D12Sim shim attached to process (PID=%lu, TID=%lu).",
                              ::GetCurrentProcessId(), ::GetCurrentThreadId());

            if (!D3D12Sim::InstallD3D12Hooks())
            {
                D3D12Sim::LogError("D3D12Sim shim: D3D12 hook installation failed; the child will "
                                   "run with the real driver behavior for D3D12.");
                // Return TRUE anyway - failing DllMain would tear down the
                // child process, which is worse than a passive shim.
            }
            if (!D3D12Sim::InstallDXGIHooks())
            {
                D3D12Sim::LogError("D3D12Sim shim: DXGI hook installation failed; GpuInfoPanel "
                                   "will still show the real adapter list.");
            }
            break;
        }

        case DLL_PROCESS_DETACH:
        {
            D3D12Sim::LogInfo("D3D12Sim shim detaching.");
            D3D12Sim::RemoveDXGIHooks();
            D3D12Sim::RemoveD3D12Hooks();
            D3D12Sim::ReleaseFactoryWrappedState();
            D3D12Sim::ReleaseWrappedState();
            D3D12Sim::LogShutdown();
            break;
        }
    }
    return TRUE;
}
