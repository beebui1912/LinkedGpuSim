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

    // Check node masks and cross-node use against the simulated topology and
    // reject or report what real linked hardware would reject (see NodeMasks.hpp).
    // DILIGENT_SIM_VALIDATION=0 turns this off (masks are only remapped).
    bool Validate = true;

    // Only devices on this adapter get simulated nodes, like real hardware where
    // only the linked adapter has several. DILIGENT_SIM_HOST_ADAPTER_LUID
    // ("0xHIGH_0xLOW", set by SimulationApp); without it every device is wrapped.
    bool  HasHostLuid = false;
    LUID  HostLuid    = {};

    // Add one virtual DXGI adapter per simulated node in front of the real
    // adapter list (DILIGENT_SIM_VIRTUAL_ADAPTERS=1). A real linked adapter is ONE
    // DXGI adapter with several nodes, so this is off by default; it exists for
    // tools that show one entry per GPU (Diligent's GpuInfoPanel).
    bool VirtualAdapters = false;

    // D3D12_CROSS_NODE_SHARING_TIER reported by CheckFeatureSupport
    // (DILIGENT_SIM_CROSS_NODE_TIER, default 1: cross-node copies only).
    unsigned CrossNodeSharingTier = 1;
};

// Reads the configuration from environment variables.  Idempotent; safe to
// call more than once (later calls return the same cached instance).
const ShimConfig& GetConfig();

// Best-effort formatting helpers used by the hook logs.
std::string NodeMaskToHex(unsigned Mask);

} // namespace D3D12Sim
