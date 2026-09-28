/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

//  ProcessLauncher
//  ---------------
//  Thin wrapper around CreateProcessW that:
//    * creates anonymous pipes for the child's stdout and stderr,
//    * lets the caller merge extra environment variables (DILIGENT_SIM_* etc.)
//      on top of the parent's environment,
//    * argv-style argument list is properly quoted per Windows conventions,
//    * exposes Wait/Terminate/IsRunning helpers.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace SimApp
{

struct EnvOverride
{
    std::wstring Name;
    std::wstring Value;
};

class ProcessLauncher
{
public:
    struct Options
    {
        std::wstring              ExePath;         // Path to child .exe
        std::vector<std::wstring> Args;            // Args passed to the child (excluding argv[0])
        std::wstring              WorkingDir;      // Empty = child's exe directory
        std::vector<EnvOverride>  EnvOverrides;    // Merged into the parent's environment
        bool                      CapturePipes = true;
        bool                      NewProcessGroup = true; // Enables Ctrl+Break isolation
        // When true, CreateProcess is invoked with CREATE_SUSPENDED so callers
        // can inject DLLs / patch memory before the child starts executing.
        // The caller must invoke Resume() after Start() succeeds.
        bool                      CreateSuspended = false;
    };

    ProcessLauncher();
    ~ProcessLauncher();

    ProcessLauncher(const ProcessLauncher&)            = delete;
    ProcessLauncher& operator=(const ProcessLauncher&) = delete;

    // Launches the child. Returns false and populates OutError on failure.
    bool Start(const Options& Opts, std::string& OutError);

    bool  IsRunning() const;
    DWORD GetPid() const { return m_ProcInfo.dwProcessId; }

    // Handles the caller must read from. Ownership stays with ProcessLauncher.
    HANDLE GetStdoutRead() const { return m_hStdoutRead; }
    HANDLE GetStderrRead() const { return m_hStderrRead; }

    // Process/thread handles for callers that need to inject code before the
    // child runs (must have been started with CreateSuspended=true).
    HANDLE GetProcessHandle() const { return m_ProcInfo.hProcess; }
    HANDLE GetMainThreadHandle() const { return m_ProcInfo.hThread; }

    // Wakes up a CreateSuspended-launched process. Returns true if it was
    // suspended and successfully resumed. No-op for already-running children.
    bool Resume();

    // Blocks until the child exits.  Returns its exit code (or -1 if unavailable).
    int Wait();

    // Best-effort forceful termination.  Safe to call on an already-exited child.
    void Terminate(unsigned ExitCode = 1);

    // Reconstructs the escaped command line for logging/diagnostics.
    static std::wstring BuildCommandLine(const std::wstring& ExePath, const std::vector<std::wstring>& Args);

private:
    void CloseAllHandles();
    void CloseWriteEnds();

    HANDLE              m_hStdoutRead  = nullptr;
    HANDLE              m_hStdoutWrite = nullptr;
    HANDLE              m_hStderrRead  = nullptr;
    HANDLE              m_hStderrWrite = nullptr;
    PROCESS_INFORMATION m_ProcInfo{};
    bool                m_Started = false;
};

} // namespace SimApp
