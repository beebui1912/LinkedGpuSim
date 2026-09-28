/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  ShimConfig
//  ----------
//  All shim behaviour is driven by the DILIGENT_SIM_* environment variables
//  that SimulationApp.exe exports before starting the child.  This module
//  reads them once at DLL load time and exposes the parsed state.

#pragma once

#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace D3D12Sim
{

struct ShimConfig
{
    // Number of simulated linked GPU nodes to advertise to the child
    // (via ID3D12Device::GetNodeCount and everything that flows from it).
    unsigned SimNodeCount = 2;

    // Convenience: (1u << SimNodeCount) - 1u
    unsigned SimNodeMask = 0x3u;

    // Path to the shared log file (may be empty).  Written to append-mode so
    // SimulationApp.exe's LogSink and this shim can tee into the same file.
    std::wstring LogFilePath;

    // Parent PID (SimulationApp).  0 when running outside SimulationApp.
    DWORD ParentPid = 0;

    // If true, the shim reports every hooked call via ShimLog::LogInfo.
    // Set with DILIGENT_SIM_VERBOSE=1 (default off).
    bool Verbose = false;
};

// Reads the configuration from environment variables.  Idempotent; safe to
// call more than once (later calls return the same cached instance).
const ShimConfig& GetConfig();

// Best-effort formatting helpers used by the hook logs.
std::string NodeMaskToHex(unsigned Mask);

} // namespace D3D12Sim
