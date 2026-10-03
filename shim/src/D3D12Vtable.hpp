/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12Vtable
//  -----------
//  Slot indices in the vtables the shim patches, in declaration order in
//  d3d12.h.  Microsoft only appends to derived interfaces, so the indices are
//  stable; D3D12VtableCheck.cpp verifies every index used here against the
//  SDK headers at compile time.

#pragma once

namespace D3D12Sim
{

// ---- ID3D12Device (IUnknown 0-2, ID3D12Object 3-6) -------------------------
constexpr int kSlot_GetNodeCount                = 7;
constexpr int kSlot_CreateCommandQueue          = 8;
constexpr int kSlot_CreateGraphicsPipelineState = 10;
constexpr int kSlot_CreateComputePipelineState  = 11;
constexpr int kSlot_CreateCommandList           = 12;
constexpr int kSlot_CheckFeatureSupport         = 13;
constexpr int kSlot_CreateDescriptorHeap        = 14;
constexpr int kSlot_CreateRootSignature         = 16;
constexpr int kSlot_GetResourceAllocationInfo   = 25; // returns a struct (hidden pointer)
constexpr int kSlot_GetCustomHeapProperties     = 26; // returns a struct (hidden pointer)
constexpr int kSlot_CreateCommittedResource     = 27;
constexpr int kSlot_CreateHeap                  = 28;
constexpr int kSlot_CreatePlacedResource        = 29;
constexpr int kSlot_CreateQueryHeap             = 39;
constexpr int kSlot_CreateCommandSignature      = 41;
constexpr int kSlot_GetAdapterLuid              = 43; // returns a struct (hidden pointer)

// ---- ID3D12Device1 .. ID3D12Device10 ---------------------------------------
constexpr int kSlot_CreatePipelineState         = 47; // ID3D12Device2
constexpr int kSlot_CreateCommandList1          = 51; // ID3D12Device4
constexpr int kSlot_CreateCommittedResource1    = 53; // ID3D12Device4
constexpr int kSlot_CreateHeap1                 = 54; // ID3D12Device4
constexpr int kSlot_GetResourceAllocationInfo1  = 56; // ID3D12Device4, returns a struct
constexpr int kSlot_CreateStateObject           = 62; // ID3D12Device5
constexpr int kSlot_GetResourceAllocationInfo2  = 68; // ID3D12Device8, returns a struct
constexpr int kSlot_CreateCommittedResource2    = 69; // ID3D12Device8
constexpr int kSlot_CreatePlacedResource1       = 70; // ID3D12Device8
constexpr int kSlot_CreateCommandQueue1         = 75; // ID3D12Device9
constexpr int kSlot_CreateCommittedResource3    = 76; // ID3D12Device10
constexpr int kSlot_CreatePlacedResource2       = 77; // ID3D12Device10

// ---- ID3D12CommandQueue (ID3D12DeviceChild::GetDevice is 7) ---------------
constexpr int kQueueSlot_ExecuteCommandLists = 10;
constexpr int kQueueSlot_GetDesc             = 18; // returns a struct (hidden pointer)

// ---- ID3D12GraphicsCommandList (ID3D12CommandList::GetType is 8) ----------
constexpr int kListSlot_CopyBufferRegion  = 15;
constexpr int kListSlot_CopyTextureRegion = 16;
constexpr int kListSlot_CopyResource      = 17;

// Entries copied from an original vtable into its shadow, so that methods of
// derived interfaces (ID3D12Device14, ID3D12GraphicsCommandList10, ...) that
// the shim does not patch still reach the real implementation.
constexpr int kVtblCopySlots = 256;

} // namespace D3D12Sim
