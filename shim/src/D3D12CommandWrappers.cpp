/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "D3D12CommandWrappers.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "D3D12Tracking.hpp"
#include "D3D12Vtable.hpp"
#include "NodeMasks.hpp"
#include "ShimLog.hpp"
#include "VtableShadow.hpp"

namespace D3D12Sim
{

namespace
{

void PatchQueueVtable(void** Slots, size_t NumSlots);
void PatchListVtable(void** Slots, size_t NumSlots);

VtableShadowSet g_Queues{"ID3D12CommandQueue", &PatchQueueVtable};
VtableShadowSet g_Lists{"ID3D12GraphicsCommandList", &PatchListVtable};

// {7A1C2E40-3B5D-4C6E-8F70-91A2B3C4D510}: lifetime of a command list's tracked state
constexpr GUID kListLifetime = {0x7a1c2e40, 0x3b5d, 0x4c6e, {0x8f, 0x70, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0x10}};
// {7A1C2E40-3B5D-4C6E-8F70-91A2B3C4D511}: UINT, 1 if a command signature records dispatches
constexpr GUID kCommandSignatureKind = {0x7a1c2e40, 0x3b5d, 0x4c6e, {0x8f, 0x70, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0x11}};

// Unbounded descriptor ranges are checked up to this many descriptors
constexpr UINT kMaxUnboundedDescriptors = 1024;

template <typename PFN>
PFN Orig(const void* This, int Slot)
{
    return reinterpret_cast<PFN>(VtableShadowSet::GetOrigVtbl(This)[Slot]);
}

UINT NodeIndexOf(UINT Mask)
{
    UINT Index = 0;
    while (Index < 31 && (Mask & (1u << Index)) == 0)
        ++Index;
    return Index;
}

// ---- Command list state ----------------------------------------------------

struct BindPoint
{
    std::shared_ptr<const RootLayout>      Layout;
    std::vector<UINT64>                    Tables;    // GPU descriptor handle per root parameter
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> RootViews; // root CBV/SRV/UAV address per parameter

    void SetLayout(std::shared_ptr<const RootLayout> NewLayout)
    {
        Layout           = std::move(NewLayout);
        const size_t Num = Layout ? Layout->size() : 0;
        Tables.assign(Num, 0);
        RootViews.assign(Num, 0);
    }
};

struct ListState
{
    UINT                                   NodeMask = 1;
    BindPoint                              Graphics, Compute;
    std::vector<SIZE_T>                    RenderTargets; // CPU descriptors
    SIZE_T                                 DepthStencil = 0;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> VertexBuffers;
    D3D12_GPU_VIRTUAL_ADDRESS              IndexBuffer = 0;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> StreamOutput;
    std::set<std::pair<const void*, int>>  Reported; // one report per resource and access kind per recording

    void ResetBindings()
    {
        Graphics = BindPoint{};
        Compute  = BindPoint{};
        RenderTargets.clear();
        DepthStencil = 0;
        VertexBuffers.clear();
        IndexBuffer = 0;
        StreamOutput.clear();
        Reported.clear();
    }
};

std::mutex                                 g_ListMutex;
std::unordered_map<const void*, ListState> g_ListStates;

void OnListDestroyed(const void* pList)
{
    std::lock_guard<std::mutex> Lock{g_ListMutex};
    g_ListStates.erase(pList);
}

// Runs Fn(ListState&) under the lock if the list is tracked
template <typename F>
void WithState(const void* pList, F&& Fn)
{
    std::lock_guard<std::mutex> Lock{g_ListMutex};
    auto                        It = g_ListStates.find(pList);
    if (It != g_ListStates.end())
        Fn(It->second);
}

void Check(ListState& S, const char* Api, const char* Role, const void* pResource, ACCESS_KIND Kind)
{
    if (pResource == nullptr)
        return;
    const auto Key = std::make_pair(pResource, static_cast<int>(Kind));
    if (S.Reported.count(Key) != 0)
        return;
    if (!CheckResourceAccess(Api, Role, S.NodeMask, pResource, Kind))
        S.Reported.insert(Key);
}

void CheckAddress(ListState& S, const char* Api, const char* Role, D3D12_GPU_VIRTUAL_ADDRESS VA, ACCESS_KIND Kind)
{
    ResourceInfo Info;
    Check(S, Api, Role, FindResourceByVA(VA, Info), Kind);
}

void CheckDescriptor(ListState& S, const char* Api, SIZE_T Cpu, ACCESS_KIND Kind)
{
    DescriptorInfo D;
    if (!GetDescriptor(Cpu, D))
        return;
    const char* Role = D.Kind == DESCRIPTOR_CBV ? "constant buffer" :
        D.Kind == DESCRIPTOR_SRV                ? "shader resource" :
        D.Kind == DESCRIPTOR_UAV                ? "unordered access" :
        D.Kind == DESCRIPTOR_RTV                ? "render target" :
                                                  "depth-stencil";
    Check(S, Api, Role, D.pResource, Kind);
    Check(S, Api, "UAV counter", D.pCounter, Kind);
    if (D.VA != 0)
        CheckAddress(S, Api, Role, D.VA, Kind);
}

// Everything a draw or dispatch can reach through the bound root signature
void CheckBindPoint(ListState& S, const BindPoint& B, const char* Api)
{
    if (!B.Layout)
        return;
    for (size_t p = 0; p < B.Layout->size(); ++p)
    {
        const RootParameter& Param = (*B.Layout)[p];
        if (Param.Type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            DescriptorHeapInfo Heap;
            SIZE_T             TableCpu = 0;
            if (B.Tables[p] == 0 || !FindDescriptorHeapByGpu(B.Tables[p], Heap, TableCpu) || Heap.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
                continue;
            const UINT Remaining = Heap.Count - static_cast<UINT>((TableCpu - Heap.CpuStart) / Heap.Increment);
            for (const RootRange& R : Param.Ranges)
            {
                if (R.Type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER || R.Offset == UINT_MAX)
                    continue;
                const UINT Count = R.Count == UINT_MAX ? kMaxUnboundedDescriptors : R.Count;
                for (UINT i = 0; i < Count && R.Offset + i < Remaining; ++i)
                    CheckDescriptor(S, Api, TableCpu + SIZE_T{R.Offset + i} * Heap.Increment, ACCESS_SHADER);
            }
        }
        else if (Param.Type == D3D12_ROOT_PARAMETER_TYPE_CBV || Param.Type == D3D12_ROOT_PARAMETER_TYPE_SRV || Param.Type == D3D12_ROOT_PARAMETER_TYPE_UAV)
        {
            if (B.RootViews[p] != 0)
                CheckAddress(S, Api, "root view", B.RootViews[p], ACCESS_SHADER);
        }
    }
}

void CheckGraphicsState(ListState& S, const char* Api, bool InputAssembler)
{
    CheckBindPoint(S, S.Graphics, Api);
    for (SIZE_T Rtv : S.RenderTargets)
        CheckDescriptor(S, Api, Rtv, ACCESS_TARGET);
    if (S.DepthStencil != 0)
        CheckDescriptor(S, Api, S.DepthStencil, ACCESS_TARGET);
    if (InputAssembler)
    {
        for (D3D12_GPU_VIRTUAL_ADDRESS VA : S.VertexBuffers)
            CheckAddress(S, Api, "vertex buffer", VA, ACCESS_SHADER);
        CheckAddress(S, Api, "index buffer", S.IndexBuffer, ACCESS_SHADER);
    }
    for (D3D12_GPU_VIRTUAL_ADDRESS VA : S.StreamOutput)
        CheckAddress(S, Api, "stream-output buffer", VA, ACCESS_SHADER);
}

// An object created for a set of nodes (pipeline state, root signature,
// command signature, bundle) used on a list of another node
void CheckObjectNodes(const char* Api, const char* What, IUnknown* pObject, UINT ListNodeMask)
{
    UINT Mask = 0;
    if (ValidationEnabled() && pObject != nullptr && GetObjectNodeMask(pObject, Mask) && (Mask & ListNodeMask) == 0)
        ReportValidationError("%s: the %s %p was created for node mask 0x%X and cannot be used on node %u", Api, What, pObject, Mask, NodeIndexOf(ListNodeMask));
}

// An object of exactly one node (descriptor heap, query heap)
void CheckObjectNode(const char* Api, const char* What, IUnknown* pObject, UINT ListNodeMask)
{
    UINT Mask = 0;
    if (ValidationEnabled() && pObject != nullptr && GetObjectNodeMask(pObject, Mask) && Mask != ListNodeMask)
        ReportValidationError("%s: the %s %p belongs to node %u, the command list to node %u", Api, What, pObject, NodeIndexOf(Mask), NodeIndexOf(ListNodeMask));
}

UINT ListNodeMask(ID3D12GraphicsCommandList* This)
{
    UINT Mask = 1;
    GetObjectNodeMask(This, Mask);
    return Mask;
}

// ---- ID3D12CommandQueue --------------------------------------------------

D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE Hook_Queue_GetDesc(ID3D12CommandQueue* This, D3D12_COMMAND_QUEUE_DESC* pRetVal)
{
    using PFN                       = D3D12_COMMAND_QUEUE_DESC*(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, D3D12_COMMAND_QUEUE_DESC*);
    D3D12_COMMAND_QUEUE_DESC* pDesc = Orig<PFN>(This, kQueueSlot_GetDesc)(This, pRetVal);
    UINT                      Mask  = 0;
    if (pDesc != nullptr && GetObjectNodeMask(This, Mask))
        pDesc->NodeMask = Mask;
    return pDesc;
}

void STDMETHODCALLTYPE Hook_Queue_ExecuteCommandLists(ID3D12CommandQueue* This, UINT NumLists, ID3D12CommandList* const* ppLists)
{
    using PFN      = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    UINT QueueNode = 0;
    if (ValidationEnabled() && ppLists != nullptr && GetObjectNodeMask(This, QueueNode))
    {
        for (UINT i = 0; i < NumLists; ++i)
        {
            UINT ListNode = 0;
            if (ppLists[i] != nullptr && GetObjectNodeMask(ppLists[i], ListNode) && ListNode != QueueNode)
                ReportValidationError("ExecuteCommandLists: command list %p of node %u is executed on a queue of node %u", ppLists[i], NodeIndexOf(ListNode), NodeIndexOf(QueueNode));
        }
    }
    Orig<PFN>(This, kQueueSlot_ExecuteCommandLists)(This, NumLists, ppLists);
}

void PatchQueueVtable(void** Slots, size_t NumSlots)
{
    if (kQueueSlot_GetDesc < static_cast<int>(NumSlots))
    {
        Slots[kQueueSlot_GetDesc]             = reinterpret_cast<void*>(&Hook_Queue_GetDesc);
        Slots[kQueueSlot_ExecuteCommandLists] = reinterpret_cast<void*>(&Hook_Queue_ExecuteCommandLists);
    }
}

// ---- ID3D12GraphicsCommandList: recording state ----------------------------

HRESULT STDMETHODCALLTYPE Hook_List_Reset(ID3D12GraphicsCommandList* This, ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    CheckObjectNodes("Reset", "pipeline state", pInitialState, ListNodeMask(This));
    WithState(This, [](ListState& S) { S.ResetBindings(); });
    return Orig<PFN>(This, kListSlot_Reset)(This, pAllocator, pInitialState);
}

void STDMETHODCALLTYPE Hook_List_SetPipelineState(ID3D12GraphicsCommandList* This, ID3D12PipelineState* pState)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
    CheckObjectNodes("SetPipelineState", "pipeline state", pState, ListNodeMask(This));
    Orig<PFN>(This, kListSlot_SetPipelineState)(This, pState);
}

void STDMETHODCALLTYPE Hook_List_ExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* pBundle)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
    UINT BundleNode = 0;
    if (ValidationEnabled() && pBundle != nullptr && GetObjectNodeMask(pBundle, BundleNode) && BundleNode != ListNodeMask(This))
        ReportValidationError("ExecuteBundle: bundle %p of node %u is executed on a command list of node %u", pBundle, NodeIndexOf(BundleNode), NodeIndexOf(ListNodeMask(This)));
    Orig<PFN>(This, kListSlot_ExecuteBundle)(This, pBundle);
}

void STDMETHODCALLTYPE Hook_List_SetDescriptorHeaps(ID3D12GraphicsCommandList* This, UINT NumHeaps, ID3D12DescriptorHeap* const* ppHeaps)
{
    using PFN       = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
    const UINT Node = ListNodeMask(This);
    for (UINT i = 0; ppHeaps != nullptr && i < NumHeaps; ++i)
        CheckObjectNode("SetDescriptorHeaps", "descriptor heap", ppHeaps[i], Node);
    Orig<PFN>(This, kListSlot_SetDescriptorHeaps)(This, NumHeaps, ppHeaps);
}

void SetRootSignature(ID3D12GraphicsCommandList* This, ID3D12RootSignature* pRootSignature, bool Compute, const char* Api)
{
    CheckObjectNodes(Api, "root signature", pRootSignature, ListNodeMask(This));
    auto Layout = FindRootSignature(pRootSignature);
    WithState(This, [&](ListState& S) { (Compute ? S.Compute : S.Graphics).SetLayout(Layout); });
}

void STDMETHODCALLTYPE Hook_List_SetComputeRootSignature(ID3D12GraphicsCommandList* This, ID3D12RootSignature* pRootSignature)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
    SetRootSignature(This, pRootSignature, true, "SetComputeRootSignature");
    Orig<PFN>(This, kListSlot_SetComputeRootSignature)(This, pRootSignature);
}

void STDMETHODCALLTYPE Hook_List_SetGraphicsRootSignature(ID3D12GraphicsCommandList* This, ID3D12RootSignature* pRootSignature)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
    SetRootSignature(This, pRootSignature, false, "SetGraphicsRootSignature");
    Orig<PFN>(This, kListSlot_SetGraphicsRootSignature)(This, pRootSignature);
}

void SetTable(ID3D12GraphicsCommandList* This, bool Compute, UINT Index, UINT64 Gpu)
{
    WithState(This, [&](ListState& S) {
        BindPoint& B = Compute ? S.Compute : S.Graphics;
        if (Index < B.Tables.size())
            B.Tables[Index] = Gpu;
    });
}

void SetRootView(ID3D12GraphicsCommandList* This, bool Compute, UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA)
{
    WithState(This, [&](ListState& S) {
        BindPoint& B = Compute ? S.Compute : S.Graphics;
        if (Index < B.RootViews.size())
            B.RootViews[Index] = VA;
    });
}

void STDMETHODCALLTYPE Hook_List_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE Handle)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    SetTable(This, true, Index, Handle.ptr);
    Orig<PFN>(This, kListSlot_SetComputeRootDescriptorTable)(This, Index, Handle);
}

void STDMETHODCALLTYPE Hook_List_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE Handle)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
    SetTable(This, false, Index, Handle.ptr);
    Orig<PFN>(This, kListSlot_SetGraphicsRootDescriptorTable)(This, Index, Handle);
}

#define D3D12SIM_ROOT_VIEW_HOOK(Name, Compute)                                                                   \
    void STDMETHODCALLTYPE Hook_List_##Name(ID3D12GraphicsCommandList* This, UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) \
    {                                                                                                            \
        using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);       \
        SetRootView(This, Compute, Index, VA);                                                                   \
        Orig<PFN>(This, kListSlot_##Name)(This, Index, VA);                                                      \
    }
D3D12SIM_ROOT_VIEW_HOOK(SetComputeRootConstantBufferView, true)
D3D12SIM_ROOT_VIEW_HOOK(SetGraphicsRootConstantBufferView, false)
D3D12SIM_ROOT_VIEW_HOOK(SetComputeRootShaderResourceView, true)
D3D12SIM_ROOT_VIEW_HOOK(SetGraphicsRootShaderResourceView, false)
D3D12SIM_ROOT_VIEW_HOOK(SetComputeRootUnorderedAccessView, true)
D3D12SIM_ROOT_VIEW_HOOK(SetGraphicsRootUnorderedAccessView, false)
#undef D3D12SIM_ROOT_VIEW_HOOK

void STDMETHODCALLTYPE Hook_List_IASetIndexBuffer(ID3D12GraphicsCommandList* This, const D3D12_INDEX_BUFFER_VIEW* pView)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
    WithState(This, [&](ListState& S) { S.IndexBuffer = pView != nullptr ? pView->BufferLocation : 0; });
    Orig<PFN>(This, kListSlot_IASetIndexBuffer)(This, pView);
}

void STDMETHODCALLTYPE Hook_List_IASetVertexBuffers(ID3D12GraphicsCommandList* This, UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
    WithState(This, [&](ListState& S) {
        if (S.VertexBuffers.size() < size_t{StartSlot} + NumViews)
            S.VertexBuffers.resize(size_t{StartSlot} + NumViews, 0);
        for (UINT i = 0; i < NumViews; ++i)
            S.VertexBuffers[StartSlot + i] = pViews != nullptr ? pViews[i].BufferLocation : 0;
    });
    Orig<PFN>(This, kListSlot_IASetVertexBuffers)(This, StartSlot, NumViews, pViews);
}

void STDMETHODCALLTYPE Hook_List_SOSetTargets(ID3D12GraphicsCommandList* This, UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*);
    WithState(This, [&](ListState& S) {
        if (S.StreamOutput.size() < size_t{StartSlot} + NumViews * 2)
            S.StreamOutput.resize(size_t{StartSlot} + NumViews * 2, 0);
        for (UINT i = 0; i < NumViews; ++i)
        {
            S.StreamOutput[StartSlot + i * 2]     = pViews != nullptr ? pViews[i].BufferLocation : 0;
            S.StreamOutput[StartSlot + i * 2 + 1] = pViews != nullptr ? pViews[i].BufferFilledSizeLocation : 0;
        }
    });
    Orig<PFN>(This, kListSlot_SOSetTargets)(This, StartSlot, NumViews, pViews);
}

void STDMETHODCALLTYPE Hook_List_OMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRTs, const D3D12_CPU_DESCRIPTOR_HANDLE* pRTs, BOOL SingleRange,
                                                    const D3D12_CPU_DESCRIPTOR_HANDLE* pDSV)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    std::vector<SIZE_T> Targets;
    if (pRTs != nullptr && NumRTs > 0)
    {
        DescriptorHeapInfo Heap;
        const UINT         Increment = SingleRange && FindDescriptorHeapByCpu(pRTs[0].ptr, Heap) ? Heap.Increment : 0;
        for (UINT i = 0; i < NumRTs; ++i)
            Targets.push_back(SingleRange ? pRTs[0].ptr + SIZE_T{i} * Increment : pRTs[i].ptr);
    }
    WithState(This, [&](ListState& S) {
        S.RenderTargets = Targets;
        S.DepthStencil  = pDSV != nullptr ? pDSV->ptr : 0;
    });
    Orig<PFN>(This, kListSlot_OMSetRenderTargets)(This, NumRTs, pRTs, SingleRange, pDSV);
}

// ---- ID3D12GraphicsCommandList: commands that access resources -------------

void STDMETHODCALLTYPE Hook_List_DrawInstanced(ID3D12GraphicsCommandList* This, UINT Vertices, UINT Instances, UINT StartVertex, UINT StartInstance)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
    WithState(This, [&](ListState& S) { CheckGraphicsState(S, "DrawInstanced", true); });
    Orig<PFN>(This, kListSlot_DrawInstanced)(This, Vertices, Instances, StartVertex, StartInstance);
}

void STDMETHODCALLTYPE Hook_List_DrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT Indices, UINT Instances, UINT StartIndex, INT BaseVertex, UINT StartInstance)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
    WithState(This, [&](ListState& S) { CheckGraphicsState(S, "DrawIndexedInstanced", true); });
    Orig<PFN>(This, kListSlot_DrawIndexedInstanced)(This, Indices, Instances, StartIndex, BaseVertex, StartInstance);
}

void STDMETHODCALLTYPE Hook_List_Dispatch(ID3D12GraphicsCommandList* This, UINT X, UINT Y, UINT Z)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
    WithState(This, [&](ListState& S) { CheckBindPoint(S, S.Compute, "Dispatch"); });
    Orig<PFN>(This, kListSlot_Dispatch)(This, X, Y, Z);
}

void STDMETHODCALLTYPE Hook_List_DispatchMesh(ID3D12GraphicsCommandList* This, UINT X, UINT Y, UINT Z)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
    WithState(This, [&](ListState& S) { CheckGraphicsState(S, "DispatchMesh", false); });
    Orig<PFN>(This, kListSlot_DispatchMesh)(This, X, Y, Z);
}

void STDMETHODCALLTYPE Hook_List_ExecuteIndirect(ID3D12GraphicsCommandList* This, ID3D12CommandSignature* pSignature, UINT MaxCount, ID3D12Resource* pArgs, UINT64 ArgsOffset,
                                                 ID3D12Resource* pCount, UINT64 CountOffset)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
    CheckObjectNodes("ExecuteIndirect", "command signature", pSignature, ListNodeMask(This));
    UINT Compute = 0, Size = sizeof(Compute);
    if (pSignature != nullptr)
        pSignature->GetPrivateData(kCommandSignatureKind, &Size, &Compute);
    WithState(This, [&](ListState& S) {
        Check(S, "ExecuteIndirect", "argument buffer", pArgs, ACCESS_SHADER);
        Check(S, "ExecuteIndirect", "count buffer", pCount, ACCESS_SHADER);
        if (Compute != 0)
            CheckBindPoint(S, S.Compute, "ExecuteIndirect");
        else
            CheckGraphicsState(S, "ExecuteIndirect", true);
    });
    Orig<PFN>(This, kListSlot_ExecuteIndirect)(This, pSignature, MaxCount, pArgs, ArgsOffset, pCount, CountOffset);
}

void STDMETHODCALLTYPE Hook_List_CopyBufferRegion(ID3D12GraphicsCommandList* This, ID3D12Resource* pDst, UINT64 DstOffset, ID3D12Resource* pSrc, UINT64 SrcOffset,
                                                  UINT64 NumBytes)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
    WithState(This, [&](ListState& S) {
        Check(S, "CopyBufferRegion", "destination", pDst, ACCESS_COPY);
        Check(S, "CopyBufferRegion", "source", pSrc, ACCESS_COPY);
    });
    Orig<PFN>(This, kListSlot_CopyBufferRegion)(This, pDst, DstOffset, pSrc, SrcOffset, NumBytes);
}

void STDMETHODCALLTYPE Hook_List_CopyTextureRegion(ID3D12GraphicsCommandList* This, const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
                                                   const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
    WithState(This, [&](ListState& S) {
        Check(S, "CopyTextureRegion", "destination", pDst != nullptr ? pDst->pResource : nullptr, ACCESS_COPY);
        Check(S, "CopyTextureRegion", "source", pSrc != nullptr ? pSrc->pResource : nullptr, ACCESS_COPY);
    });
    Orig<PFN>(This, kListSlot_CopyTextureRegion)(This, pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}

void STDMETHODCALLTYPE Hook_List_CopyResource(ID3D12GraphicsCommandList* This, ID3D12Resource* pDst, ID3D12Resource* pSrc)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
    WithState(This, [&](ListState& S) {
        Check(S, "CopyResource", "destination", pDst, ACCESS_COPY);
        Check(S, "CopyResource", "source", pSrc, ACCESS_COPY);
    });
    Orig<PFN>(This, kListSlot_CopyResource)(This, pDst, pSrc);
}

void STDMETHODCALLTYPE Hook_List_CopyTiles(ID3D12GraphicsCommandList* This, ID3D12Resource* pTiled, const D3D12_TILED_RESOURCE_COORDINATE* pStart,
                                           const D3D12_TILE_REGION_SIZE* pSize, ID3D12Resource* pBuffer, UINT64 BufferOffset, D3D12_TILE_COPY_FLAGS Flags)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Resource*,
                                         UINT64, D3D12_TILE_COPY_FLAGS);
    WithState(This, [&](ListState& S) {
        Check(S, "CopyTiles", "tiled", pTiled, ACCESS_COPY);
        Check(S, "CopyTiles", "buffer", pBuffer, ACCESS_COPY);
    });
    Orig<PFN>(This, kListSlot_CopyTiles)(This, pTiled, pStart, pSize, pBuffer, BufferOffset, Flags);
}

void STDMETHODCALLTYPE Hook_List_ResolveSubresource(ID3D12GraphicsCommandList* This, ID3D12Resource* pDst, UINT DstSub, ID3D12Resource* pSrc, UINT SrcSub, DXGI_FORMAT Format)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
    WithState(This, [&](ListState& S) {
        Check(S, "ResolveSubresource", "destination", pDst, ACCESS_COPY);
        Check(S, "ResolveSubresource", "source", pSrc, ACCESS_COPY);
    });
    Orig<PFN>(This, kListSlot_ResolveSubresource)(This, pDst, DstSub, pSrc, SrcSub, Format);
}

void STDMETHODCALLTYPE Hook_List_ClearDepthStencilView(ID3D12GraphicsCommandList* This, D3D12_CPU_DESCRIPTOR_HANDLE Dsv, D3D12_CLEAR_FLAGS Flags, FLOAT Depth, UINT8 Stencil,
                                                       UINT NumRects, const D3D12_RECT* pRects)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
    WithState(This, [&](ListState& S) { CheckDescriptor(S, "ClearDepthStencilView", Dsv.ptr, ACCESS_TARGET); });
    Orig<PFN>(This, kListSlot_ClearDepthStencilView)(This, Dsv, Flags, Depth, Stencil, NumRects, pRects);
}

void STDMETHODCALLTYPE Hook_List_ClearRenderTargetView(ID3D12GraphicsCommandList* This, D3D12_CPU_DESCRIPTOR_HANDLE Rtv, const FLOAT Color[4], UINT NumRects,
                                                       const D3D12_RECT* pRects)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
    WithState(This, [&](ListState& S) { CheckDescriptor(S, "ClearRenderTargetView", Rtv.ptr, ACCESS_TARGET); });
    Orig<PFN>(This, kListSlot_ClearRenderTargetView)(This, Rtv, Color, NumRects, pRects);
}

void STDMETHODCALLTYPE Hook_List_ClearUnorderedAccessViewUint(ID3D12GraphicsCommandList* This, D3D12_GPU_DESCRIPTOR_HANDLE Gpu, D3D12_CPU_DESCRIPTOR_HANDLE Cpu,
                                                              ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT*, UINT, const D3D12_RECT*);
    WithState(This, [&](ListState& S) { Check(S, "ClearUnorderedAccessViewUint", "unordered access", pResource, ACCESS_SHADER); });
    Orig<PFN>(This, kListSlot_ClearUnorderedAccessViewUint)(This, Gpu, Cpu, pResource, Values, NumRects, pRects);
}

void STDMETHODCALLTYPE Hook_List_ClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList* This, D3D12_GPU_DESCRIPTOR_HANDLE Gpu, D3D12_CPU_DESCRIPTOR_HANDLE Cpu,
                                                               ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const FLOAT*, UINT, const D3D12_RECT*);
    WithState(This, [&](ListState& S) { Check(S, "ClearUnorderedAccessViewFloat", "unordered access", pResource, ACCESS_SHADER); });
    Orig<PFN>(This, kListSlot_ClearUnorderedAccessViewFloat)(This, Gpu, Cpu, pResource, Values, NumRects, pRects);
}

void STDMETHODCALLTYPE Hook_List_BeginQuery(ID3D12GraphicsCommandList* This, ID3D12QueryHeap* pHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
    CheckObjectNode("BeginQuery", "query heap", pHeap, ListNodeMask(This));
    Orig<PFN>(This, kListSlot_BeginQuery)(This, pHeap, Type, Index);
}

void STDMETHODCALLTYPE Hook_List_EndQuery(ID3D12GraphicsCommandList* This, ID3D12QueryHeap* pHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
    CheckObjectNode("EndQuery", "query heap", pHeap, ListNodeMask(This));
    Orig<PFN>(This, kListSlot_EndQuery)(This, pHeap, Type, Index);
}

void STDMETHODCALLTYPE Hook_List_ResolveQueryData(ID3D12GraphicsCommandList* This, ID3D12QueryHeap* pHeap, D3D12_QUERY_TYPE Type, UINT Start, UINT Num, ID3D12Resource* pDst,
                                                  UINT64 DstOffset)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64);
    CheckObjectNode("ResolveQueryData", "query heap", pHeap, ListNodeMask(This));
    WithState(This, [&](ListState& S) { Check(S, "ResolveQueryData", "destination", pDst, ACCESS_COPY); });
    Orig<PFN>(This, kListSlot_ResolveQueryData)(This, pHeap, Type, Start, Num, pDst, DstOffset);
}

void STDMETHODCALLTYPE Hook_List_BeginRenderPass(ID3D12GraphicsCommandList* This, UINT NumRTs, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRTs,
                                                 const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDS, D3D12_RENDER_PASS_FLAGS Flags)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                         D3D12_RENDER_PASS_FLAGS);
    WithState(This, [&](ListState& S) {
        S.RenderTargets.clear();
        for (UINT i = 0; pRTs != nullptr && i < NumRTs; ++i)
            S.RenderTargets.push_back(pRTs[i].cpuDescriptor.ptr);
        S.DepthStencil = pDS != nullptr ? pDS->cpuDescriptor.ptr : 0;
        for (SIZE_T Rtv : S.RenderTargets)
            CheckDescriptor(S, "BeginRenderPass", Rtv, ACCESS_TARGET);
        if (S.DepthStencil != 0)
            CheckDescriptor(S, "BeginRenderPass", S.DepthStencil, ACCESS_TARGET);
    });
    Orig<PFN>(This, kListSlot_BeginRenderPass)(This, NumRTs, pRTs, pDS, Flags);
}

void PatchListVtable(void** Slots, size_t NumSlots)
{
    auto Set = [&](int Slot, auto* pHook) {
        if (static_cast<size_t>(Slot) < NumSlots)
            Slots[Slot] = reinterpret_cast<void*>(pHook);
    };
    Set(kListSlot_Reset, &Hook_List_Reset);
    Set(kListSlot_DrawInstanced, &Hook_List_DrawInstanced);
    Set(kListSlot_DrawIndexedInstanced, &Hook_List_DrawIndexedInstanced);
    Set(kListSlot_Dispatch, &Hook_List_Dispatch);
    Set(kListSlot_CopyBufferRegion, &Hook_List_CopyBufferRegion);
    Set(kListSlot_CopyTextureRegion, &Hook_List_CopyTextureRegion);
    Set(kListSlot_CopyResource, &Hook_List_CopyResource);
    Set(kListSlot_CopyTiles, &Hook_List_CopyTiles);
    Set(kListSlot_ResolveSubresource, &Hook_List_ResolveSubresource);
    Set(kListSlot_SetPipelineState, &Hook_List_SetPipelineState);
    Set(kListSlot_ExecuteBundle, &Hook_List_ExecuteBundle);
    Set(kListSlot_SetDescriptorHeaps, &Hook_List_SetDescriptorHeaps);
    Set(kListSlot_SetComputeRootSignature, &Hook_List_SetComputeRootSignature);
    Set(kListSlot_SetGraphicsRootSignature, &Hook_List_SetGraphicsRootSignature);
    Set(kListSlot_SetComputeRootDescriptorTable, &Hook_List_SetComputeRootDescriptorTable);
    Set(kListSlot_SetGraphicsRootDescriptorTable, &Hook_List_SetGraphicsRootDescriptorTable);
    Set(kListSlot_SetComputeRootConstantBufferView, &Hook_List_SetComputeRootConstantBufferView);
    Set(kListSlot_SetGraphicsRootConstantBufferView, &Hook_List_SetGraphicsRootConstantBufferView);
    Set(kListSlot_SetComputeRootShaderResourceView, &Hook_List_SetComputeRootShaderResourceView);
    Set(kListSlot_SetGraphicsRootShaderResourceView, &Hook_List_SetGraphicsRootShaderResourceView);
    Set(kListSlot_SetComputeRootUnorderedAccessView, &Hook_List_SetComputeRootUnorderedAccessView);
    Set(kListSlot_SetGraphicsRootUnorderedAccessView, &Hook_List_SetGraphicsRootUnorderedAccessView);
    Set(kListSlot_IASetIndexBuffer, &Hook_List_IASetIndexBuffer);
    Set(kListSlot_IASetVertexBuffers, &Hook_List_IASetVertexBuffers);
    Set(kListSlot_SOSetTargets, &Hook_List_SOSetTargets);
    Set(kListSlot_OMSetRenderTargets, &Hook_List_OMSetRenderTargets);
    Set(kListSlot_ClearDepthStencilView, &Hook_List_ClearDepthStencilView);
    Set(kListSlot_ClearRenderTargetView, &Hook_List_ClearRenderTargetView);
    Set(kListSlot_ClearUnorderedAccessViewUint, &Hook_List_ClearUnorderedAccessViewUint);
    Set(kListSlot_ClearUnorderedAccessViewFloat, &Hook_List_ClearUnorderedAccessViewFloat);
    Set(kListSlot_BeginQuery, &Hook_List_BeginQuery);
    Set(kListSlot_EndQuery, &Hook_List_EndQuery);
    Set(kListSlot_ResolveQueryData, &Hook_List_ResolveQueryData);
    Set(kListSlot_ExecuteIndirect, &Hook_List_ExecuteIndirect);
    // ID3D12GraphicsCommandList4 / 6: only reachable on lists that implement them
    Set(kListSlot_BeginRenderPass, &Hook_List_BeginRenderPass);
    Set(kListSlot_DispatchMesh, &Hook_List_DispatchMesh);
}

} // namespace

void WrapCommandQueue(IUnknown* pQueue, unsigned NodeMask)
{
    SetObjectNodeMask(pQueue, NodeMask);
    LogVerbose("Command queue %p created for node mask 0x%X", pQueue, NodeMask);
    if (!g_Queues.Wrap(pQueue))
        LogError("Command queue %p could not be wrapped", pQueue);
}

void WrapCommandList(IUnknown* pList, unsigned NodeMask)
{
    SetObjectNodeMask(pList, NodeMask);
    {
        std::lock_guard<std::mutex> Lock{g_ListMutex};
        ListState&                  S = g_ListStates[pList];
        S                             = ListState{};
        S.NodeMask                    = NormalizeSingleNode(NodeMask);
    }
    OnObjectDestroyed(pList, kListLifetime, &OnListDestroyed);
    if (!g_Lists.Wrap(pList))
        LogError("Command list %p could not be wrapped", pList);
}

void RegisterCommandSignature(IUnknown* pSignature, unsigned NodeMask, bool Compute)
{
    SetObjectNodeMask(pSignature, NodeMask);
    ID3D12Object* pObject = nullptr;
    if (SUCCEEDED(pSignature->QueryInterface(IID_PPV_ARGS(&pObject))))
    {
        const UINT Kind = Compute ? 1u : 0u;
        pObject->SetPrivateData(kCommandSignatureKind, sizeof(Kind), &Kind);
        pObject->Release();
    }
}

} // namespace D3D12Sim
