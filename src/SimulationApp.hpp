/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

//  SimulationApp
//  -------------
//  Top-level orchestrator that stitches together the LinkedGpuSimulator,
//  GpuInfoConsole, ProcessLauncher and LogCapture pieces into the
//  `SimulationApp.exe <child.exe> [args...]` command-line tool.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace SimApp
{

struct SimulationOptions
{
    // Child process to run under the simulator.  If empty, only the info panel
    // is printed and the tool exits.
    std::filesystem::path     ChildExe;
    std::vector<std::wstring> ChildArgs;

    // Working directory of the child. Empty ⇒ the child's executable directory.
    std::filesystem::path ChildWorkingDir;

    // Log destination.  Empty + WriteLogFile==false ⇒ console only.
    std::filesystem::path LogFile;
    bool                  WriteLogFile = true;
    bool                  EchoToConsole = true;

    // Simulated linked-GPU parameters.
    unsigned SimNodeCount   = 2;         // 1..8
    unsigned AdapterIndex   = UINT32_MAX; // UINT32_MAX ⇒ first non-software adapter

    // Feature toggles.
    bool CaptureDebugOutput = true;
    bool RunChild           = true;      // false when only the info panel is wanted
    bool InfoOnly           = false;     // synonym for RunChild=false + no child arg

    // Driver-level shim (D3D12Sim.dll injected into the child).  When enabled,
    // the child's D3D12 backend genuinely sees N linked GPU nodes and its
    // Create* calls are silently node-mask remapped down to the real single
    // physical node.  See DiligentGpuSimulation/shim/.
    bool UseDriverShim = false;

    // Absolute path to D3D12Sim.dll.  Empty ⇒ resolve automatically next to
    // SimulationApp.exe.
    std::filesystem::path ShimDllPath;

    // How often the GPU stats snapshot is written to the log while the child
    // is alive (in seconds).  Set to 0 or a huge value to disable.
    double RefreshSeconds = 2.0;

    // The shims check the child's use of the simulated nodes like linked
    // hardware would (DILIGENT_SIM_VALIDATION). A run with validation errors
    // ends with exit code 3 when the child itself succeeded.
    bool Validate = true;

    // One virtual DXGI adapter per node in front of the real list
    // (DILIGENT_SIM_VIRTUAL_ADAPTERS). Real linked hardware is one adapter, so off.
    bool VirtualAdapters = false;

    // D3D12 cross-node sharing tier reported to the child (0..3)
    unsigned CrossNodeTier = 1;
};

// Exit code of a run whose child succeeded but used the simulated nodes in a
// way linked hardware would reject
constexpr int kExitValidationErrors = 3;

// Parses argv into OutOpts. On error, returns false and populates OutErr.
// If the user passed -h/--help, OutShowHelp is set to true and the caller
// should call PrintHelp().
bool ParseCommandLine(int argc, wchar_t** argv,
                      SimulationOptions& OutOpts,
                      std::string&       OutErr,
                      bool&              OutShowHelp);

void PrintHelp();

class SimulationApp
{
public:
    SimulationApp();
    ~SimulationApp();

    SimulationApp(const SimulationApp&)            = delete;
    SimulationApp& operator=(const SimulationApp&) = delete;

    // Runs the tool according to Opts.  Returns the child's exit code, or a
    // negative value on host-side failure.
    int Run(const SimulationOptions& Opts);

private:
    // Resolves a user-supplied child.exe path to an existing absolute path,
    // searching (in order): as-is, cwd, this exe's directory, and PATH.
    static bool ResolveChildExe(const std::filesystem::path& Requested,
                                std::filesystem::path&       OutResolved,
                                std::string&                 OutError);

    static std::filesystem::path GetExecutableDir();
    static std::filesystem::path DefaultLogPath();
};

} // namespace SimApp
