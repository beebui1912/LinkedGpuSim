/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "SimulationApp.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "GpuInfoConsole.hpp"
#include "LinkedGpuSimulator.hpp"
#include "LogCapture.hpp"
#include "ProcessLauncher.hpp"
#include "ShimInjector.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace SimApp
{

namespace
{

// A single, process-wide shutdown flag used by the Ctrl+C handler + Run loop.
std::atomic<bool> g_ShutdownRequested{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD CtrlType)
{
    switch (CtrlType)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_ShutdownRequested.store(true, std::memory_order_release);
            return TRUE;
        default:
            return FALSE;
    }
}

std::string WideToUtf8(const std::wstring& W)
{
    if (W.empty())
        return {};
    const int Len = ::WideCharToMultiByte(CP_UTF8, 0, W.c_str(), static_cast<int>(W.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (Len <= 0)
        return {};
    std::string Out(static_cast<size_t>(Len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, W.c_str(), static_cast<int>(W.size()),
                          Out.data(), Len, nullptr, nullptr);
    return Out;
}

std::wstring Utf8ToWide(const std::string& S)
{
    if (S.empty())
        return {};
    const int Len = ::MultiByteToWideChar(CP_UTF8, 0, S.c_str(), static_cast<int>(S.size()),
                                          nullptr, 0);
    if (Len <= 0)
        return {};
    std::wstring Out(static_cast<size_t>(Len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, S.c_str(), static_cast<int>(S.size()),
                          Out.data(), Len);
    return Out;
}

bool EqualsAny(const wchar_t* Arg, std::initializer_list<const wchar_t*> Aliases)
{
    for (const wchar_t* A : Aliases)
    {
        if (std::wcscmp(Arg, A) == 0)
            return true;
    }
    return false;
}

bool ParseUInt(const wchar_t* S, unsigned& Out)
{
    if (S == nullptr || *S == L'\0')
        return false;
    wchar_t* End = nullptr;
    const unsigned long V = std::wcstoul(S, &End, 10);
    if (End == S || (End != nullptr && *End != L'\0'))
        return false;
    Out = static_cast<unsigned>(V);
    return true;
}

bool ParseDouble(const wchar_t* S, double& Out)
{
    if (S == nullptr || *S == L'\0')
        return false;
    wchar_t* End = nullptr;
    const double V = std::wcstod(S, &End);
    if (End == S || (End != nullptr && *End != L'\0'))
        return false;
    Out = V;
    return true;
}

} // namespace


// ---------------------------- CLI parsing ---------------------------------

void PrintHelp()
{
    static const char* kHelp =
        "\n"
        "SimulationApp - Diligent linked multi-GPU simulator launcher\n"
        "\n"
        "USAGE:\n"
        "  SimulationApp.exe [options] [--] <child.exe> [child-args...]\n"
        "\n"
        "  Launches <child.exe> as a subprocess, presents a simulated 'linked\n"
        "  device group' built from a single physical GPU, and tees the child's\n"
        "  stdout/stderr and OutputDebugString messages to the console and to a\n"
        "  log file.\n"
        "\n"
        "OPTIONS:\n"
        "  -n, --nodes N          Number of simulated linked-GPU nodes (1..8).\n"
        "                         Default: 2.\n"
        "  -a, --adapter INDEX    Index of the host DXGI adapter to base the\n"
        "                         simulated group on (default: first non-software).\n"
        "  -l, --log PATH         Path to the log file (UTF-8, BOM-prefixed).\n"
        "                         Default: <exe-dir>\\SimulationApp.log\n"
        "      --no-log           Do not write a log file (console only).\n"
        "      --no-debug-capture Skip OutputDebugString (DebugView-style) capture.\n"
        "      --info-only        Print the GPU info panel and exit.\n"
        "      --driver-shim      Inject D3D12Sim.dll into the child so its D3D12\n"
        "                         backend genuinely reports N linked GPU nodes.\n"
        "                         Requires the child to run with the D3D12 backend\n"
        "                         (e.g. Tutorial31 -mode D3D12).\n"
        "      --no-driver-shim   Disable the driver-level shim (default).\n"
        "      --shim-dll PATH    Explicit path to D3D12Sim.dll; implies\n"
        "                         --driver-shim. Default: next to SimulationApp.exe.\n"
        "      --no-validation    Do not check the child's node masks and device masks\n"
        "                         (by default a run whose child succeeds but uses the\n"
        "                         simulated nodes invalidly exits with code 3).\n"
        "      --virtual-adapters Also list one virtual DXGI adapter per node (for UIs\n"
        "                         that show one entry per GPU). Real linked hardware is\n"
        "                         one adapter with several nodes, so this is off.\n"
        "      --cross-node-tier N  D3D12 cross-node sharing tier reported (0..3, default 1).\n"
        "      --refresh SEC      GPU stats refresh interval in seconds\n"
        "                         (0 to disable periodic snapshots). Default: 2.0.\n"
        "      --cwd DIR          Working directory of the child ('.' = this one).\n"
        "                         Default: the child's executable directory.\n"
        "  -h, --help             Show this help.\n"
        "\n"
        "EXAMPLES:\n"
        "  SimulationApp.exe Tutorial31_LinkedMultiGPU.exe\n"
        "  SimulationApp.exe -n 4 Tutorial31_LinkedMultiGPU.exe --mode D3D12\n"
        "  SimulationApp.exe --info-only\n"
        "\n"
        "ENVIRONMENT VARIABLES SET FOR THE CHILD:\n"
        "  DILIGENT_SIM_LINKED_NODE_COUNT   Number of simulated linked nodes.\n"
        "  DILIGENT_SIM_LINKED_NODE_MASK    Bitmask (hex) of the simulated nodes.\n"
        "  DILIGENT_SIM_HOST_ADAPTER        Name of the host adapter.\n"
        "  DILIGENT_SIM_HOST_ADAPTER_LUID   Host adapter LUID (HIGH_LOW hex).\n"
        "  DILIGENT_SIM_LOG_FILE            Path to the shared log file.\n"
        "  DILIGENT_SIM_PARENT_PID          Parent (SimulationApp) PID.\n"
        "  DILIGENT_SIM_VALIDATION          0 with --no-validation.\n"
        "  DILIGENT_SIM_VIRTUAL_ADAPTERS    1 with --virtual-adapters.\n"
        "  DILIGENT_SIM_CROSS_NODE_TIER     --cross-node-tier.\n"
        "  VK_ADD_LAYER_PATH, VK_INSTANCE_LAYERS   with --driver-shim: the Vulkan layer.\n"
        "\n";
    std::fputs(kHelp, stdout);
}

bool ParseCommandLine(int argc, wchar_t** argv,
                      SimulationOptions& OutOpts,
                      std::string&       OutErr,
                      bool&              OutShowHelp)
{
    OutShowHelp = false;
    OutErr.clear();

    if (argc <= 1)
    {
        OutShowHelp = true;
        return true;
    }

    int i = 1;
    bool SawSeparator = false;
    for (; i < argc; ++i)
    {
        const wchar_t* Arg = argv[i];

        if (!SawSeparator && Arg[0] == L'-' && Arg[1] != L'\0')
        {
            if (EqualsAny(Arg, {L"-h", L"--help", L"/?", L"/help"}))
            {
                OutShowHelp = true;
                return true;
            }
            if (EqualsAny(Arg, {L"-n", L"--nodes"}))
            {
                if (i + 1 >= argc) { OutErr = "--nodes requires an integer."; return false; }
                unsigned N = 0;
                if (!ParseUInt(argv[++i], N)) { OutErr = "Invalid value for --nodes."; return false; }
                if (N < 1 || N > 8) { OutErr = "--nodes must be between 1 and 8."; return false; }
                OutOpts.SimNodeCount = N;
                continue;
            }
            if (EqualsAny(Arg, {L"-a", L"--adapter"}))
            {
                if (i + 1 >= argc) { OutErr = "--adapter requires an integer."; return false; }
                unsigned N = 0;
                if (!ParseUInt(argv[++i], N)) { OutErr = "Invalid value for --adapter."; return false; }
                OutOpts.AdapterIndex = N;
                continue;
            }
            if (EqualsAny(Arg, {L"-l", L"--log"}))
            {
                if (i + 1 >= argc) { OutErr = "--log requires a path."; return false; }
                OutOpts.LogFile      = std::filesystem::path{argv[++i]};
                OutOpts.WriteLogFile = true;
                continue;
            }
            if (EqualsAny(Arg, {L"--no-log"}))
            {
                OutOpts.WriteLogFile = false;
                OutOpts.LogFile.clear();
                continue;
            }
            if (EqualsAny(Arg, {L"--no-debug-capture"}))
            {
                OutOpts.CaptureDebugOutput = false;
                continue;
            }
            if (EqualsAny(Arg, {L"--driver-shim"}))
            {
                OutOpts.UseDriverShim = true;
                continue;
            }
            if (EqualsAny(Arg, {L"--no-driver-shim"}))
            {
                OutOpts.UseDriverShim = false;
                continue;
            }
            if (EqualsAny(Arg, {L"--shim-dll"}))
            {
                if (i + 1 >= argc) { OutErr = "--shim-dll requires a path."; return false; }
                OutOpts.ShimDllPath   = std::filesystem::path{argv[++i]};
                OutOpts.UseDriverShim = true;
                continue;
            }
            if (EqualsAny(Arg, {L"--no-validation"}))
            {
                OutOpts.Validate = false;
                continue;
            }
            if (EqualsAny(Arg, {L"--virtual-adapters"}))
            {
                OutOpts.VirtualAdapters = true;
                continue;
            }
            if (EqualsAny(Arg, {L"--cross-node-tier"}))
            {
                if (i + 1 >= argc) { OutErr = "--cross-node-tier requires 0..3."; return false; }
                unsigned N = 0;
                if (!ParseUInt(argv[++i], N) || N > 3) { OutErr = "--cross-node-tier must be between 0 and 3."; return false; }
                OutOpts.CrossNodeTier = N;
                continue;
            }
            if (EqualsAny(Arg, {L"--info-only"}))
            {
                OutOpts.InfoOnly = true;
                OutOpts.RunChild = false;
                continue;
            }
            if (EqualsAny(Arg, {L"--refresh"}))
            {
                if (i + 1 >= argc) { OutErr = "--refresh requires a number of seconds."; return false; }
                double V = 0.0;
                if (!ParseDouble(argv[++i], V)) { OutErr = "Invalid value for --refresh."; return false; }
                OutOpts.RefreshSeconds = V;
                continue;
            }
            if (EqualsAny(Arg, {L"--cwd"}))
            {
                if (i + 1 >= argc) { OutErr = "--cwd requires a directory."; return false; }
                std::error_code Ec;
                OutOpts.ChildWorkingDir = std::filesystem::absolute(std::filesystem::path{argv[++i]}, Ec);
                if (Ec || !std::filesystem::is_directory(OutOpts.ChildWorkingDir)) { OutErr = "--cwd: not a directory."; return false; }
                continue;
            }
            if (std::wcscmp(Arg, L"--") == 0)
            {
                SawSeparator = true;
                continue;
            }

            std::ostringstream ss;
            ss << "Unknown option: " << WideToUtf8(Arg);
            OutErr = ss.str();
            return false;
        }

        // First positional argument is the child executable, everything after
        // it is passed through to the child.
        OutOpts.ChildExe = std::filesystem::path{Arg};
        OutOpts.RunChild = !OutOpts.InfoOnly;
        for (int j = i + 1; j < argc; ++j)
            OutOpts.ChildArgs.emplace_back(argv[j]);
        return true;
    }

    // Fell off the end of the loop: no positional child was provided.
    // That's fine only if --info-only was requested.
    if (!OutOpts.InfoOnly)
    {
        OutErr = "No child executable specified. Pass one, or use --info-only.";
        return false;
    }
    OutOpts.RunChild = false;
    return true;
}


// ---------------------------- SimulationApp -------------------------------

SimulationApp::SimulationApp()  = default;
SimulationApp::~SimulationApp() = default;

std::filesystem::path SimulationApp::GetExecutableDir()
{
    wchar_t Buf[MAX_PATH]{};
    DWORD   Len = ::GetModuleFileNameW(nullptr, Buf, MAX_PATH);
    if (Len == 0 || Len >= MAX_PATH)
        return {};
    std::filesystem::path P{Buf};
    return P.parent_path();
}

std::filesystem::path SimulationApp::DefaultLogPath()
{
    std::filesystem::path Dir = GetExecutableDir();
    if (Dir.empty())
        Dir = std::filesystem::current_path();
    return Dir / L"SimulationApp.log";
}

bool SimulationApp::ResolveChildExe(const std::filesystem::path& Requested,
                                    std::filesystem::path&       OutResolved,
                                    std::string&                 OutError)
{
    OutError.clear();
    if (Requested.empty())
    {
        OutError = "No child executable specified.";
        return false;
    }

    std::error_code Ec;

    auto TryPath = [&](const std::filesystem::path& P) {
        if (P.empty()) return false;
        if (std::filesystem::exists(P, Ec) && std::filesystem::is_regular_file(P, Ec))
        {
            OutResolved = std::filesystem::absolute(P, Ec);
            if (Ec) OutResolved = P;
            return true;
        }
        return false;
    };

    // 1) As given (absolute or relative to CWD).
    if (TryPath(Requested))
        return true;

    // 2) Only for bare filenames (no directory component), search common places.
    if (!Requested.has_parent_path())
    {
        // Same directory as this exe.
        const std::filesystem::path ExeDir = GetExecutableDir();
        if (!ExeDir.empty() && TryPath(ExeDir / Requested))
            return true;

        // PATH.
        wchar_t Found[MAX_PATH]{};
        DWORD   Len = ::SearchPathW(nullptr, Requested.wstring().c_str(), L".exe",
                                    MAX_PATH, Found, nullptr);
        if (Len == 0)
            Len = ::SearchPathW(nullptr, Requested.wstring().c_str(), nullptr,
                                MAX_PATH, Found, nullptr);
        if (Len > 0 && Len < MAX_PATH)
        {
            if (TryPath(std::filesystem::path{Found}))
                return true;
        }
    }

    std::ostringstream ss;
    ss << "Could not find child executable '" << WideToUtf8(Requested.wstring())
       << "'. Looked in the working directory, the SimulationApp folder, and PATH.";
    OutError = ss.str();
    return false;
}

int SimulationApp::Run(const SimulationOptions& InOpts)
{
    SimulationOptions Opts = InOpts;

    // Install Ctrl+C handler so we can shut down cleanly.
    ::SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    // Resolve log path.
    std::filesystem::path LogPath;
    if (Opts.WriteLogFile)
        LogPath = Opts.LogFile.empty() ? DefaultLogPath() : Opts.LogFile;

    LogSink Sink;
    Sink.Open(LogPath, Opts.EchoToConsole);

    // --- Banner --------------------------------------------------------
    {
        std::ostringstream ss;
        ss << "SimulationApp - Diligent linked multi-GPU simulator\n"
           << "Log file: " << (LogPath.empty() ? "<console only>" : WideToUtf8(LogPath.wstring())) << "\n"
           << "Parent PID: " << ::GetCurrentProcessId() << "\n";
        Sink.WriteBlock(ss.str());
    }

    // --- Build the simulated linked-GPU group --------------------------
    LinkedGpuSimulator Sim;
    {
        std::string Err;
        const unsigned AdapterIdx = (Opts.AdapterIndex == UINT32_MAX) ? UINT32_MAX : Opts.AdapterIndex;
        if (!Sim.Initialize(Opts.SimNodeCount, AdapterIdx, Err))
        {
            std::ostringstream ss;
            ss << "LinkedGpuSimulator initialization failed: " << Err << "\n";
            Sink.WriteBlock(ss.str());
            return -1;
        }
    }
    Sim.Refresh(); // prime PDH + get first VRAM snapshot
    Sink.WriteBlock(GpuInfoConsole::FormatFullReport(Sim));

    // --- Info-only mode: done here -------------------------------------
    if (Opts.InfoOnly || !Opts.RunChild || Opts.ChildExe.empty())
    {
        Sink.WriteBanner("Info-only mode - exiting");
        return 0;
    }

    // --- Resolve child exe --------------------------------------------
    std::filesystem::path ResolvedChild;
    {
        std::string Err;
        if (!ResolveChildExe(Opts.ChildExe, ResolvedChild, Err))
        {
            Sink.WriteBlock(Err + "\n");
            return -1;
        }
    }

    // --- Set up env overrides for the child ---------------------------
    ProcessLauncher::Options POpts;
    POpts.ExePath = ResolvedChild.wstring();
    POpts.Args    = Opts.ChildArgs;
    POpts.WorkingDir = Opts.ChildWorkingDir.empty() ? ResolvedChild.parent_path().wstring() : Opts.ChildWorkingDir.wstring();

    const SimGpuGroup& Group = Sim.GetGroup();

    auto AddEnv = [&](const wchar_t* Name, const std::wstring& Value) {
        EnvOverride E;
        E.Name  = Name;
        E.Value = Value;
        POpts.EnvOverrides.push_back(std::move(E));
    };

    AddEnv(L"DILIGENT_SIM_LINKED_NODE_COUNT", std::to_wstring(Group.NodeCount));

    {
        wchar_t Buf[32];
        std::swprintf(Buf, 32, L"0x%08X", Group.NodeMask);
        AddEnv(L"DILIGENT_SIM_LINKED_NODE_MASK", Buf);
    }
    AddEnv(L"DILIGENT_SIM_HOST_ADAPTER", Utf8ToWide(Group.Host.Name));
    {
        wchar_t Buf[64];
        std::swprintf(Buf, 64, L"0x%08X_0x%08X", Group.Host.LuidHigh, Group.Host.LuidLow);
        AddEnv(L"DILIGENT_SIM_HOST_ADAPTER_LUID", Buf);
    }
    if (!LogPath.empty())
        AddEnv(L"DILIGENT_SIM_LOG_FILE", LogPath.wstring());
    AddEnv(L"DILIGENT_SIM_PARENT_PID", std::to_wstring(::GetCurrentProcessId()));
    AddEnv(L"DILIGENT_SIM_VALIDATION", Opts.Validate ? L"1" : L"0");
    AddEnv(L"DILIGENT_SIM_VIRTUAL_ADAPTERS", Opts.VirtualAdapters ? L"1" : L"0");
    AddEnv(L"DILIGENT_SIM_CROSS_NODE_TIER", std::to_wstring(Opts.CrossNodeTier));

    // --- Resolve driver shim (optional) --------------------------------
    std::filesystem::path ShimDll;
    std::filesystem::path VkLayerDir; // Empty unless the Vulkan layer manifest is found next to the exe.
    if (Opts.UseDriverShim)
    {
        if (!Opts.ShimDllPath.empty())
        {
            ShimDll = Opts.ShimDllPath;
        }
        else
        {
            const std::filesystem::path ExeDir = GetExecutableDir();
            if (!ExeDir.empty())
                ShimDll = ExeDir / L"D3D12Sim.dll";
        }

        std::error_code Ec;
        if (ShimDll.empty() || !std::filesystem::exists(ShimDll, Ec))
        {
            std::ostringstream ss;
            ss << "Driver shim requested but D3D12Sim.dll was not found";
            if (!ShimDll.empty())
                ss << " at " << WideToUtf8(ShimDll.wstring());
            ss << ". Build the DiligentGpuSimulation target or pass --shim-dll PATH.\n";
            Sink.WriteBlock(ss.str());
            return -1;
        }

        // Best-effort: locate the Vulkan layer next to D3D12Sim.dll.  If both
        // the DLL and its JSON manifest are present, we'll additionally set
        // VK_LAYER_PATH + VK_INSTANCE_LAYERS in the child's environment so a
        // Vulkan-mode child auto-enables the layer.
        const std::filesystem::path BinDir     = ShimDll.parent_path();
        const std::filesystem::path VkDll      = BinDir / L"VkLayer_DiligentGpuSim.dll";
        const std::filesystem::path VkManifest = BinDir / L"VkLayer_DiligentGpuSim.json";
        if (std::filesystem::exists(VkDll, Ec) && std::filesystem::exists(VkManifest, Ec))
        {
            VkLayerDir = BinDir;
            // VK_ADD_LAYER_PATH adds to the loader's search; VK_LAYER_PATH would hide
            // the installed layers (the child's validation layer among them)
            auto GetEnv = [](const wchar_t* Name) {
                wchar_t     Buf[2048];
                const DWORD Len = ::GetEnvironmentVariableW(Name, Buf, 2048);
                return Len > 0 && Len < 2048 ? std::wstring{Buf, Len} : std::wstring{};
            };
            const std::wstring AddPath = GetEnv(L"VK_ADD_LAYER_PATH");
            const std::wstring Layers  = GetEnv(L"VK_INSTANCE_LAYERS");
            AddEnv(L"VK_ADD_LAYER_PATH", AddPath.empty() ? VkLayerDir.wstring() : VkLayerDir.wstring() + L";" + AddPath);
            AddEnv(L"VK_INSTANCE_LAYERS", Layers.empty() ? std::wstring{L"VK_LAYER_DiligentGraphics_LinkedGpuSim"} : L"VK_LAYER_DiligentGraphics_LinkedGpuSim;" + Layers);
        }

        POpts.CreateSuspended = true;
    }

    // --- Launch child --------------------------------------------------
    Sink.WriteBanner("Launching child process");
    {
        std::ostringstream ss;
        ss << "Command: " << WideToUtf8(ProcessLauncher::BuildCommandLine(POpts.ExePath, POpts.Args)) << "\n"
           << "Working directory: " << WideToUtf8(POpts.WorkingDir) << "\n";
        if (Opts.UseDriverShim)
        {
            ss << "Driver shim (D3D12): " << WideToUtf8(ShimDll.wstring()) << "\n";
            if (!VkLayerDir.empty())
                ss << "Vulkan layer manifest dir: " << WideToUtf8(VkLayerDir.wstring())
                   << " (VK_INSTANCE_LAYERS=VK_LAYER_DiligentGraphics_LinkedGpuSim)\n";
            else
                ss << "Vulkan layer: not found (child will run without VK layer)\n";
        }
        Sink.WriteBlock(ss.str());
    }

    ProcessLauncher Launcher;
    {
        std::string Err;
        if (!Launcher.Start(POpts, Err))
        {
            Sink.WriteBlock("Failed to launch child: " + Err + "\n");
            return -1;
        }
    }
    const DWORD ChildPid = Launcher.GetPid();
    {
        std::ostringstream ss;
        ss << "Child PID: " << ChildPid << "\n";
        Sink.WriteBlock(ss.str());
    }

    // --- Inject driver shim before the child starts executing ----------
    if (Opts.UseDriverShim)
    {
        std::string InjErr;
        const bool  Ok = InjectDll(Launcher.GetProcessHandle(), ShimDll.wstring(), InjErr);
        if (Ok)
        {
            std::ostringstream ss;
            ss << "Driver shim injected successfully (LoadLibraryW in child returned non-null).\n";
            Sink.WriteBlock(ss.str());
        }
        else
        {
            std::ostringstream ss;
            ss << "WARNING: Driver shim injection failed: " << InjErr
               << "\nThe child will start with the real driver behavior.\n";
            Sink.WriteBlock(ss.str());
        }

        // Resume the main thread; injection is complete either way.
        if (!Launcher.Resume())
            Sink.WriteBlock("WARNING: ResumeThread failed; the child may be stuck.\n");
    }

    // --- Start log capture --------------------------------------------
    PipeReader OutReader;
    PipeReader ErrReader;
    OutReader.Start(Launcher.GetStdoutRead(), "child.out", Sink, /*AsStderr*/ false);
    ErrReader.Start(Launcher.GetStderrRead(), "child.err", Sink, /*AsStderr*/ true);

    DebugOutputReader DbgReader;
    if (Opts.CaptureDebugOutput)
    {
        std::string DbgErr;
        if (!DbgReader.Start(ChildPid, Sink, "child.dbg", DbgErr))
        {
            std::ostringstream ss;
            ss << "OutputDebugString capture disabled: " << DbgErr << "\n";
            Sink.WriteBlock(ss.str());
        }
    }

    // Point the simulator's PDH filter at the child so per-process GPU stats
    // reflect the actual sample, not this launcher.
    Sim.SetFilterPid(ChildPid);

    // --- Monitor loop --------------------------------------------------
    const auto RefreshInterval = std::chrono::duration<double>(Opts.RefreshSeconds > 0.0 ? Opts.RefreshSeconds : 60.0);
    auto NextRefresh = std::chrono::steady_clock::now() + RefreshInterval;

    while (true)
    {
        if (g_ShutdownRequested.load(std::memory_order_acquire))
        {
            Sink.WriteBanner("Shutdown requested - terminating child");
            Launcher.Terminate(1);
            break;
        }

        if (!Launcher.IsRunning())
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (Opts.RefreshSeconds > 0.0 && std::chrono::steady_clock::now() >= NextRefresh)
        {
            Sim.Refresh();
            std::ostringstream ss;
            ss << "\n--- GPU stats snapshot (child PID " << ChildPid << ") ---\n"
               << GpuInfoConsole::FormatGroupPanel(Sim.GetGroup());
            Sink.WriteBlock(ss.str());
            NextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
        }
    }

    // --- Wait for exit + drain log capture ----------------------------
    const int ExitCode = Launcher.Wait();

    // Give the readers a brief moment to observe pipe EOF, then stop them.
    for (int i = 0; i < 20; ++i)
    {
        if (OutReader.IsEof() && ErrReader.IsEof())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    OutReader.Stop();
    ErrReader.Stop();
    DbgReader.Stop();

    {
        std::ostringstream ss;
        ss << "Child process exited with code " << ExitCode << " (0x" << std::hex << ExitCode << ")\n";
        Sink.WriteBanner("Child exited");
        Sink.WriteBlock(ss.str());
    }

    // Final GPU snapshot for the log.
    Sim.SetFilterPid(::GetCurrentProcessId());
    Sim.Refresh();
    Sink.WriteBlock(GpuInfoConsole::FormatFullReport(Sim));

    const unsigned ValidationErrors = Sink.GetValidationErrorCount();
    if (ValidationErrors > 0)
    {
        std::ostringstream ss;
        ss << "The shims reported " << ValidationErrors << " validation error(s): the child used the simulated nodes in a way linked hardware rejects.\n";
        Sink.WriteBlock(ss.str());
    }
    Sink.Close();
    return (ExitCode == 0 && ValidationErrors > 0) ? kExitValidationErrors : ExitCode;
}

} // namespace SimApp
