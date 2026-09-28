/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include "LauncherDialog.hpp"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, LPWSTR /*lpCmdLine*/, int /*nShow*/)
{
    const INT_PTR Result = SimLauncher::RunDialog(hInstance);
    return Result == -1 ? 1 : 0;
}
