/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12Hook
//  ---------
//  Installs / removes the inline hook on d3d12!D3D12CreateDevice via MinHook.
//  The hook, when invoked by the child, delegates to the real driver export
//  and then wraps the returned ID3D12Device with D3D12DeviceWrapper.

#pragma once

namespace D3D12Sim
{

// Installs the D3D12CreateDevice hook.  Must be called after d3d12.dll is
// loaded in the process.  Returns true on success.
bool InstallD3D12Hooks();

// Removes the hook and uninitializes MinHook.
void RemoveD3D12Hooks();

} // namespace D3D12Sim
