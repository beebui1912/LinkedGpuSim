/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

//  LinkedGpuSimulator
//  ------------------
//  Enumerates the physical GPUs on the host with DXGI + PDH (mirroring the
//  strategy used by DiligentSamples' GpuInfoPanel.cpp) and presents ONE physical
//  adapter as a "linked device group" of N virtual nodes.  This lets the
//  SimulationApp show what a Diligent linked multi-GPU device (GPU_MODE_LINKED)
//  would look like on a system that only exposes a single GPU node.
//
//  The class is deliberately independent from the Diligent Engine build so the
//  simulator can be compiled standalone.

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
#include <dxgi1_4.h>
#include <pdh.h>
#include <wrl/client.h>

namespace SimApp
{

// Static (or slowly-changing) info about one virtual linked-GPU node.
struct SimNodeInfo
{
    unsigned    Index      = 0;              // 0..NodeCount-1
    uint32_t    NodeMaskBit = 1u;            // 1u << Index
    std::string Name;                        // "<host adapter> [Simulated Node k]"
    std::string TypeStr    = "Simulated Linked Node";
    uint64_t    VramTotal  = 0;              // Share of the host adapter's total VRAM.
    uint64_t    VramUsed   = 0;              // Share of live per-process VRAM usage.
    uint64_t    VramBudget = 0;              // Share of live budget.
    double      UtilPercent = -1.0;          // < 0 means "not available".
};

// Info for one physical adapter reported by DXGI on this machine.
struct HostAdapterInfo
{
    unsigned    Index       = 0;
    std::string Name;
    std::string TypeStr;                 // "Discrete", "Integrated", "Software", "Unknown"
    uint64_t    VramTotal   = 0;         // DedicatedVideoMemory
    uint64_t    VramUsed    = 0;         // Live per-process usage (LOCAL segment).
    uint64_t    VramBudget  = 0;         // Live per-process budget.
    double      UtilPercent = -1.0;      // Aggregate per-process GPU engine util.
    uint32_t    LuidHigh    = 0;
    uint32_t    LuidLow     = 0;
    bool        HasLiveVram = false;
};

// Simulated linked-adapter group built on top of ONE host adapter.
struct SimGpuGroup
{
    HostAdapterInfo          Host;
    unsigned                 NodeCount = 1;
    uint32_t                 NodeMask  = 1u;   // (1u << NodeCount) - 1u
    std::vector<SimNodeInfo> Nodes;
};

class LinkedGpuSimulator
{
public:
    LinkedGpuSimulator();
    ~LinkedGpuSimulator();

    LinkedGpuSimulator(const LinkedGpuSimulator&)            = delete;
    LinkedGpuSimulator& operator=(const LinkedGpuSimulator&) = delete;

    // Initializes DXGI + PDH, enumerates host adapters, and prepares the
    // simulated linked-GPU group over PreferredAdapterIndex (clamped to the
    // discovered adapter count).  NodeCount is clamped to >= 1 and <= 8.
    // Returns false with a message on failure.
    bool Initialize(unsigned NodeCount, unsigned PreferredAdapterIndex, std::string& OutErr);

    // Sets the process id used to filter PDH "GPU Engine" per-process counters.
    // Defaults to the parent (this) process id after Initialize().
    void SetFilterPid(DWORD Pid) { m_FilterPid = Pid; }
    DWORD GetFilterPid() const { return m_FilterPid; }

    // Re-queries live VRAM + GPU utilization and re-derives the per-node values.
    // Safe to call many times per second (the underlying PDH query is cheap).
    void Refresh();

    const std::vector<HostAdapterInfo>& GetHostAdapters() const { return m_HostAdapters; }
    const SimGpuGroup&                  GetGroup() const { return m_Group; }
    unsigned                            GetPrimaryIndex() const { return m_PrimaryIndex; }
    unsigned                            GetNodeCount() const { return m_Group.NodeCount; }

private:
    void RefreshVram();
    void RefreshUtilization();
    void RebuildNodes();

    Microsoft::WRL::ComPtr<IDXGIFactory4>              m_Factory;
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter3>> m_Adapters;

    std::vector<HostAdapterInfo> m_HostAdapters;
    SimGpuGroup                  m_Group;
    unsigned                     m_PrimaryIndex = 0;

    PDH_HQUERY   m_PdhQuery   = nullptr;
    PDH_HCOUNTER m_PdhCounter = nullptr;
    bool         m_PdhReady   = false;
    bool         m_PdhPrimed  = false;
    DWORD        m_FilterPid  = 0;

    std::vector<unsigned char> m_PdhBuffer;
};

} // namespace SimApp
