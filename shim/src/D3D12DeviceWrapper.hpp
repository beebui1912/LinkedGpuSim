/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12DeviceWrapper
//  ------------------
//  Installs a per-instance vtable copy on an ID3D12Device*, replacing a small
//  set of methods with our stubs so:
//    * GetNodeCount() reports the simulated linked-node count, and
//    * every method that takes a NodeMask remaps non-zero masks down to 1
//      (the single real physical node) before delegating to the driver.

#pragma once

struct ID3D12Device;

namespace D3D12Sim
{

// Installs the vtable patch on the given device.  Idempotent: if the device
// is already wrapped, does nothing.  Not thread-safe with respect to other
// concurrent D3D12CreateDevice calls, but that is a driver-serialised path
// anyway.  Returns true on success.
bool WrapDevice(ID3D12Device* pDevice);

// Frees the shadow vtable allocated for WrapDevice's per-instance copy.
// Called from DllMain(DLL_PROCESS_DETACH); safe to skip on abnormal exit.
void ReleaseWrappedState();

} // namespace D3D12Sim
