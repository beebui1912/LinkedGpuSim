/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "ShimLog.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "ShimConfig.hpp"

namespace D3D12Sim
{

namespace
{

std::mutex g_Mutex;
FILE*      g_File = nullptr;
bool       g_LogInited = false;
bool       g_LogFailed = false;

std::string NowStamp()
{
    using namespace std::chrono;
    const auto Now      = system_clock::now();
    const auto Sec      = time_point_cast<seconds>(Now);
    const auto Ms       = duration_cast<milliseconds>(Now - Sec).count();
    const std::time_t T = system_clock::to_time_t(Now);
    std::tm Local{};
    ::localtime_s(&Local, &T);
    char Buf[32];
    std::snprintf(Buf, sizeof(Buf), "%02d:%02d:%02d.%03lld",
                  Local.tm_hour, Local.tm_min, Local.tm_sec,
                  static_cast<long long>(Ms));
    return Buf;
}

void OpenFileIfNeeded()
{
    if (g_LogInited || g_LogFailed)
        return;
    const auto& Cfg = GetConfig();
    if (!Cfg.LogFilePath.empty())
    {
        // Open append-shared so SimulationApp's LogSink can hold the file too.
        g_File = _wfsopen(Cfg.LogFilePath.c_str(), L"ab", _SH_DENYNO);
        if (g_File == nullptr)
            g_LogFailed = true;
    }
    g_LogInited = true;
}

void Emit(const char* Level, const char* Buf)
{
    // OutputDebugString path - visible in DebugView and via SimulationApp's DBWIN reader.
    char DbgLine[2048];
    std::snprintf(DbgLine, sizeof(DbgLine), "[shim] [%s] %s\n", Level, Buf);
    ::OutputDebugStringA(DbgLine);

    std::lock_guard<std::mutex> Lock(g_Mutex);
    OpenFileIfNeeded();
    if (g_File != nullptr)
    {
        std::fprintf(g_File, "[%s] [shim] [%s] %s\n", NowStamp().c_str(), Level, Buf);
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
    // Force-open the file now so any early failure is deterministic.
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

void LogInfo(const char* Fmt, ...)
{
    va_list ap;
    va_start(ap, Fmt);
    EmitV("info", Fmt, ap);
    va_end(ap);
}

void LogWarn(const char* Fmt, ...)
{
    va_list ap;
    va_start(ap, Fmt);
    EmitV("warn", Fmt, ap);
    va_end(ap);
}

void LogError(const char* Fmt, ...)
{
    va_list ap;
    va_start(ap, Fmt);
    EmitV("error", Fmt, ap);
    va_end(ap);
}

void LogVerbose(const char* Fmt, ...)
{
    if (!GetConfig().Verbose)
        return;
    va_list ap;
    va_start(ap, Fmt);
    EmitV("trace", Fmt, ap);
    va_end(ap);
}

} // namespace D3D12Sim
