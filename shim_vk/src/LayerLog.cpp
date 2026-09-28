/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  Small logger for the Vulkan layer.  Mirrors ShimLog in the D3D12 shim so
//  parent SimulationApp's DBWIN capture picks lines up as [child.dbg]
//  automatically.

#include "LayerLog.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace VkSim
{

namespace
{

std::mutex g_Mutex;
FILE*      g_File     = nullptr;
bool       g_Inited   = false;
bool       g_Verbose  = false;
bool       g_FailedOpen = false;

std::wstring GetEnvW(const wchar_t* Name)
{
    wchar_t Buf[4096];
    const DWORD Len = ::GetEnvironmentVariableW(Name, Buf, static_cast<DWORD>(std::size(Buf)));
    if (Len == 0 || Len >= std::size(Buf))
        return {};
    return std::wstring{Buf, Len};
}

std::string NowStamp()
{
    using namespace std::chrono;
    const auto Now = system_clock::now();
    const auto Sec = time_point_cast<seconds>(Now);
    const auto Ms  = duration_cast<milliseconds>(Now - Sec).count();
    const std::time_t T = system_clock::to_time_t(Now);
    std::tm L{};
    ::localtime_s(&L, &T);
    char Buf[32];
    std::snprintf(Buf, sizeof(Buf), "%02d:%02d:%02d.%03lld",
                  L.tm_hour, L.tm_min, L.tm_sec, static_cast<long long>(Ms));
    return Buf;
}

void OpenFileIfNeeded()
{
    if (g_Inited || g_FailedOpen) return;
    const std::wstring LogPath = GetEnvW(L"DILIGENT_SIM_LOG_FILE");
    g_Verbose = !GetEnvW(L"DILIGENT_SIM_VERBOSE").empty();
    if (!LogPath.empty())
    {
        g_File = _wfsopen(LogPath.c_str(), L"ab", _SH_DENYNO);
        if (g_File == nullptr) g_FailedOpen = true;
    }
    g_Inited = true;
}

void Emit(const char* Level, const char* Buf)
{
    char DbgLine[2048];
    std::snprintf(DbgLine, sizeof(DbgLine), "[shim-vk] [%s] %s\n", Level, Buf);
    ::OutputDebugStringA(DbgLine);

    std::lock_guard<std::mutex> Lock(g_Mutex);
    OpenFileIfNeeded();
    if (g_File != nullptr)
    {
        std::fprintf(g_File, "[%s] [shim-vk] [%s] %s\n", NowStamp().c_str(), Level, Buf);
        std::fflush(g_File);
    }
}

void EmitV(const char* Level, const char* Fmt, va_list ap)
{
    char Buf[2048];
    std::vsnprintf(Buf, sizeof(Buf), Fmt, ap);
    Emit(Level, Buf);
}

} // namespace

void LogInit()
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    OpenFileIfNeeded();
}

void LogShutdown()
{
    std::lock_guard<std::mutex> Lock(g_Mutex);
    if (g_File != nullptr)
    {
        std::fflush(g_File);
        std::fclose(g_File);
        g_File = nullptr;
    }
}

void LogInfo(const char* Fmt, ...)    { va_list ap; va_start(ap, Fmt); EmitV("info",  Fmt, ap); va_end(ap); }
void LogWarn(const char* Fmt, ...)    { va_list ap; va_start(ap, Fmt); EmitV("warn",  Fmt, ap); va_end(ap); }
void LogError(const char* Fmt, ...)   { va_list ap; va_start(ap, Fmt); EmitV("error", Fmt, ap); va_end(ap); }
void LogVerbose(const char* Fmt, ...) { if (!g_Verbose) return; va_list ap; va_start(ap, Fmt); EmitV("trace", Fmt, ap); va_end(ap); }

} // namespace VkSim
