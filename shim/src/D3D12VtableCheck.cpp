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
#include <dxgi1_6.h>
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

D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateConstantBufferView, kSlot_CreateConstantBufferView);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateShaderResourceView, kSlot_CreateShaderResourceView);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView, kSlot_CreateUnorderedAccessView);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateRenderTargetView, kSlot_CreateRenderTargetView);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CreateDepthStencilView, kSlot_CreateDepthStencilView);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CopyDescriptors, kSlot_CopyDescriptors);
D3D12SIM_CHECK_SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple, kSlot_CopyDescriptorsSimple);

D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, Reset, kListSlot_Reset);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced, kListSlot_DrawInstanced);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced, kListSlot_DrawIndexedInstanced);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, Dispatch, kListSlot_Dispatch);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, CopyTiles, kListSlot_CopyTiles);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ResolveSubresource, kListSlot_ResolveSubresource);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetPipelineState, kListSlot_SetPipelineState);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ExecuteBundle, kListSlot_ExecuteBundle);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps, kListSlot_SetDescriptorHeaps);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootSignature, kListSlot_SetComputeRootSignature);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature, kListSlot_SetGraphicsRootSignature);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable, kListSlot_SetComputeRootDescriptorTable);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable, kListSlot_SetGraphicsRootDescriptorTable);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView, kListSlot_SetComputeRootConstantBufferView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootConstantBufferView, kListSlot_SetGraphicsRootConstantBufferView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView, kListSlot_SetComputeRootShaderResourceView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootShaderResourceView, kListSlot_SetGraphicsRootShaderResourceView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootUnorderedAccessView, kListSlot_SetComputeRootUnorderedAccessView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootUnorderedAccessView, kListSlot_SetGraphicsRootUnorderedAccessView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, IASetIndexBuffer, kListSlot_IASetIndexBuffer);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, IASetVertexBuffers, kListSlot_IASetVertexBuffers);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, SOSetTargets, kListSlot_SOSetTargets);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets, kListSlot_OMSetRenderTargets);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ClearDepthStencilView, kListSlot_ClearDepthStencilView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ClearRenderTargetView, kListSlot_ClearRenderTargetView);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ClearUnorderedAccessViewUint, kListSlot_ClearUnorderedAccessViewUint);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ClearUnorderedAccessViewFloat, kListSlot_ClearUnorderedAccessViewFloat);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, BeginQuery, kListSlot_BeginQuery);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, EndQuery, kListSlot_EndQuery);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ResolveQueryData, kListSlot_ResolveQueryData);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandListVtbl, ExecuteIndirect, kListSlot_ExecuteIndirect);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandList4Vtbl, BeginRenderPass, kListSlot_BeginRenderPass);
D3D12SIM_CHECK_SLOT(ID3D12GraphicsCommandList6Vtbl, DispatchMesh, kListSlot_DispatchMesh);

D3D12SIM_CHECK_SLOT(IDXGIFactoryVtbl, EnumAdapters, kFactorySlot_EnumAdapters);
D3D12SIM_CHECK_SLOT(IDXGIFactoryVtbl, CreateSwapChain, kFactorySlot_CreateSwapChain);
D3D12SIM_CHECK_SLOT(IDXGIFactory1Vtbl, EnumAdapters1, kFactorySlot_EnumAdapters1);
D3D12SIM_CHECK_SLOT(IDXGIFactory2Vtbl, CreateSwapChainForHwnd, kFactorySlot_CreateSwapChainForHwnd);
D3D12SIM_CHECK_SLOT(IDXGIFactory2Vtbl, CreateSwapChainForCoreWindow, kFactorySlot_CreateSwapChainForCoreWindow);
D3D12SIM_CHECK_SLOT(IDXGIFactory2Vtbl, CreateSwapChainForComposition, kFactorySlot_CreateSwapChainForComposition);
D3D12SIM_CHECK_SLOT(IDXGIFactory4Vtbl, EnumAdapterByLuid, kFactorySlot_EnumAdapterByLuid);
D3D12SIM_CHECK_SLOT(IDXGIFactory6Vtbl, EnumAdapterByGpuPreference, kFactorySlot_EnumAdapterByGpuPreference);
D3D12SIM_CHECK_SLOT(IDXGIAdapter3Vtbl, QueryVideoMemoryInfo, kAdapterSlot_QueryVideoMemoryInfo);
D3D12SIM_CHECK_SLOT(IDXGIAdapter3Vtbl, SetVideoMemoryReservation, kAdapterSlot_SetVideoMemoryReservation);
D3D12SIM_CHECK_SLOT(IDXGISwapChainVtbl, GetBuffer, kSwapChainSlot_GetBuffer);
D3D12SIM_CHECK_SLOT(IDXGISwapChainVtbl, ResizeBuffers, kSwapChainSlot_ResizeBuffers);
D3D12SIM_CHECK_SLOT(IDXGISwapChain3Vtbl, ResizeBuffers1, kSwapChainSlot_ResizeBuffers1);