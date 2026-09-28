/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  DXGIHook
//  --------
//  MinHook installers for dxgi!CreateDXGIFactory / CreateDXGIFactory1 /
//  CreateDXGIFactory2.  Every returned factory has its vtable patched so
//  EnumAdapters1 injects the simulated linked-node adapters.

#pragma once

namespace D3D12Sim
{

bool InstallDXGIHooks();
void RemoveDXGIHooks();

} // namespace D3D12Sim
