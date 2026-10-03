/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  DXGIFactoryWrapper
//  ------------------
//  Patches every DXGI factory the process creates:
//
//    * adapters it returns (EnumAdapters[1], EnumAdapterByLuid,
//      EnumAdapterByGpuPreference) on the host adapter answer per node, as a
//      linked adapter does: QueryVideoMemoryInfo / SetVideoMemoryReservation
//      accept node indices below N; each node's local budget is the adapter's
//      divided by N (the nodes share one GPU's memory), and its usage is the
//      memory of the resources and heaps created on that node plus a share of
//      what the shim cannot attribute;
//    * swap chains it creates on a D3D12 command queue: their buffers live on
//      the queue's node (ResizeBuffers1: on the nodes given per buffer) and are
//      visible to that node only;
//    * with DILIGENT_SIM_VIRTUAL_ADAPTERS=1, EnumAdapters[1] also lists one
//      virtual adapter per node before the real ones (VirtualDXGIAdapter).

#pragma once

struct IUnknown;

namespace D3D12Sim
{

// Wraps a factory returned by CreateDXGIFactory*. Idempotent.
bool WrapFactory(IUnknown* pFactory);

} // namespace D3D12Sim
