/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "LayerConfig.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include "LayerLog.hpp"

namespace VkSim
{

namespace
{

std::once_flag        g_InitFlag;
LayerConfig           g_Config;
std::atomic<unsigned> g_ValidationErrors{0};
constexpr unsigned    kMaxReportedErrors = 50;

std::wstring GetEnvW(const wchar_t* Name)
{
    wchar_t     Buf[256];
    const DWORD Len = ::GetEnvironmentVariableW(Name, Buf, static_cast<DWORD>(std::size(Buf)));
    if (Len == 0 || Len >= std::size(Buf))
        return {};
    return std::wstring{Buf, Len};
}

void InitConfig()
{
    const std::wstring Count = GetEnvW(L"DILIGENT_SIM_LINKED_NODE_COUNT");
    unsigned long      N     = Count.empty() ? 2 : std::wcstoul(Count.c_str(), nullptr, 10);
    g_Config.NodeCount       = static_cast<uint32_t>(N < 1 ? 1 : (N > 8 ? 8 : N));

    const std::wstring Validate = GetEnvW(L"DILIGENT_SIM_VALIDATION");
    g_Config.Validate           = Validate.empty() || std::wcstoul(Validate.c_str(), nullptr, 10) != 0;

    // "0xHIGH_0xLOW"; the LUID bytes are the Windows LUID struct (LowPart first)
    const std::wstring Luid = GetEnvW(L"DILIGENT_SIM_HOST_ADAPTER_LUID");
    const size_t       Sep  = Luid.find(L'_');
    if (Sep != std::wstring::npos)
    {
        LUID L{};
        L.HighPart = static_cast<LONG>(std::wcstoul(Luid.c_str(), nullptr, 16));
        L.LowPart  = static_cast<DWORD>(std::wcstoul(Luid.c_str() + Sep + 1, nullptr, 16));
        static_assert(sizeof(L) == VK_LUID_SIZE, "LUID size");
        std::memcpy(g_Config.HostLuid, &L, VK_LUID_SIZE);
        g_Config.HasHostLuid = true;
    }
}

} // namespace

const LayerConfig& GetConfig()
{
    std::call_once(g_InitFlag, InitConfig);
    return g_Config;
}

void ReportValidationError(const char* Fmt, ...)
{
    const unsigned Index = g_ValidationErrors.fetch_add(1) + 1;
    if (Index > kMaxReportedErrors)
        return;
    char    Buf[1024];
    va_list ap;
    va_start(ap, Fmt);
    std::vsnprintf(Buf, sizeof(Buf), Fmt, ap);
    va_end(ap);
    LogError("VALIDATION ERROR: %s", Buf);
    std::fprintf(stderr, "[shim-vk] VALIDATION ERROR: %s\n", Buf);
    if (Index == kMaxReportedErrors)
        LogError("VALIDATION ERROR: further messages are suppressed (still counted)");
}

unsigned GetValidationErrorCount() { return g_ValidationErrors.load(); }

} // namespace VkSim

// Number of validation errors so far (for test harnesses running with the layer)
extern "C" __declspec(dllexport) unsigned VkSim_GetValidationErrorCount()
{
    return VkSim::GetValidationErrorCount();
}
