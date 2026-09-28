/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

//  GpuInfoConsole
//  --------------
//  Renders the LinkedGpuSimulator state as plain text (equivalent in intent to
//  DiligentSamples/Tutorials/Common/src/GpuInfoPanel.cpp's ImGui panel, but
//  purely console-based so the simulator has no Diligent runtime dependency).

#pragma once

#include <string>

#include "LinkedGpuSimulator.hpp"

namespace SimApp
{

class GpuInfoConsole
{
public:
    // "Simulation GPU (linked device group)" panel:
    //   header + host adapter summary + per-node breakdown.
    static std::string FormatGroupPanel(const SimGpuGroup& Group);

    // All physical adapters discovered on the host, one row each.
    static std::string FormatHostAdapters(const std::vector<HostAdapterInfo>& Hosts, unsigned PrimaryIndex);

    // Convenience: host adapters + simulated group in one string.
    static std::string FormatFullReport(const LinkedGpuSimulator& Sim);

    // Progress-bar style single line, e.g. "VRAM: [##########----------]  512 / 1024 MB".
    static std::string FormatVramLine(const char* Label, uint64_t Used, uint64_t Total);

    // Utility helpers used by the formatters (also handy for external callers).
    static std::string BytesToMB(uint64_t Bytes);
    static std::string LuidToString(uint32_t High, uint32_t Low);
};

} // namespace SimApp
