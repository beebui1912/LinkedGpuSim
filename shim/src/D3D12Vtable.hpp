/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12Vtable
//  -----------
//  Constant slot indices in the ID3D12Device vtable, in declaration order in
//  d3d12.h.  Kept stable across Windows SDK versions per the ABI-stability
//  guarantee (Microsoft only appends to derived interfaces, never reorders).

#pragma once

namespace D3D12Sim
{

// Base IUnknown (3 slots).
constexpr int kSlot_QueryInterface           = 0;
constexpr int kSlot_AddRef                   = 1;
constexpr int kSlot_Release                  = 2;

// ID3D12Object (adds 4).
constexpr int kSlot_GetPrivateData           = 3;
constexpr int kSlot_SetPrivateData           = 4;
constexpr int kSlot_SetPrivateDataInterface  = 5;
constexpr int kSlot_SetName                  = 6;

// ID3D12Device (adds the rest, 44 total for the base interface).
constexpr int kSlot_GetNodeCount                     = 7;   // patched
constexpr int kSlot_CreateCommandQueue               = 8;   // patched
constexpr int kSlot_CreateCommandAllocator           = 9;
constexpr int kSlot_CreateGraphicsPipelineState      = 10;
constexpr int kSlot_CreateComputePipelineState       = 11;
constexpr int kSlot_CreateCommandList                = 12;  // patched
constexpr int kSlot_CheckFeatureSupport              = 13;
constexpr int kSlot_CreateDescriptorHeap             = 14;  // patched
constexpr int kSlot_GetDescriptorHandleIncrementSize = 15;
constexpr int kSlot_CreateRootSignature              = 16;
constexpr int kSlot_CreateConstantBufferView         = 17;
constexpr int kSlot_CreateShaderResourceView         = 18;
constexpr int kSlot_CreateUnorderedAccessView        = 19;
constexpr int kSlot_CreateRenderTargetView           = 20;
constexpr int kSlot_CreateDepthStencilView           = 21;
constexpr int kSlot_CreateSampler                    = 22;
constexpr int kSlot_CopyDescriptors                  = 23;
constexpr int kSlot_CopyDescriptorsSimple            = 24;
constexpr int kSlot_GetResourceAllocationInfo        = 25;  // patched (sret)
constexpr int kSlot_GetCustomHeapProperties          = 26;  // patched (sret)
constexpr int kSlot_CreateCommittedResource          = 27;  // patched
constexpr int kSlot_CreateHeap                       = 28;  // patched
constexpr int kSlot_CreatePlacedResource             = 29;
constexpr int kSlot_CreateReservedResource           = 30;
constexpr int kSlot_CreateSharedHandle               = 31;
constexpr int kSlot_OpenSharedHandle                 = 32;
constexpr int kSlot_OpenSharedHandleByName           = 33;
constexpr int kSlot_MakeResident                     = 34;
constexpr int kSlot_Evict                            = 35;
constexpr int kSlot_CreateFence                      = 36;
constexpr int kSlot_GetDeviceRemovedReason           = 37;
constexpr int kSlot_GetCopyableFootprints            = 38;
constexpr int kSlot_CreateQueryHeap                  = 39;  // patched
constexpr int kSlot_SetStablePowerState              = 40;
constexpr int kSlot_CreateCommandSignature           = 41;  // patched
constexpr int kSlot_GetResourceTiling                = 42;
constexpr int kSlot_GetAdapterLuid                   = 43;

// Extended interfaces (ID3D12Device1..14) append at higher slots.  We copy
// this many entries from the original vtable so QueryInterface'd extended
// interfaces still see the real driver code for methods we do not touch.
constexpr int kVtblCopySlots = 256;

} // namespace D3D12Sim
