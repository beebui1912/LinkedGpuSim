/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  DXGIFactoryWrapper
//  ------------------
//  Patches the vtable of any IDXGIFactory1 returned by dxgi!CreateDXGIFactory1
//  so that EnumAdapters1 injects N virtual "linked node" adapters ahead of the
//  real adapter list, without touching the factory instance's own memory beyond
//  the vtable pointer.  Same per-orig-vtable shadow-copy technique as
//  D3D12DeviceWrapper.

#pragma once

struct IDXGIFactory1;

namespace D3D12Sim
{

// Wraps the returned factory.  Multiple factories (with the same or different
// vtable) can be wrapped over the lifetime of the process.  Idempotent.
bool WrapFactory(IDXGIFactory1* pFactory);

// Restore original vtables (called from DllMain(DETACH)).
void ReleaseFactoryWrappedState();

} // namespace D3D12Sim
