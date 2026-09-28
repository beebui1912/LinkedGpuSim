/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "GpuInfoConsole.hpp"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace SimApp
{

namespace
{

const char* kThickRule = "============================================================";
const char* kThinRule  = "------------------------------------------------------------";

std::string ProgressBar(double Frac, unsigned Width = 20)
{
    if (Frac < 0.0) Frac = 0.0;
    if (Frac > 1.0) Frac = 1.0;
    const unsigned Filled = static_cast<unsigned>(Frac * Width + 0.5);
    std::string Bar;
    Bar.reserve(static_cast<size_t>(Width) + 2);
    Bar.push_back('[');
    for (unsigned i = 0; i < Width; ++i)
        Bar.push_back(i < Filled ? '#' : '-');
    Bar.push_back(']');
    return Bar;
}

} // namespace

std::string GpuInfoConsole::BytesToMB(uint64_t Bytes)
{
    const double MB = static_cast<double>(Bytes) / (1024.0 * 1024.0);
    char Buf[64];
    std::snprintf(Buf, sizeof(Buf), "%.0f MB", MB);
    return Buf;
}

std::string GpuInfoConsole::LuidToString(uint32_t High, uint32_t Low)
{
    char Buf[64];
    std::snprintf(Buf, sizeof(Buf), "0x%08X_0x%08X", High, Low);
    return Buf;
}

std::string GpuInfoConsole::FormatVramLine(const char* Label, uint64_t Used, uint64_t Total)
{
    const double Frac = (Total > 0) ? (static_cast<double>(Used) / static_cast<double>(Total)) : 0.0;
    std::ostringstream ss;
    ss << Label << " " << ProgressBar(Frac)
       << "  " << BytesToMB(Used) << " / " << BytesToMB(Total);
    return ss.str();
}

std::string GpuInfoConsole::FormatGroupPanel(const SimGpuGroup& Group)
{
    std::ostringstream ss;
    const HostAdapterInfo& Host = Group.Host;

    ss << kThickRule << "\n";
    ss << " Simulation GPU (linked device group)\n";
    ss << kThickRule << "\n";
    ss << " Host adapter : " << (Host.Name.empty() ? "<unknown>" : Host.Name) << "\n";
    ss << " Adapter type : " << (Host.TypeStr.empty() ? "Unknown" : Host.TypeStr) << "\n";
    ss << " Adapter LUID : " << LuidToString(Host.LuidHigh, Host.LuidLow) << "\n";
    ss << " Node count   : " << Group.NodeCount << "  (NodeMask = 0x" << std::hex << std::uppercase
       << Group.NodeMask << std::dec << ")\n";

    if (Host.HasLiveVram && Host.VramTotal > 0)
        ss << " " << FormatVramLine("Host VRAM :", Host.VramUsed, Host.VramTotal) << "\n";
    else if (Host.VramTotal > 0)
        ss << " Host VRAM : " << BytesToMB(Host.VramTotal) << " total\n";
    else
        ss << " Host VRAM : N/A\n";

    if (Host.UtilPercent >= 0.0)
    {
        char UtilBuf[64];
        std::snprintf(UtilBuf, sizeof(UtilBuf), "%.0f%%", Host.UtilPercent);
        ss << " Host GPU  " << ProgressBar(Host.UtilPercent / 100.0) << "  " << UtilBuf << "\n";
    }
    else
    {
        ss << " Host GPU  : N/A\n";
    }

    for (const SimNodeInfo& Node : Group.Nodes)
    {
        ss << kThinRule << "\n";
        ss << " Node " << Node.Index << "  (bit 0x" << std::hex << std::uppercase << Node.NodeMaskBit
           << std::dec << ")  " << Node.TypeStr << "\n";
        ss << "   Name       : " << Node.Name << "\n";
        if (Node.VramTotal > 0)
            ss << "   " << FormatVramLine("VRAM share:", Node.VramUsed, Node.VramTotal) << "\n";
        else
            ss << "   VRAM share : N/A\n";

        if (Node.UtilPercent >= 0.0)
        {
            char UtilBuf[64];
            std::snprintf(UtilBuf, sizeof(UtilBuf), "%.0f%%", Node.UtilPercent);
            ss << "   GPU share  " << ProgressBar(Node.UtilPercent / 100.0) << "  " << UtilBuf << "\n";
        }
        else
        {
            ss << "   GPU share  : N/A\n";
        }
    }

    ss << kThickRule << "\n";
    return ss.str();
}

std::string GpuInfoConsole::FormatHostAdapters(const std::vector<HostAdapterInfo>& Hosts, unsigned PrimaryIndex)
{
    std::ostringstream ss;
    ss << kThickRule << "\n";
    ss << " Physical GPUs discovered on host (" << Hosts.size() << ")\n";
    ss << kThickRule << "\n";
    for (const HostAdapterInfo& H : Hosts)
    {
        const char* Marker = (H.Index == PrimaryIndex) ? "*" : " ";
        ss << " " << Marker << " [" << H.Index << "] " << H.Name
           << "  (" << (H.TypeStr.empty() ? "Unknown" : H.TypeStr) << ")\n";
        ss << "     LUID     : " << LuidToString(H.LuidHigh, H.LuidLow) << "\n";
        if (H.HasLiveVram && H.VramTotal > 0)
            ss << "     " << FormatVramLine("VRAM :", H.VramUsed, H.VramTotal) << "\n";
        else if (H.VramTotal > 0)
            ss << "     VRAM     : " << BytesToMB(H.VramTotal) << " total\n";
        else
            ss << "     VRAM     : N/A\n";

        if (H.UtilPercent >= 0.0)
        {
            char UtilBuf[64];
            std::snprintf(UtilBuf, sizeof(UtilBuf), "%.0f%%", H.UtilPercent);
            ss << "     GPU util : " << UtilBuf << "\n";
        }
    }
    ss << " (* = primary adapter used to build the simulated linked group)\n";
    ss << kThickRule << "\n";
    return ss.str();
}

std::string GpuInfoConsole::FormatFullReport(const LinkedGpuSimulator& Sim)
{
    std::ostringstream ss;
    ss << FormatHostAdapters(Sim.GetHostAdapters(), Sim.GetPrimaryIndex());
    ss << FormatGroupPanel(Sim.GetGroup());
    return ss.str();
}

} // namespace SimApp
