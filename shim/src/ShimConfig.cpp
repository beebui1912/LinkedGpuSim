/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "ShimConfig.hpp"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>

namespace D3D12Sim
{

namespace
{

std::once_flag       g_InitFlag;
ShimConfig           g_Config;

std::wstring GetEnvW(const wchar_t* Name)
{
    wchar_t Buf[4096];
    const DWORD Len = ::GetEnvironmentVariableW(Name, Buf, static_cast<DWORD>(std::size(Buf)));
    if (Len == 0 || Len >= std::size(Buf))
        return {};
    return std::wstring{Buf, Len};
}

unsigned ParseUInt(const std::wstring& S, unsigned Default)
{
    if (S.empty())
        return Default;
    wchar_t* End = nullptr;
    const unsigned long V = std::wcstoul(S.c_str(), &End,
                                         (S.size() > 2 && S[0] == L'0' && (S[1] == L'x' || S[1] == L'X')) ? 16 : 10);
    if (End == S.c_str())
        return Default;
    return static_cast<unsigned>(V);
}

void InitConfig()
{
    const std::wstring Count = GetEnvW(L"DILIGENT_SIM_LINKED_NODE_COUNT");
    unsigned N = ParseUInt(Count, 2);
    if (N < 1) N = 1;
    if (N > 8) N = 8;
    g_Config.SimNodeCount = N;

    const std::wstring MaskEnv = GetEnvW(L"DILIGENT_SIM_LINKED_NODE_MASK");
    if (!MaskEnv.empty())
        g_Config.SimNodeMask = ParseUInt(MaskEnv, (1u << N) - 1u);
    else
        g_Config.SimNodeMask = (N >= 32) ? 0xFFFFFFFFu : ((1u << N) - 1u);

    g_Config.LogFilePath = GetEnvW(L"DILIGENT_SIM_LOG_FILE");
    g_Config.ParentPid   = ParseUInt(GetEnvW(L"DILIGENT_SIM_PARENT_PID"), 0);
    g_Config.Verbose     = ParseUInt(GetEnvW(L"DILIGENT_SIM_VERBOSE"), 0) != 0;
    g_Config.Validate    = ParseUInt(GetEnvW(L"DILIGENT_SIM_VALIDATION"), 1) != 0;

    g_Config.VirtualAdapters      = ParseUInt(GetEnvW(L"DILIGENT_SIM_VIRTUAL_ADAPTERS"), 0) != 0;
    g_Config.CrossNodeSharingTier = ParseUInt(GetEnvW(L"DILIGENT_SIM_CROSS_NODE_TIER"), 1);

    // "0xHIGH_0xLOW" as written by SimulationApp
    const std::wstring Luid = GetEnvW(L"DILIGENT_SIM_HOST_ADAPTER_LUID");
    const size_t       Sep  = Luid.find(L'_');
    if (Sep != std::wstring::npos)
    {
        wchar_t*            End  = nullptr;
        const unsigned long High = std::wcstoul(Luid.c_str(), &End, 16);
        const unsigned long Low  = std::wcstoul(Luid.c_str() + Sep + 1, &End, 16);
        g_Config.HostLuid.HighPart = static_cast<LONG>(High);
        g_Config.HostLuid.LowPart  = static_cast<DWORD>(Low);
        g_Config.HasHostLuid       = true;
    }
}

} // namespace

const ShimConfig& GetConfig()
{
    std::call_once(g_InitFlag, InitConfig);
    return g_Config;
}

std::string NodeMaskToHex(unsigned Mask)
{
    char Buf[16];
    std::snprintf(Buf, sizeof(Buf), "0x%X", Mask);
    return Buf;
}

} // namespace D3D12Sim
