/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  ShimLog
//  -------
//  Thread-safe logger used by the shim DLL. Every message is:
//    - written via OutputDebugStringA (picked up by SimulationApp's
//      DebugOutputReader as [child.dbg]),
//    - and, if DILIGENT_SIM_LOG_FILE is set, appended to that file.

#pragma once

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

void LogInit();
void LogShutdown();

void LogInfo(const char* Fmt, ...);
void LogWarn(const char* Fmt, ...);
void LogError(const char* Fmt, ...);

// Fires only when DILIGENT_SIM_VERBOSE=1 is set.  Used for per-call tracing.
void LogVerbose(const char* Fmt, ...);

} // namespace D3D12Sim
