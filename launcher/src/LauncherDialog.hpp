/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  LauncherDialog
//  --------------
//  Owns the main dialog window: populates the GPU + node-count combos,
//  handles Browse (IFileOpenDialog), and, on Launch, spawns SimulationApp.exe
//  with the assembled command line in a new console window.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

namespace SimLauncher
{

// Runs the modal dialog.  Returns the last-clicked result (IDOK / IDCANCEL).
INT_PTR RunDialog(HINSTANCE hInstance);

} // namespace SimLauncher
