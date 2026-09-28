/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  ShimInjector
//  ------------
//  Loads D3D12Sim.dll (or any other DLL) into a suspended child process
//  using the standard CreateRemoteThread(LoadLibraryW) technique.
//  Called by SimulationApp between ProcessLauncher::Start() and Resume().

#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace SimApp
{

// Injects DllPath into hProcess.  The DLL is loaded via LoadLibraryW running
// on a temporary remote thread.  Returns true if the remote LoadLibrary call
// completed with a non-zero (truncated) HMODULE.
//
// The child must already have kernel32.dll mapped at the same address as this
// process (guaranteed on Windows for the standard system DLLs), so the local
// GetProcAddress(LoadLibraryW) result can be used as the remote thread's
// start routine.
bool InjectDll(HANDLE hProcess, const std::wstring& DllPath, std::string& OutError);

} // namespace SimApp
