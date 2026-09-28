/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "ProcessLauncher.hpp"

#include <cwchar>
#include <sstream>
#include <string>

namespace SimApp
{

namespace
{

// Quotes/escapes a single argv element per the Windows CRT parsing rules
// (see docs on CommandLineToArgvW).  Appends the result to OutBuf.
void AppendQuotedArg(std::wstring& OutBuf, const std::wstring& Arg)
{
    // Simple case: no whitespace, no quotes, no backslashes => append verbatim.
    if (!Arg.empty() &&
        Arg.find_first_of(L" \t\n\v\"\\") == std::wstring::npos)
    {
        OutBuf.append(Arg);
        return;
    }

    OutBuf.push_back(L'"');
    for (size_t i = 0; i < Arg.size();)
    {
        // Count consecutive backslashes.
        size_t Backslashes = 0;
        while (i < Arg.size() && Arg[i] == L'\\')
        {
            ++Backslashes;
            ++i;
        }
        if (i == Arg.size())
        {
            // Backslashes at end: double them (they precede the closing quote).
            OutBuf.append(Backslashes * 2, L'\\');
            break;
        }
        else if (Arg[i] == L'"')
        {
            // Escape all preceding backslashes and the quote itself.
            OutBuf.append(Backslashes * 2 + 1, L'\\');
            OutBuf.push_back(L'"');
            ++i;
        }
        else
        {
            OutBuf.append(Backslashes, L'\\');
            OutBuf.push_back(Arg[i]);
            ++i;
        }
    }
    OutBuf.push_back(L'"');
}

// Builds a null-separated, double-null-terminated CREATE_UNICODE_ENVIRONMENT
// block from the parent's environment merged with the caller's overrides.
std::vector<wchar_t> BuildEnvironmentBlock(const std::vector<EnvOverride>& Overrides)
{
    // Grab parent's environment.
    LPWCH pParent = GetEnvironmentStringsW();
    std::vector<std::pair<std::wstring, std::wstring>> Env;

    if (pParent != nullptr)
    {
        for (LPWCH p = pParent; *p != L'\0';)
        {
            std::wstring Entry(p);
            p += Entry.size() + 1;

            // Skip drive-specific entries like "=C:=C:\Foo" only if they're
            // malformed; otherwise preserve them (the CRT sometimes needs them).
            const size_t Eq = Entry.find(L'=');
            if (Eq == std::wstring::npos || Eq == 0)
            {
                // Preserve the raw string with an empty key so it round-trips.
                Env.emplace_back(std::wstring{}, std::move(Entry));
            }
            else
            {
                std::wstring Key   = Entry.substr(0, Eq);
                std::wstring Value = Entry.substr(Eq + 1);
                Env.emplace_back(std::move(Key), std::move(Value));
            }
        }
        FreeEnvironmentStringsW(pParent);
    }

    auto EqualsIgnoreCaseW = [](const std::wstring& A, const std::wstring& B) {
        return A.size() == B.size() &&
               _wcsicmp(A.c_str(), B.c_str()) == 0;
    };

    // Apply overrides: replace existing keys, append new ones.
    for (const EnvOverride& Ov : Overrides)
    {
        bool Replaced = false;
        for (auto& Kv : Env)
        {
            if (!Kv.first.empty() && EqualsIgnoreCaseW(Kv.first, Ov.Name))
            {
                Kv.second = Ov.Value;
                Replaced  = true;
                break;
            }
        }
        if (!Replaced)
            Env.emplace_back(Ov.Name, Ov.Value);
    }

    std::vector<wchar_t> Block;
    Block.reserve(1024);
    for (const auto& Kv : Env)
    {
        std::wstring Entry;
        if (Kv.first.empty())
        {
            // Preserved verbatim (already contains '=' or is a special entry).
            Entry = Kv.second;
        }
        else
        {
            Entry = Kv.first;
            Entry.push_back(L'=');
            Entry.append(Kv.second);
        }
        Block.insert(Block.end(), Entry.begin(), Entry.end());
        Block.push_back(L'\0');
    }
    // Terminating extra null (block terminator).
    Block.push_back(L'\0');
    return Block;
}

std::string FormatLastError(const char* Prefix, DWORD Err)
{
    LPVOID pMsg = nullptr;
    ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, Err, 0, reinterpret_cast<LPSTR>(&pMsg), 0, nullptr);
    std::ostringstream ss;
    ss << Prefix << " (" << Err << ")";
    if (pMsg != nullptr)
    {
        std::string Msg(static_cast<const char*>(pMsg));
        while (!Msg.empty() && (Msg.back() == '\r' || Msg.back() == '\n' || Msg.back() == '.'))
            Msg.pop_back();
        if (!Msg.empty())
            ss << ": " << Msg;
        ::LocalFree(pMsg);
    }
    return ss.str();
}

} // namespace

std::wstring ProcessLauncher::BuildCommandLine(const std::wstring& ExePath, const std::vector<std::wstring>& Args)
{
    std::wstring Cmd;
    Cmd.reserve(ExePath.size() + 2);
    AppendQuotedArg(Cmd, ExePath);
    for (const std::wstring& A : Args)
    {
        Cmd.push_back(L' ');
        AppendQuotedArg(Cmd, A);
    }
    return Cmd;
}

ProcessLauncher::ProcessLauncher()
{
    m_ProcInfo.hProcess = nullptr;
    m_ProcInfo.hThread  = nullptr;
}

ProcessLauncher::~ProcessLauncher()
{
    Terminate();
    CloseAllHandles();
}

bool ProcessLauncher::Start(const Options& Opts, std::string& OutError)
{
    OutError.clear();
    if (m_Started)
    {
        OutError = "ProcessLauncher already started.";
        return false;
    }
    if (Opts.ExePath.empty())
    {
        OutError = "No child executable specified.";
        return false;
    }

    SECURITY_ATTRIBUTES SA{};
    SA.nLength              = sizeof(SA);
    SA.bInheritHandle       = TRUE;
    SA.lpSecurityDescriptor = nullptr;

    if (Opts.CapturePipes)
    {
        if (!::CreatePipe(&m_hStdoutRead, &m_hStdoutWrite, &SA, 0))
        {
            OutError = FormatLastError("CreatePipe(stdout) failed", ::GetLastError());
            CloseAllHandles();
            return false;
        }
        if (!::SetHandleInformation(m_hStdoutRead, HANDLE_FLAG_INHERIT, 0))
        {
            OutError = FormatLastError("SetHandleInformation(stdout) failed", ::GetLastError());
            CloseAllHandles();
            return false;
        }
        if (!::CreatePipe(&m_hStderrRead, &m_hStderrWrite, &SA, 0))
        {
            OutError = FormatLastError("CreatePipe(stderr) failed", ::GetLastError());
            CloseAllHandles();
            return false;
        }
        if (!::SetHandleInformation(m_hStderrRead, HANDLE_FLAG_INHERIT, 0))
        {
            OutError = FormatLastError("SetHandleInformation(stderr) failed", ::GetLastError());
            CloseAllHandles();
            return false;
        }
    }

    STARTUPINFOW SI{};
    SI.cb          = sizeof(SI);
    SI.dwFlags     = STARTF_USESTDHANDLES;
    SI.hStdInput   = ::GetStdHandle(STD_INPUT_HANDLE);
    SI.hStdOutput  = Opts.CapturePipes ? m_hStdoutWrite : ::GetStdHandle(STD_OUTPUT_HANDLE);
    SI.hStdError   = Opts.CapturePipes ? m_hStderrWrite : ::GetStdHandle(STD_ERROR_HANDLE);

    std::wstring         CommandLine = BuildCommandLine(Opts.ExePath, Opts.Args);
    std::vector<wchar_t> CmdBuf(CommandLine.begin(), CommandLine.end());
    CmdBuf.push_back(L'\0');

    std::vector<wchar_t> EnvBlock = BuildEnvironmentBlock(Opts.EnvOverrides);

    DWORD Flags = CREATE_UNICODE_ENVIRONMENT;
    if (Opts.NewProcessGroup)
        Flags |= CREATE_NEW_PROCESS_GROUP;
    if (Opts.CreateSuspended)
        Flags |= CREATE_SUSPENDED;

    const wchar_t* WorkDir = Opts.WorkingDir.empty() ? nullptr : Opts.WorkingDir.c_str();

    BOOL Ok = ::CreateProcessW(
        Opts.ExePath.c_str(),
        CmdBuf.data(),
        nullptr, nullptr,
        /*bInheritHandles*/ TRUE,
        Flags,
        EnvBlock.data(),
        WorkDir,
        &SI,
        &m_ProcInfo);

    if (!Ok)
    {
        OutError = FormatLastError("CreateProcessW failed", ::GetLastError());
        CloseAllHandles();
        return false;
    }

    // Parent no longer needs the write ends: closing them lets the read ends
    // observe EOF once the child releases its side.
    CloseWriteEnds();

    m_Started = true;
    return true;
}

bool ProcessLauncher::IsRunning() const
{
    if (!m_Started || m_ProcInfo.hProcess == nullptr)
        return false;
    DWORD Code = 0;
    if (::GetExitCodeProcess(m_ProcInfo.hProcess, &Code) == FALSE)
        return false;
    return Code == STILL_ACTIVE;
}

bool ProcessLauncher::Resume()
{
    if (!m_Started || m_ProcInfo.hThread == nullptr)
        return false;
    // ResumeThread returns the previous suspend count.  0xFFFFFFFF means error.
    const DWORD Prev = ::ResumeThread(m_ProcInfo.hThread);
    return Prev != static_cast<DWORD>(-1);
}

int ProcessLauncher::Wait()
{
    if (!m_Started || m_ProcInfo.hProcess == nullptr)
        return -1;

    ::WaitForSingleObject(m_ProcInfo.hProcess, INFINITE);
    DWORD Code = 0;
    if (::GetExitCodeProcess(m_ProcInfo.hProcess, &Code) == FALSE)
        return -1;
    return static_cast<int>(Code);
}

void ProcessLauncher::Terminate(unsigned ExitCode)
{
    if (m_ProcInfo.hProcess != nullptr && IsRunning())
    {
        ::TerminateProcess(m_ProcInfo.hProcess, ExitCode);
        ::WaitForSingleObject(m_ProcInfo.hProcess, 5000);
    }
}

void ProcessLauncher::CloseWriteEnds()
{
    if (m_hStdoutWrite != nullptr)
    {
        ::CloseHandle(m_hStdoutWrite);
        m_hStdoutWrite = nullptr;
    }
    if (m_hStderrWrite != nullptr)
    {
        ::CloseHandle(m_hStderrWrite);
        m_hStderrWrite = nullptr;
    }
}

void ProcessLauncher::CloseAllHandles()
{
    CloseWriteEnds();

    if (m_hStdoutRead != nullptr)
    {
        ::CloseHandle(m_hStdoutRead);
        m_hStdoutRead = nullptr;
    }
    if (m_hStderrRead != nullptr)
    {
        ::CloseHandle(m_hStderrRead);
        m_hStderrRead = nullptr;
    }
    if (m_ProcInfo.hProcess != nullptr)
    {
        ::CloseHandle(m_ProcInfo.hProcess);
        m_ProcInfo.hProcess = nullptr;
    }
    if (m_ProcInfo.hThread != nullptr)
    {
        ::CloseHandle(m_ProcInfo.hThread);
        m_ProcInfo.hThread = nullptr;
    }
}

} // namespace SimApp
