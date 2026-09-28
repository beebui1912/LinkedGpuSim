/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "LinkedGpuSimulator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <sstream>

#include <pdhmsg.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")

namespace SimApp
{

namespace
{

std::string WideToUtf8(const wchar_t* Wide)
{
    if (Wide == nullptr)
        return {};
    const int Len = ::WideCharToMultiByte(CP_UTF8, 0, Wide, -1, nullptr, 0, nullptr, nullptr);
    if (Len <= 1)
        return {};
    std::string Out(static_cast<size_t>(Len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Out.data(), Len, nullptr, nullptr);
    return Out;
}

// Parses a PDH "GPU Engine" instance name such as
//   pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D
// and extracts the pid + adapter LUID.
bool ParseGpuEngineInstance(const wchar_t* Name, DWORD& Pid, uint32_t& LuidHigh, uint32_t& LuidLow)
{
    const wchar_t* pPid = std::wcsstr(Name, L"pid_");
    if (pPid == nullptr || swscanf_s(pPid, L"pid_%u", &Pid) != 1)
        return false;

    const wchar_t* pLuid = std::wcsstr(Name, L"luid_0x");
    unsigned int   High  = 0;
    unsigned int   Low   = 0;
    if (pLuid == nullptr || swscanf_s(pLuid, L"luid_0x%x_0x%x", &High, &Low) != 2)
        return false;

    LuidHigh = High;
    LuidLow  = Low;
    return true;
}

constexpr unsigned kMaxNodeCount = 8u;

} // namespace

LinkedGpuSimulator::LinkedGpuSimulator() = default;

LinkedGpuSimulator::~LinkedGpuSimulator()
{
    if (m_PdhQuery != nullptr)
    {
        PdhCloseQuery(m_PdhQuery);
        m_PdhQuery = nullptr;
    }
}

bool LinkedGpuSimulator::Initialize(unsigned NodeCount, unsigned PreferredAdapterIndex, std::string& OutErr)
{
    OutErr.clear();

    NodeCount = std::max(1u, std::min(NodeCount, kMaxNodeCount));

    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                    reinterpret_cast<void**>(m_Factory.GetAddressOf()));
    if (FAILED(hr))
    {
        std::ostringstream ss;
        ss << "CreateDXGIFactory1 failed with HRESULT 0x" << std::hex << hr;
        OutErr = ss.str();
        return false;
    }

    using Microsoft::WRL::ComPtr;

    m_HostAdapters.clear();
    m_Adapters.clear();

    for (UINT AdapterId = 0;; ++AdapterId)
    {
        ComPtr<IDXGIAdapter1> Adapter1;
        if (m_Factory->EnumAdapters1(AdapterId, Adapter1.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;

        DXGI_ADAPTER_DESC1 Desc{};
        Adapter1->GetDesc1(&Desc);

        HostAdapterInfo Info;
        Info.Index     = AdapterId;
        Info.Name      = WideToUtf8(Desc.Description);
        Info.VramTotal = static_cast<uint64_t>(Desc.DedicatedVideoMemory);
        Info.LuidHigh  = static_cast<uint32_t>(Desc.AdapterLuid.HighPart);
        Info.LuidLow   = static_cast<uint32_t>(Desc.AdapterLuid.LowPart);

        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            Info.TypeStr = "Software";
        else if (Desc.DedicatedVideoMemory >= static_cast<SIZE_T>(512) * 1024 * 1024)
            Info.TypeStr = "Discrete";
        else
            Info.TypeStr = "Integrated";

        ComPtr<IDXGIAdapter3> Adapter3;
        if (SUCCEEDED(Adapter1.As(&Adapter3)))
            m_Adapters.push_back(Adapter3);
        else
            m_Adapters.push_back(nullptr);

        m_HostAdapters.push_back(std::move(Info));
    }

    if (m_HostAdapters.empty())
    {
        OutErr = "No DXGI adapters found on this system.";
        return false;
    }

    // Skip software adapters when picking a "primary" adapter, unless the caller
    // explicitly asked for one.
    if (PreferredAdapterIndex < m_HostAdapters.size())
    {
        m_PrimaryIndex = PreferredAdapterIndex;
    }
    else
    {
        m_PrimaryIndex = 0;
        for (unsigned i = 0; i < m_HostAdapters.size(); ++i)
        {
            if (m_HostAdapters[i].TypeStr != "Software")
            {
                m_PrimaryIndex = i;
                break;
            }
        }
    }

    // Build the initial simulated group (static parts) before we have live stats.
    m_Group             = SimGpuGroup{};
    m_Group.Host        = m_HostAdapters[m_PrimaryIndex];
    m_Group.NodeCount   = NodeCount;
    m_Group.NodeMask    = (NodeCount >= 32) ? 0xFFFFFFFFu : ((1u << NodeCount) - 1u);
    RebuildNodes();

    // PDH per-process GPU utilization query. Best-effort: absence is not fatal.
    m_FilterPid = GetCurrentProcessId();
    if (PdhOpenQueryW(nullptr, 0, &m_PdhQuery) == ERROR_SUCCESS)
    {
        if (PdhAddEnglishCounterW(m_PdhQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_PdhCounter) == ERROR_SUCCESS)
        {
            PdhCollectQueryData(m_PdhQuery); // Prime the query.
            m_PdhReady  = true;
            m_PdhPrimed = false; // The next Refresh() produces the first valid delta.
        }
        else
        {
            PdhCloseQuery(m_PdhQuery);
            m_PdhQuery = nullptr;
        }
    }

    return true;
}

void LinkedGpuSimulator::Refresh()
{
    RefreshVram();
    RefreshUtilization();

    if (m_PrimaryIndex < m_HostAdapters.size())
        m_Group.Host = m_HostAdapters[m_PrimaryIndex];

    RebuildNodes();
}

void LinkedGpuSimulator::RefreshVram()
{
    for (size_t i = 0; i < m_HostAdapters.size(); ++i)
    {
        if (!m_Adapters[i])
            continue;

        DXGI_QUERY_VIDEO_MEMORY_INFO Mem{};
        if (SUCCEEDED(m_Adapters[i]->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Mem)))
        {
            m_HostAdapters[i].VramUsed    = Mem.CurrentUsage;
            m_HostAdapters[i].VramBudget  = Mem.Budget;
            m_HostAdapters[i].HasLiveVram = true;
        }
    }
}

void LinkedGpuSimulator::RefreshUtilization()
{
    // Reset before summing this frame's samples.
    for (HostAdapterInfo& Adapter : m_HostAdapters)
        Adapter.UtilPercent = 0.0;

    if (!m_PdhReady || m_PdhQuery == nullptr)
    {
        for (HostAdapterInfo& Adapter : m_HostAdapters)
            Adapter.UtilPercent = -1.0;
        return;
    }

    if (PdhCollectQueryData(m_PdhQuery) != ERROR_SUCCESS)
    {
        for (HostAdapterInfo& Adapter : m_HostAdapters)
            Adapter.UtilPercent = -1.0;
        return;
    }

    // The first successful collection primes the delta; the next one is valid.
    if (!m_PdhPrimed)
    {
        m_PdhPrimed = true;
        for (HostAdapterInfo& Adapter : m_HostAdapters)
            Adapter.UtilPercent = 0.0;
        return;
    }

    DWORD      BufferSize = 0;
    DWORD      ItemCount  = 0;
    PDH_STATUS Status     = PdhGetFormattedCounterArrayW(m_PdhCounter, PDH_FMT_DOUBLE, &BufferSize, &ItemCount, nullptr);
    if (Status != PDH_MORE_DATA || BufferSize == 0)
        return;

    m_PdhBuffer.resize(BufferSize);
    auto* pItems = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(m_PdhBuffer.data());
    Status       = PdhGetFormattedCounterArrayW(m_PdhCounter, PDH_FMT_DOUBLE, &BufferSize, &ItemCount, pItems);
    if (Status != ERROR_SUCCESS)
        return;

    for (DWORD i = 0; i < ItemCount; ++i)
    {
        if (pItems[i].FmtValue.CStatus != ERROR_SUCCESS)
            continue;

        DWORD    Pid = 0;
        uint32_t Hi  = 0;
        uint32_t Lo  = 0;
        if (!ParseGpuEngineInstance(pItems[i].szName, Pid, Hi, Lo))
            continue;
        if (m_FilterPid != 0 && Pid != m_FilterPid)
            continue;

        for (HostAdapterInfo& Adapter : m_HostAdapters)
        {
            if (Adapter.LuidHigh == Hi && Adapter.LuidLow == Lo)
            {
                Adapter.UtilPercent += pItems[i].FmtValue.doubleValue;
                break;
            }
        }
    }

    // Engine utilizations are summed; clamp to a sensible display range.
    for (HostAdapterInfo& Adapter : m_HostAdapters)
    {
        if (Adapter.UtilPercent > 100.0)
            Adapter.UtilPercent = 100.0;
    }
}

void LinkedGpuSimulator::RebuildNodes()
{
    const HostAdapterInfo& Host = m_Group.Host;
    const unsigned         N    = std::max(1u, m_Group.NodeCount);

    m_Group.Nodes.clear();
    m_Group.Nodes.reserve(N);

    // Divide the host's VRAM figures across virtual nodes; the last node absorbs
    // any rounding remainder so the totals still add up.
    const uint64_t TotalPer   = Host.VramTotal / N;
    const uint64_t TotalRem   = Host.VramTotal - TotalPer * N;
    const uint64_t UsedPer    = Host.VramUsed / N;
    const uint64_t UsedRem    = Host.VramUsed - UsedPer * N;
    const uint64_t BudgetPer  = Host.VramBudget / N;
    const uint64_t BudgetRem  = Host.VramBudget - BudgetPer * N;
    const double   UtilPer    = (Host.UtilPercent < 0.0) ? -1.0 : (Host.UtilPercent / static_cast<double>(N));

    for (unsigned v = 0; v < N; ++v)
    {
        SimNodeInfo Node;
        Node.Index       = v;
        Node.NodeMaskBit = 1u << v;
        Node.TypeStr     = "Simulated Linked Node";

        std::ostringstream ss;
        ss << Host.Name << " [Simulated Node " << v << "]";
        Node.Name = ss.str();

        const bool IsLast = (v == N - 1);
        Node.VramTotal    = TotalPer  + (IsLast ? TotalRem  : 0);
        Node.VramUsed     = UsedPer   + (IsLast ? UsedRem   : 0);
        Node.VramBudget   = BudgetPer + (IsLast ? BudgetRem : 0);
        Node.UtilPercent  = UtilPer;

        m_Group.Nodes.push_back(std::move(Node));
    }
}

} // namespace SimApp
