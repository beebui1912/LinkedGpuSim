/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12DeviceWrapper
//  ------------------
//  Patches an ID3D12Device so that it reports the simulated node count and
//  accepts node masks for those nodes: every method that takes a node mask
//  checks it against the simulated topology (NodeMasks.hpp) and passes the
//  single physical node to the driver.  Command queues and command lists the
//  device creates are wrapped as well (D3D12CommandWrappers.hpp).

#pragma once

struct ID3D12Device;

namespace D3D12Sim
{

// Idempotent and thread-safe. The patch stays for the lifetime of the device
// (and of the process: the shim pins itself and is never unloaded).
bool WrapDevice(ID3D12Device* pDevice);

} // namespace D3D12Sim
