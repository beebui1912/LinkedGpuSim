/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  Compile-time check of the slot indices in D3D12Vtable.hpp against the C
//  declarations of the Windows SDK (CINTERFACE exposes the vtables as structs).
//  No code is generated; a wrong index fails the build.

#define CINTERFACE
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <cstddef>

#include "D3D12Vtable.hpp"

#define D3D12SIM_CHECK_SLOT(Vtbl, Method, Slot) \
    static_assert(offsetof(Vtbl, Method) / sizeof(void*) == D3D12Sim::Slot, #Vtbl "::" #Method " is not at " #Slot)

// ID3D12Device .. ID3D12Device10
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, GetNodeCount, kSlot_GetNodeCount);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateCommandQueue, kSlot_CreateCommandQueue);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateGraphicsPipelineState, kSlot_CreateGraphicsPipelineState);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateComputePipelineState, kSlot_CreateComputePipelineState);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateCommandList, kSlot_CreateCommandList);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CheckFeatureSupport, kSlot_CheckFeatureSupport);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap, kSlot_CreateDescriptorHeap);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateRootSignature, kSlot_CreateRootSignature);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, GetResourceAllocationInfo, kSlot_GetResourceAllocationInfo);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, GetCustomHeapProperties, kSlot_GetCustomHeapProperties);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateCommittedResource, kSlot_CreateCommittedResource);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateHeap, kSlot_CreateHeap);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreatePlacedResource, kSlot_CreatePlacedResource);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateQueryHeap, kSlot_CreateQueryHeap);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateCommandSignature, kSlot_CreateCommandSignature);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, GetAdapterLuid, kSlot_GetAdapterLuid);
D3D12SIM_CHECK_SLOT(ID3D12Device2Vtbl, CreatePipelineState, kSlot_CreatePipelineState);
D3D12SIM_CHECK_SLOT(ID3D12Device4Vtbl, CreateCommandList1, kSlot_CreateCommandList1);
D3D12SIM_CHECK_SLOT(ID3D12Device4Vtbl, CreateCommittedResource1, kSlot_CreateCommittedResource1);
D3D12SIM_CHECK_SLOT(ID3D12Device4Vtbl, CreateHeap1, kSlot_CreateHeap1);
D3D12SIM_CHECK_SLOT(ID3D12Device4Vtbl, GetResourceAllocationInfo1, kSlot_GetResourceAllocationInfo1);
D3D12SIM_CHECK_SLOT(ID3D12Device5Vtbl, CreateStateObject, kSlot_CreateStateObject);
D3D12SIM_CHECK_SLOT(ID3D12Device8Vtbl, GetResourceAllocationInfo2, kSlot_GetResourceAllocationInfo2);
D3D12SIM_CHECK_SLOT(ID3D12Device8Vtbl, CreateCommittedResource2, kSlot_CreateCommittedResource2);
D3D12SIM_CHECK_SLOT(ID3D12Device8Vtbl, CreatePlacedResource1, kSlot_CreatePlacedResource1);
D3D12SIM_CHECK_SLOT(ID3D12Device9Vtbl, CreateCommandQueue1, kSlot_CreateCommandQueue1);
D3D12SIM_CHECK_SLOT(ID3D12Device10Vtbl, CreateCommittedResource3, kSlot_CreateCommittedResource3);
D3D12SIM_CHECK_SLOT(ID3D12Device10Vtbl, CreatePlacedResource2, kSlot_CreatePlacedResource2);

// ID3D12CommandQueue
D3D12SIM_CHECK_SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists, kQueueSlot_ExecuteCommandLists);
D3D12SIM_CHECK_SLOT(ID3D12CommandQueueVtbl, GetDesc, kQueueSlot_GetDesc);

// ID3D12GraphicsCommandList
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, CopyBufferRegion, kListSlot_CopyBufferRegion);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, CopyTextureRegion, kListSlot_CopyTextureRegion);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, CopyResource, kListSlot_CopyResource);
