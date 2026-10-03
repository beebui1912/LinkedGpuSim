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
constexpr int kSlot_CreateConstantBufferView    = 17;
constexpr int kSlot_CreateShaderResourceView    = 18;
constexpr int kSlot_CreateUnorderedAccessView   = 19;
constexpr int kSlot_CreateRenderTargetView      = 20;
constexpr int kSlot_CreateDepthStencilView      = 21;
constexpr int kSlot_CopyDescriptors             = 23;
constexpr int kSlot_CopyDescriptorsSimple       = 24;
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
constexpr int kListSlot_Reset                              = 10;
constexpr int kListSlot_DrawInstanced                      = 12;
constexpr int kListSlot_DrawIndexedInstanced               = 13;
constexpr int kListSlot_Dispatch                           = 14;
constexpr int kListSlot_CopyBufferRegion                   = 15;
constexpr int kListSlot_CopyTextureRegion                  = 16;
constexpr int kListSlot_CopyResource                       = 17;
constexpr int kListSlot_CopyTiles                          = 18;
constexpr int kListSlot_ResolveSubresource                 = 19;
constexpr int kListSlot_SetPipelineState                   = 25;
constexpr int kListSlot_ExecuteBundle                      = 27;
constexpr int kListSlot_SetDescriptorHeaps                 = 28;
constexpr int kListSlot_SetComputeRootSignature            = 29;
constexpr int kListSlot_SetGraphicsRootSignature           = 30;
constexpr int kListSlot_SetComputeRootDescriptorTable      = 31;
constexpr int kListSlot_SetGraphicsRootDescriptorTable     = 32;
constexpr int kListSlot_SetComputeRootConstantBufferView   = 37;
constexpr int kListSlot_SetGraphicsRootConstantBufferView  = 38;
constexpr int kListSlot_SetComputeRootShaderResourceView   = 39;
constexpr int kListSlot_SetGraphicsRootShaderResourceView  = 40;
constexpr int kListSlot_SetComputeRootUnorderedAccessView  = 41;
constexpr int kListSlot_SetGraphicsRootUnorderedAccessView = 42;
constexpr int kListSlot_IASetIndexBuffer                   = 43;
constexpr int kListSlot_IASetVertexBuffers                 = 44;
constexpr int kListSlot_SOSetTargets                       = 45;
constexpr int kListSlot_OMSetRenderTargets                 = 46;
constexpr int kListSlot_ClearDepthStencilView              = 47;
constexpr int kListSlot_ClearRenderTargetView              = 48;
constexpr int kListSlot_ClearUnorderedAccessViewUint       = 49;
constexpr int kListSlot_ClearUnorderedAccessViewFloat      = 50;
constexpr int kListSlot_BeginQuery                         = 52;
constexpr int kListSlot_EndQuery                           = 53;
constexpr int kListSlot_ResolveQueryData                   = 54;
constexpr int kListSlot_ExecuteIndirect                    = 59;
constexpr int kListSlot_BeginRenderPass                    = 68; // ID3D12GraphicsCommandList4
constexpr int kListSlot_DispatchMesh                       = 79; // ID3D12GraphicsCommandList6

// ---- DXGI -----------------------------------------------------------------
constexpr int kFactorySlot_EnumAdapters                  = 7;
constexpr int kFactorySlot_CreateSwapChain               = 10;
constexpr int kFactorySlot_EnumAdapters1                 = 12; // IDXGIFactory1
constexpr int kFactorySlot_CreateSwapChainForHwnd        = 15; // IDXGIFactory2
constexpr int kFactorySlot_CreateSwapChainForCoreWindow  = 16; // IDXGIFactory2
constexpr int kFactorySlot_CreateSwapChainForComposition = 24; // IDXGIFactory2
constexpr int kFactorySlot_EnumAdapterByLuid             = 26; // IDXGIFactory4
constexpr int kFactorySlot_EnumAdapterByGpuPreference    = 29; // IDXGIFactory6
constexpr int kAdapterSlot_QueryVideoMemoryInfo          = 14; // IDXGIAdapter3
constexpr int kAdapterSlot_SetVideoMemoryReservation     = 15; // IDXGIAdapter3
constexpr int kSwapChainSlot_GetBuffer                   = 9;
constexpr int kSwapChainSlot_ResizeBuffers               = 13;
constexpr int kSwapChainSlot_ResizeBuffers1              = 39; // IDXGISwapChain3

// Entries copied from an original vtable into its shadow, so that methods of
// derived interfaces (ID3D12Device14, ID3D12GraphicsCommandList10, ...) that
// the shim does not patch still reach the real implementation.
constexpr int kVtblCopySlots = 256;

} // namespace D3D12Sim
