/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  DllMain for D3D12Sim.dll.
//  DLL_PROCESS_ATTACH   -> read config, open log, pin the DLL, install hooks.
//  DLL_PROCESS_DETACH   -> log a summary and close the log.
//
//  The DLL is pinned (never unloaded): wrapped devices, queues and command
//  lists dispatch through vtables and hooks that live in this module, and they
//  may be alive until the process ends.  For the same reason nothing is
//  restored at detach: during process exit the objects may already be freed.

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include "D3D12Hook.hpp"
#include "DXGIHook.hpp"
#include "NodeMasks.hpp"
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
            const auto& Cfg = D3D12Sim::GetConfig();
            D3D12Sim::LogInit();

            HMODULE hPinned = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                 reinterpret_cast<LPCWSTR>(&DllMain), &hPinned);

            D3D12Sim::LogInfo("D3D12Sim shim attached to process (PID=%lu): %u simulated nodes, validation %s, host adapter %s, virtual DXGI adapters %s.",
                              ::GetCurrentProcessId(), Cfg.SimNodeCount, Cfg.Validate ? "on" : "off",
                              Cfg.HasHostLuid ? "by LUID" : "any (no DILIGENT_SIM_HOST_ADAPTER_LUID)", Cfg.VirtualAdapters ? "on" : "off");

            if (!D3D12Sim::InstallD3D12Hooks())
            {
                D3D12Sim::LogError("D3D12Sim shim: D3D12 hook installation failed; the child will "
                                   "run with the real driver behavior for D3D12.");
                // Return TRUE anyway - failing DllMain would tear down the
                // child process, which is worse than a passive shim.
            }
            if (Cfg.VirtualAdapters && !D3D12Sim::InstallDXGIHooks())
            {
                D3D12Sim::LogError("D3D12Sim shim: DXGI hook installation failed; GpuInfoPanel "
                                   "will still show the real adapter list.");
            }
            break;
        }

        case DLL_PROCESS_DETACH:
        {
            const unsigned Errors = D3D12Sim::GetValidationErrorCount();
            D3D12Sim::LogInfo("D3D12Sim shim detaching: %u validation error(s).", Errors);
            D3D12Sim::LogShutdown();
            break;
        }
    }
    return TRUE;
}
