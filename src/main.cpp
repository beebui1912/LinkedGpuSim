/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include <cstdio>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "SimulationApp.hpp"

#pragma comment(lib, "shell32.lib")

namespace
{

// Simple wide-string helper so we don't drag Utf8ToWide from SimulationApp.cpp.
std::string WideToUtf8Local(const std::wstring& W)
{
    if (W.empty())
        return {};
    const int Len = ::WideCharToMultiByte(CP_UTF8, 0, W.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
    if (Len <= 1)
        return {};
    std::string Out(static_cast<size_t>(Len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, W.c_str(), -1,
                          Out.data(), Len, nullptr, nullptr);
    return Out;
}

} // namespace

int main()
{
    // Grab the "true" wide argv so paths and args with unicode / spaces work
    // regardless of the CRT entry point (wmain vs main).
    int      wargc = 0;
    LPWSTR*  wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    if (wargv == nullptr)
    {
        std::fputs("Failed to parse command line.\n", stderr);
        return 2;
    }

    SimApp::SimulationOptions Opts;
    std::string               Err;
    bool                      ShowHelp = false;
    const bool                Ok       = SimApp::ParseCommandLine(wargc, wargv, Opts, Err, ShowHelp);
    ::LocalFree(wargv);

    if (!Ok)
    {
        std::fprintf(stderr, "SimulationApp: %s\n\n", Err.c_str());
        SimApp::PrintHelp();
        return 2;
    }
    if (ShowHelp)
    {
        SimApp::PrintHelp();
        return 0;
    }

    // Best-effort: enable UTF-8 output so unicode adapter names render properly.
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    SimApp::SimulationApp App;
    const int             Rc = App.Run(Opts);
    return Rc;
}
