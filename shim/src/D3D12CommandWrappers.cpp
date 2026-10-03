/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "D3D12CommandWrappers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

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

// Reports a copy that touches a resource its node cannot see
void CheckVisible(const char* Api, const char* Role, ID3D12GraphicsCommandList* pList, ID3D12Resource* pResource)
{
    if (!ValidationEnabled() || pResource == nullptr)
        return;
    UINT ListNode = 0, Creation = 0, Visible = 0;
    if (!GetObjectNodeMask(pList, ListNode) || !GetResourceNodeMasks(pResource, Creation, Visible))
        return; // not created through the shim (e.g. swap chain buffers)
    if ((Visible & ListNode) == 0)
    {
        ReportValidationError("%s: the %s resource %p (CreationNodeMask 0x%X, VisibleNodeMask 0x%X) is not visible to node %u of the command list",
                              Api, Role, pResource, Creation, Visible, NodeIndexOf(ListNode));
    }
}

// ---- ID3D12CommandQueue --------------------------------------------------

D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE Hook_Queue_GetDesc(ID3D12CommandQueue* This, D3D12_COMMAND_QUEUE_DESC* pRetVal)
{
    using PFN                        = D3D12_COMMAND_QUEUE_DESC*(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, D3D12_COMMAND_QUEUE_DESC*);
    D3D12_COMMAND_QUEUE_DESC* pDesc = Orig<PFN>(This, kQueueSlot_GetDesc)(This, pRetVal);
    UINT                      NodeMask = 0;
    if (pDesc != nullptr && GetObjectNodeMask(This, NodeMask))
        pDesc->NodeMask = NodeMask;
    LogVerbose("ID3D12CommandQueue::GetDesc(%p) -> NodeMask 0x%X", This, pDesc != nullptr ? pDesc->NodeMask : 0u);
    return pDesc;
}

void STDMETHODCALLTYPE Hook_Queue_ExecuteCommandLists(ID3D12CommandQueue* This, UINT NumLists, ID3D12CommandList* const* ppLists)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    UINT QueueNode = 0;
    if (ValidationEnabled() && ppLists != nullptr && GetObjectNodeMask(This, QueueNode))
    {
        for (UINT i = 0; i < NumLists; ++i)
        {
            UINT ListNode = 0;
            if (ppLists[i] != nullptr && GetObjectNodeMask(ppLists[i], ListNode) && ListNode != QueueNode)
            {
                ReportValidationError("ExecuteCommandLists: command list %p of node %u is executed on a queue of node %u",
                                      ppLists[i], NodeIndexOf(ListNode), NodeIndexOf(QueueNode));
            }
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

// ---- ID3D12GraphicsCommandList -------------------------------------------

void STDMETHODCALLTYPE Hook_List_CopyBufferRegion(ID3D12GraphicsCommandList* This, ID3D12Resource* pDst, UINT64 DstOffset, ID3D12Resource* pSrc, UINT64 SrcOffset,
                                                  UINT64 NumBytes)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
    CheckVisible("CopyBufferRegion", "destination", This, pDst);
    CheckVisible("CopyBufferRegion", "source", This, pSrc);
    Orig<PFN>(This, kListSlot_CopyBufferRegion)(This, pDst, DstOffset, pSrc, SrcOffset, NumBytes);
}

void STDMETHODCALLTYPE Hook_List_CopyTextureRegion(ID3D12GraphicsCommandList* This, const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
                                                   const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
    CheckVisible("CopyTextureRegion", "destination", This, pDst != nullptr ? pDst->pResource : nullptr);
    CheckVisible("CopyTextureRegion", "source", This, pSrc != nullptr ? pSrc->pResource : nullptr);
    Orig<PFN>(This, kListSlot_CopyTextureRegion)(This, pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}

void STDMETHODCALLTYPE Hook_List_CopyResource(ID3D12GraphicsCommandList* This, ID3D12Resource* pDst, ID3D12Resource* pSrc)
{
    using PFN = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
    CheckVisible("CopyResource", "destination", This, pDst);
    CheckVisible("CopyResource", "source", This, pSrc);
    Orig<PFN>(This, kListSlot_CopyResource)(This, pDst, pSrc);
}

void PatchListVtable(void** Slots, size_t NumSlots)
{
    if (kListSlot_CopyResource < static_cast<int>(NumSlots))
    {
        Slots[kListSlot_CopyBufferRegion]  = reinterpret_cast<void*>(&Hook_List_CopyBufferRegion);
        Slots[kListSlot_CopyTextureRegion] = reinterpret_cast<void*>(&Hook_List_CopyTextureRegion);
        Slots[kListSlot_CopyResource]      = reinterpret_cast<void*>(&Hook_List_CopyResource);
    }
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
    if (!g_Lists.Wrap(pList))
        LogError("Command list %p could not be wrapped", pList);
}

} // namespace D3D12Sim
