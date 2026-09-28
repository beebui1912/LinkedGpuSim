/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "ShimInjector.hpp"

#include <sstream>

namespace SimApp
{

namespace
{

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

bool InjectDll(HANDLE hProcess, const std::wstring& DllPath, std::string& OutError)
{
    OutError.clear();

    if (hProcess == nullptr || hProcess == INVALID_HANDLE_VALUE)
    {
        OutError = "InjectDll: invalid process handle.";
        return false;
    }
    if (DllPath.empty())
    {
        OutError = "InjectDll: empty DLL path.";
        return false;
    }

    HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (hKernel32 == nullptr)
    {
        OutError = FormatLastError("GetModuleHandleW(kernel32) failed", ::GetLastError());
        return false;
    }

    // kernel32.dll is a Known-DLL: same load address across processes on the
    // same Windows session, so the address we resolve here is valid inside
    // hProcess too.
    const auto pfnLoadLibraryW =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(hKernel32, "LoadLibraryW"));
    if (pfnLoadLibraryW == nullptr)
    {
        OutError = FormatLastError("GetProcAddress(LoadLibraryW) failed", ::GetLastError());
        return false;
    }

    const SIZE_T Bytes = (DllPath.size() + 1) * sizeof(wchar_t);

    LPVOID pRemote = ::VirtualAllocEx(hProcess, nullptr, Bytes,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (pRemote == nullptr)
    {
        OutError = FormatLastError("VirtualAllocEx failed", ::GetLastError());
        return false;
    }

    SIZE_T Written = 0;
    if (!::WriteProcessMemory(hProcess, pRemote, DllPath.c_str(), Bytes, &Written) || Written != Bytes)
    {
        OutError = FormatLastError("WriteProcessMemory failed", ::GetLastError());
        ::VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = ::CreateRemoteThread(hProcess, nullptr, 0,
                                          pfnLoadLibraryW, pRemote, 0, nullptr);
    if (hThread == nullptr)
    {
        OutError = FormatLastError("CreateRemoteThread(LoadLibraryW) failed", ::GetLastError());
        ::VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        return false;
    }

    // Wait for the remote LoadLibrary to complete.  Cap at 30 s so a stuck
    // child cannot hang the parent forever.
    const DWORD WaitRc = ::WaitForSingleObject(hThread, 30000);
    if (WaitRc != WAIT_OBJECT_0)
    {
        OutError = "Remote LoadLibraryW did not complete within 30 seconds.";
        ::CloseHandle(hThread);
        ::VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        return false;
    }

    DWORD ExitCode = 0;
    ::GetExitCodeThread(hThread, &ExitCode);
    ::CloseHandle(hThread);
    ::VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);

    // ExitCode is LoadLibraryW's HMODULE return, truncated to 32 bits on x64.
    // Zero unambiguously means "failed" (modules never load at NULL); a
    // truncation collision to zero is astronomically unlikely.
    if (ExitCode == 0)
    {
        OutError = "Remote LoadLibraryW returned NULL - the child could not load the shim DLL.";
        return false;
    }

    return true;
}

} // namespace SimApp
