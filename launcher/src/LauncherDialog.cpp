/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "LauncherDialog.hpp"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <commctrl.h>
#include <shobjidl.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace SimLauncher
{

namespace
{

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::wstring GetExecutableDir()
{
    wchar_t Buf[MAX_PATH]{};
    DWORD   Len = ::GetModuleFileNameW(nullptr, Buf, MAX_PATH);
    if (Len == 0 || Len >= MAX_PATH)
        return {};
    std::filesystem::path P{Buf};
    return P.parent_path().wstring();
}

std::wstring FindSimulationAppExe()
{
    const std::wstring Dir = GetExecutableDir();
    if (Dir.empty()) return {};
    const std::filesystem::path Candidate = std::filesystem::path{Dir} / L"SimulationApp.exe";
    std::error_code Ec;
    if (std::filesystem::exists(Candidate, Ec))
        return Candidate.wstring();
    return {};
}

// Quotes an argument per Windows CRT rules, ready to be appended to a command
// line.  Ported verbatim from ProcessLauncher.cpp so the two agree.
void AppendQuoted(std::wstring& Out, const std::wstring& Arg)
{
    if (!Arg.empty() && Arg.find_first_of(L" \t\n\v\"\\") == std::wstring::npos)
    {
        Out.append(Arg);
        return;
    }
    Out.push_back(L'"');
    for (size_t i = 0; i < Arg.size();)
    {
        size_t Bs = 0;
        while (i < Arg.size() && Arg[i] == L'\\') { ++Bs; ++i; }
        if (i == Arg.size())
        {
            Out.append(Bs * 2, L'\\');
            break;
        }
        else if (Arg[i] == L'"')
        {
            Out.append(Bs * 2 + 1, L'\\');
            Out.push_back(L'"');
            ++i;
        }
        else
        {
            Out.append(Bs, L'\\');
            Out.push_back(Arg[i]);
            ++i;
        }
    }
    Out.push_back(L'"');
}

// Splits an argument string on whitespace, respecting double quotes.
std::vector<std::wstring> SplitArgs(const std::wstring& S)
{
    std::vector<std::wstring> Out;
    std::wstring              Cur;
    bool                      InQuote = false;
    for (wchar_t C : S)
    {
        if (!InQuote && (C == L' ' || C == L'\t'))
        {
            if (!Cur.empty()) { Out.push_back(std::move(Cur)); Cur.clear(); }
        }
        else if (C == L'"')
        {
            InQuote = !InQuote;
        }
        else
        {
            Cur.push_back(C);
        }
    }
    if (!Cur.empty())
        Out.push_back(std::move(Cur));
    return Out;
}

std::wstring FormatMB(UINT64 Bytes)
{
    wchar_t Buf[32];
    std::swprintf(Buf, 32, L"%llu MB", static_cast<unsigned long long>(Bytes / (1024ull * 1024ull)));
    return Buf;
}


// ---------------------------------------------------------------------------
// Populate the GPU combo from DXGI
// ---------------------------------------------------------------------------

struct HostAdapter
{
    std::wstring DisplayName;
    unsigned     Index;
    bool         IsSoftware;
};

std::vector<HostAdapter> EnumerateHostAdapters()
{
    std::vector<HostAdapter> Out;
    Microsoft::WRL::ComPtr<IDXGIFactory1> pFactory;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(pFactory.GetAddressOf()))))
        return Out;
    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> A;
        if (pFactory->EnumAdapters1(i, A.ReleaseAndGetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 Desc{};
        A->GetDesc1(&Desc);
        std::wstring Label = Desc.Description;
        Label += L"   (";
        Label += FormatMB(static_cast<UINT64>(Desc.DedicatedVideoMemory));
        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            Label += L", Software";
        else if (Desc.DedicatedVideoMemory >= static_cast<SIZE_T>(512) * 1024 * 1024)
            Label += L", Discrete";
        else
            Label += L", Integrated";
        Label += L")";
        Out.push_back({std::move(Label), i, (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0});
    }
    return Out;
}


// ---------------------------------------------------------------------------
// Dialog state + procedure
// ---------------------------------------------------------------------------

struct DialogState
{
    std::vector<HostAdapter> Adapters;
};

DialogState* GetState(HWND hDlg)
{
    return reinterpret_cast<DialogState*>(::GetWindowLongPtrW(hDlg, GWLP_USERDATA));
}

void SetStatus(HWND hDlg, const std::wstring& Text, COLORREF /*Color*/ = 0)
{
    ::SetDlgItemTextW(hDlg, IDC_STATIC_STATUS, Text.c_str());
}


// IFileOpenDialog wrapper for a .exe picker.
bool BrowseForExecutable(HWND hOwner, std::wstring& OutPath)
{
    OutPath.clear();
    Microsoft::WRL::ComPtr<IFileOpenDialog> Dlg;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IFileOpenDialog), reinterpret_cast<void**>(Dlg.GetAddressOf()))))
        return false;

    COMDLG_FILTERSPEC Filters[] = {
        {L"Executable files (*.exe)", L"*.exe"},
        {L"All files (*.*)",          L"*.*"},
    };
    Dlg->SetFileTypes(_countof(Filters), Filters);
    Dlg->SetTitle(L"Select a child executable");
    Dlg->SetOkButtonLabel(L"Choose");

    if (FAILED(Dlg->Show(hOwner)))
        return false;

    Microsoft::WRL::ComPtr<IShellItem> Item;
    if (FAILED(Dlg->GetResult(Item.GetAddressOf())))
        return false;

    PWSTR pFileSys = nullptr;
    if (FAILED(Item->GetDisplayName(SIGDN_FILESYSPATH, &pFileSys)))
        return false;
    OutPath = pFileSys;
    ::CoTaskMemFree(pFileSys);
    return true;
}


// Reads the dialog state, builds the command line, and spawns SimulationApp
// with CREATE_NEW_CONSOLE so the user sees the launcher output in its own
// window.  Returns a status message.
bool LaunchSimulation(HWND hDlg, std::wstring& OutStatus)
{
    DialogState* pState = GetState(hDlg);

    // ---- Collect fields --------------------------------------------------
    const int SelGpu   = static_cast<int>(::SendDlgItemMessageW(hDlg, IDC_COMBO_GPU,   CB_GETCURSEL, 0, 0));
    const int SelNodes = static_cast<int>(::SendDlgItemMessageW(hDlg, IDC_COMBO_NODES, CB_GETCURSEL, 0, 0));
    if (SelNodes == CB_ERR)
    {
        OutStatus = L"Please select a node count.";
        return false;
    }
    const unsigned NodeCount = static_cast<unsigned>(SelNodes) + 1u;

    wchar_t ExePath[MAX_PATH * 2]{};
    ::GetDlgItemTextW(hDlg, IDC_EDIT_EXE, ExePath, static_cast<int>(std::size(ExePath)));
    std::wstring Exe{ExePath};
    if (Exe.empty())
    {
        OutStatus = L"Please choose a child executable.";
        return false;
    }
    if (!std::filesystem::exists(std::filesystem::path{Exe}))
    {
        OutStatus = L"Child executable does not exist:\n" + Exe;
        return false;
    }

    wchar_t ArgsBuf[4096]{};
    ::GetDlgItemTextW(hDlg, IDC_EDIT_ARGS, ArgsBuf, static_cast<int>(std::size(ArgsBuf)));
    std::vector<std::wstring> ChildArgs = SplitArgs(std::wstring{ArgsBuf});

    wchar_t LogBuf[MAX_PATH * 2]{};
    ::GetDlgItemTextW(hDlg, IDC_EDIT_LOG, LogBuf, static_cast<int>(std::size(LogBuf)));
    std::wstring LogPath{LogBuf};

    const bool UseShim       = ::IsDlgButtonChecked(hDlg, IDC_CHECK_SHIM)       == BST_CHECKED;
    const bool UseDbgCapture = ::IsDlgButtonChecked(hDlg, IDC_CHECK_DBGCAPTURE) == BST_CHECKED;
    const bool Verbose       = ::IsDlgButtonChecked(hDlg, IDC_CHECK_VERBOSE)    == BST_CHECKED;

    // ---- Resolve SimulationApp.exe --------------------------------------
    const std::wstring SimAppExe = FindSimulationAppExe();
    if (SimAppExe.empty())
    {
        OutStatus = L"SimulationApp.exe not found next to SimulationLauncher.exe.\n"
                    L"Build the DiligentGpuSimulation target first.";
        return false;
    }

    // ---- Build the argument list for SimulationApp ----------------------
    std::vector<std::wstring> Argv;
    Argv.push_back(SimAppExe);

    Argv.push_back(L"-n");
    Argv.push_back(std::to_wstring(NodeCount));

    if (SelGpu != CB_ERR && pState != nullptr &&
        static_cast<size_t>(SelGpu) < pState->Adapters.size())
    {
        Argv.push_back(L"-a");
        Argv.push_back(std::to_wstring(pState->Adapters[SelGpu].Index));
    }

    if (!LogPath.empty())
    {
        Argv.push_back(L"-l");
        Argv.push_back(LogPath);
    }
    if (!UseDbgCapture)
        Argv.push_back(L"--no-debug-capture");
    if (UseShim)
        Argv.push_back(L"--driver-shim");
    Argv.push_back(L"--refresh");
    Argv.push_back(L"2");
    Argv.push_back(L"--");
    Argv.push_back(Exe);
    for (const std::wstring& A : ChildArgs)
        Argv.push_back(A);

    std::wstring CmdLine;
    for (size_t i = 0; i < Argv.size(); ++i)
    {
        if (i != 0) CmdLine.push_back(L' ');
        AppendQuoted(CmdLine, Argv[i]);
    }

    // Environment override for verbose tracing.  We prepare a small block
    // that adds DILIGENT_SIM_VERBOSE=1 to the child's environment.
    std::wstring EnvBlock;
    if (Verbose)
    {
        // Merge parent env + our override.
        LPWCH pParent = ::GetEnvironmentStringsW();
        for (LPWCH p = pParent; *p != L'\0';)
        {
            const size_t Len = std::wcslen(p);
            EnvBlock.append(p, Len);
            EnvBlock.push_back(L'\0');
            p += Len + 1;
        }
        ::FreeEnvironmentStringsW(pParent);
        EnvBlock.append(L"DILIGENT_SIM_VERBOSE=1");
        EnvBlock.push_back(L'\0');
        EnvBlock.push_back(L'\0');
    }

    // ---- CreateProcess with CREATE_NEW_CONSOLE ---------------------------
    STARTUPINFOW SI{};
    SI.cb = sizeof(SI);
    PROCESS_INFORMATION PI{};

    std::vector<wchar_t> CmdBuf(CmdLine.begin(), CmdLine.end());
    CmdBuf.push_back(L'\0');

    DWORD Flags = CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT;

    BOOL Ok = ::CreateProcessW(
        SimAppExe.c_str(),
        CmdBuf.data(),
        nullptr, nullptr,
        FALSE,
        Flags,
        Verbose ? EnvBlock.data() : nullptr,
        nullptr,
        &SI,
        &PI);
    if (!Ok)
    {
        wchar_t Msg[128];
        std::swprintf(Msg, 128, L"CreateProcess failed (error %lu).", ::GetLastError());
        OutStatus = Msg;
        return false;
    }

    ::CloseHandle(PI.hProcess);
    ::CloseHandle(PI.hThread);

    OutStatus = L"Launched (PID " + std::to_wstring(PI.dwProcessId) + L"). Log: " +
                (LogPath.empty() ? std::wstring{L"<default>"} : LogPath);
    return true;
}


INT_PTR CALLBACK DlgProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (Msg)
    {
        case WM_INITDIALOG:
        {
            auto* pState = new DialogState;
            ::SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pState));

            // Populate GPU combo
            pState->Adapters = EnumerateHostAdapters();
            for (const HostAdapter& A : pState->Adapters)
                ::SendDlgItemMessageW(hDlg, IDC_COMBO_GPU, CB_ADDSTRING, 0,
                                      reinterpret_cast<LPARAM>(A.DisplayName.c_str()));
            // Pre-select first non-software
            int PrimarySel = 0;
            for (size_t i = 0; i < pState->Adapters.size(); ++i)
            {
                if (!pState->Adapters[i].IsSoftware) { PrimarySel = static_cast<int>(i); break; }
            }
            ::SendDlgItemMessageW(hDlg, IDC_COMBO_GPU, CB_SETCURSEL, PrimarySel, 0);

            // Populate node count combo (1..8), preselect 2
            for (int n = 1; n <= 8; ++n)
            {
                wchar_t Buf[8];
                std::swprintf(Buf, 8, L"%d", n);
                ::SendDlgItemMessageW(hDlg, IDC_COMBO_NODES, CB_ADDSTRING, 0,
                                      reinterpret_cast<LPARAM>(Buf));
            }
            ::SendDlgItemMessageW(hDlg, IDC_COMBO_NODES, CB_SETCURSEL, 1, 0); // "2"

            // Default checkboxes
            ::CheckDlgButton(hDlg, IDC_CHECK_SHIM,       BST_CHECKED);
            ::CheckDlgButton(hDlg, IDC_CHECK_DBGCAPTURE, BST_CHECKED);
            ::CheckDlgButton(hDlg, IDC_CHECK_VERBOSE,   BST_UNCHECKED);

            // Default args suggestion for Diligent samples.
            ::SetDlgItemTextW(hDlg, IDC_EDIT_ARGS, L"--mode D3D12");

            SetStatus(hDlg, L"Ready.");
            return TRUE;
        }

        case WM_COMMAND:
        {
            const WORD Id   = LOWORD(wParam);
            const WORD Code = HIWORD(wParam);

            if (Id == IDC_BUTTON_BROWSE && Code == BN_CLICKED)
            {
                std::wstring Path;
                if (BrowseForExecutable(hDlg, Path))
                    ::SetDlgItemTextW(hDlg, IDC_EDIT_EXE, Path.c_str());
                return TRUE;
            }
            if (Id == IDOK && Code == BN_CLICKED)
            {
                std::wstring Status;
                const bool   Ok = LaunchSimulation(hDlg, Status);
                SetStatus(hDlg, Status);
                if (!Ok)
                    ::MessageBoxW(hDlg, Status.c_str(), L"SimulationLauncher", MB_ICONWARNING);
                return TRUE;
            }
            if (Id == IDCANCEL && Code == BN_CLICKED)
            {
                ::EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            ::EndDialog(hDlg, IDCANCEL);
            return TRUE;

        case WM_DESTROY:
        {
            DialogState* pState = GetState(hDlg);
            if (pState != nullptr)
            {
                ::SetWindowLongPtrW(hDlg, GWLP_USERDATA, 0);
                delete pState;
            }
            return TRUE;
        }
    }
    return FALSE;
}

} // namespace


INT_PTR RunDialog(HINSTANCE hInstance)
{
    INITCOMMONCONTROLSEX Icc{sizeof(Icc), ICC_STANDARD_CLASSES};
    ::InitCommonControlsEx(&Icc);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INT_PTR Result = ::DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_LAUNCHER),
                                       nullptr, &DlgProc, 0);

    ::CoUninitialize();
    return Result;
}

} // namespace SimLauncher
