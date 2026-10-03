/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12Tracking
//  -------------
//  What the shim needs to know about live D3D12 objects to check accesses the
//  way linked hardware enforces them:
//
//    * resources and heaps: simulated creation/visible node masks, GPU virtual
//      address range (buffers), memory charged to their node;
//    * descriptor heaps and descriptors: which resource (or GPU address) each
//      CPU descriptor refers to, and how GPU handles map to CPU handles;
//    * root signatures: descriptor table layouts (from the serialized blob).
//
//  Entries live exactly as long as the D3D12 object: a token stored in the
//  object's private data removes the entry when the object is destroyed, so a
//  pointer from a stale descriptor is never dereferenced.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

namespace D3D12Sim
{

// ---- Resources and heaps ----------------------------------------------------

struct ResourceInfo
{
    UINT                      Creation = 1; // normalized: exactly one node bit
    UINT                      Visible  = 1; // normalized: includes Creation
    D3D12_GPU_VIRTUAL_ADDRESS VA       = 0; // buffers only
    UINT64                    Size     = 0; // buffers: width in bytes
    UINT64                    Bytes    = 0; // memory charged to the creation node (committed resources, heaps)
    bool                      Local    = true; // video memory (local segment) or system memory
};

// Registers a resource or heap (pObject: ID3D12Resource / ID3D12Heap / swap chain buffer)
void RegisterResource(IUnknown* pObject, const ResourceInfo& Info);
bool FindResource(const void* pObject, ResourceInfo& Info);
// The buffer whose GPU address range contains VA
const void* FindResourceByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, ResourceInfo& Info);

// Memory charged to a node by the resources and heaps created on it
UINT64 GetTrackedNodeBytes(UINT NodeIndex, bool Local);
UINT64 GetTrackedTotalBytes(bool Local);

// ---- Descriptors --------------------------------------------------------------

enum DESCRIPTOR_KIND : uint8_t
{
    DESCRIPTOR_NONE = 0,
    DESCRIPTOR_CBV,
    DESCRIPTOR_SRV,
    DESCRIPTOR_UAV,
    DESCRIPTOR_RTV,
    DESCRIPTOR_DSV,
};

struct DescriptorInfo
{
    DESCRIPTOR_KIND           Kind      = DESCRIPTOR_NONE;
    const void*               pResource = nullptr; // the viewed resource (may be null)
    const void*               pCounter  = nullptr; // UAV counter resource
    D3D12_GPU_VIRTUAL_ADDRESS VA        = 0;       // CBV, acceleration-structure SRV
};

struct DescriptorHeapInfo
{
    SIZE_T                     CpuStart  = 0;
    UINT64                     GpuStart  = 0; // 0 if not shader visible
    UINT                       Count     = 0;
    UINT                       Increment = 0;
    D3D12_DESCRIPTOR_HEAP_TYPE Type      = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    UINT                       NodeMask  = 1;
};

void RegisterDescriptorHeap(IUnknown* pHeap, const DescriptorHeapInfo& Info);
// The heap containing a CPU handle (and its info)
bool FindDescriptorHeapByCpu(SIZE_T Cpu, DescriptorHeapInfo& Info);
// The heap containing a GPU handle; Cpu receives the matching CPU handle
bool FindDescriptorHeapByGpu(UINT64 Gpu, DescriptorHeapInfo& Info, SIZE_T& Cpu);

void SetDescriptor(SIZE_T Cpu, const DescriptorInfo& Info);
bool GetDescriptor(SIZE_T Cpu, DescriptorInfo& Info);
// Copies Count descriptors (CopyDescriptors / CopyDescriptorsSimple)
void CopyDescriptors(SIZE_T Dst, SIZE_T Src, UINT Count, UINT Increment);

// ---- Root signatures ----------------------------------------------------------

struct RootRange
{
    D3D12_DESCRIPTOR_RANGE_TYPE Type   = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    UINT                        Count  = 0; // UINT_MAX: unbounded
    UINT                        Offset = 0; // from the table start, appends resolved
};

struct RootParameter
{
    D3D12_ROOT_PARAMETER_TYPE Type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    std::vector<RootRange>    Ranges; // descriptor tables
};

using RootLayout = std::vector<RootParameter>;

// Parses a serialized root signature; registers it for pRootSignature
void                              RegisterRootSignature(IUnknown* pRootSignature, const void* pBlob, SIZE_T BlobSize);
std::shared_ptr<const RootLayout> FindRootSignature(const void* pRootSignature);

// ---- Lifetime -----------------------------------------------------------------

// Calls OnDestroy (once) when the D3D12/DXGI object is destroyed. Kind tells
// apart the trackers of one object; registering the same kind again keeps the
// first registration. Returns false if the object has no private data.
bool OnObjectDestroyed(IUnknown* pObject, const GUID& Kind, void (*OnDestroy)(const void* pObject));

} // namespace D3D12Sim
