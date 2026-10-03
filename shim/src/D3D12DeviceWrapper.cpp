/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "D3D12DeviceWrapper.hpp"

#include <cstring>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "D3D12CommandWrappers.hpp"
#include "D3D12Vtable.hpp"
#include "NodeMasks.hpp"
#include "ShimConfig.hpp"
#include "ShimLog.hpp"
#include "VtableShadow.hpp"

namespace D3D12Sim
{

namespace
{

void PatchDeviceVtable(void** Slots, size_t NumSlots);

VtableShadowSet g_Devices{"ID3D12Device", &PatchDeviceVtable};

template <typename PFN>
PFN Orig(const void* This, int Slot)
{
    return reinterpret_cast<PFN>(VtableShadowSet::GetOrigVtbl(This)[Slot]);
}

IUnknown* AsUnknown(void** ppObject)
{
    return ppObject != nullptr ? static_cast<IUnknown*>(*ppObject) : nullptr;
}

// Graphics command lists share the vtable layout the list hooks patch; video
// command lists do not
bool IsGraphicsCommandListType(D3D12_COMMAND_LIST_TYPE Type)
{
    return Type == D3D12_COMMAND_LIST_TYPE_DIRECT || Type == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
        Type == D3D12_COMMAND_LIST_TYPE_COMPUTE || Type == D3D12_COMMAND_LIST_TYPE_COPY;
}

// Heap properties as the physical node accepts them
D3D12_HEAP_PROPERTIES PhysicalHeapProperties(const D3D12_HEAP_PROPERTIES& Props)
{
    D3D12_HEAP_PROPERTIES Local = Props;
    Local.CreationNodeMask      = ToPhysicalMask(Props.CreationNodeMask);
    Local.VisibleNodeMask       = ToPhysicalMask(Props.VisibleNodeMask);
    return Local;
}

void OnQueueCreated(HRESULT hr, void** ppQueue, UINT NodeMask)
{
    if (SUCCEEDED(hr) && AsUnknown(ppQueue) != nullptr)
        WrapCommandQueue(AsUnknown(ppQueue), NodeMask);
}

void OnCommandListCreated(HRESULT hr, void** ppList, D3D12_COMMAND_LIST_TYPE Type, UINT NodeMask)
{
    if (SUCCEEDED(hr) && AsUnknown(ppList) != nullptr && IsGraphicsCommandListType(Type))
        WrapCommandList(AsUnknown(ppList), NodeMask);
}

void OnResourceCreated(HRESULT hr, void** ppResource, const D3D12_HEAP_PROPERTIES& Props)
{
    if (SUCCEEDED(hr) && AsUnknown(ppResource) != nullptr)
        SetResourceNodeMasks(AsUnknown(ppResource), Props.CreationNodeMask, Props.VisibleNodeMask);
}

void OnPlacedResourceCreated(HRESULT hr, void** ppResource, ID3D12Heap* pHeap)
{
    UINT Creation = 0, Visible = 0;
    if (SUCCEEDED(hr) && AsUnknown(ppResource) != nullptr && GetResourceNodeMasks(pHeap, Creation, Visible))
        SetResourceNodeMasks(AsUnknown(ppResource), Creation, Visible);
}

// ---------------------------------------------------------------------------
// Pipeline state streams (ID3D12Device2::CreatePipelineState)
//
// Each subobject is { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE; payload } aligned to
// a pointer. Finding the NODE_MASK subobject requires the size of every
// payload type, so the table covers all types of this SDK; a stream with an
// unknown type is passed through unchanged (and logged).
// ---------------------------------------------------------------------------

template <typename T>
constexpr size_t SubobjectSize()
{
    constexpr size_t Ptr    = sizeof(void*);
    constexpr size_t Offset = (sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + alignof(T) - 1) / alignof(T) * alignof(T);
    return (Offset + sizeof(T) + Ptr - 1) / Ptr * Ptr;
}

size_t GetSubobjectSize(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type)
{
    switch (Type)
    {
        // clang-format off
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:        return SubobjectSize<ID3D12RootSignature*>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:                    return SubobjectSize<D3D12_SHADER_BYTECODE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:         return SubobjectSize<D3D12_STREAM_OUTPUT_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:                 return SubobjectSize<D3D12_BLEND_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:           return SubobjectSize<UINT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:            return SubobjectSize<D3D12_RASTERIZER_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:         return SubobjectSize<D3D12_DEPTH_STENCIL_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:          return SubobjectSize<D3D12_INPUT_LAYOUT_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:    return SubobjectSize<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:    return SubobjectSize<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return SubobjectSize<D3D12_RT_FORMAT_ARRAY>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:  return SubobjectSize<DXGI_FORMAT>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:           return SubobjectSize<DXGI_SAMPLE_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:             return SubobjectSize<D3D12_NODE_MASK>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:            return SubobjectSize<D3D12_CACHED_PIPELINE_STATE>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:                 return SubobjectSize<D3D12_PIPELINE_STATE_FLAGS>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:        return SubobjectSize<D3D12_DEPTH_STENCIL_DESC1>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:       return SubobjectSize<D3D12_VIEW_INSTANCING_DESC>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:        return SubobjectSize<D3D12_DEPTH_STENCIL_DESC2>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:           return SubobjectSize<D3D12_RASTERIZER_DESC1>();
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:           return SubobjectSize<D3D12_RASTERIZER_DESC2>();
        // clang-format on
        default: return 0;
    }
}

// Collects the node masks of a stream. Returns false if the stream could not be parsed.
bool FindStreamNodeMasks(const D3D12_PIPELINE_STATE_STREAM_DESC& Desc, std::vector<UINT*>& NodeMasks)
{
    auto*        pBytes = static_cast<char*>(Desc.pPipelineStateSubobjectStream);
    const size_t Size   = Desc.SizeInBytes;
    for (size_t Offset = 0; Offset < Size;)
    {
        if (Size - Offset < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
            return false;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type;
        std::memcpy(&Type, pBytes + Offset, sizeof(Type));
        const size_t SubSize = GetSubobjectSize(Type);
        if (SubSize == 0 || SubSize > Size - Offset)
        {
            LogWarn("CreatePipelineState: unknown subobject type %d at offset %zu; stream passed through unchanged", static_cast<int>(Type), Offset);
            return false;
        }
        if (Type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK)
        {
            constexpr size_t PayloadOffset = (sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + alignof(D3D12_NODE_MASK) - 1) / alignof(D3D12_NODE_MASK) * alignof(D3D12_NODE_MASK);
            NodeMasks.push_back(reinterpret_cast<UINT*>(pBytes + Offset + PayloadOffset));
        }
        Offset += SubSize;
    }
    return true;
}

// Temporarily replaces masks in caller memory (const descriptors that other
// descriptors may point into, so they cannot be copied) and restores them
class MaskPatch
{
public:
    void Set(UINT* pMask, UINT Value)
    {
        m_Saved.push_back({pMask, *pMask});
        *pMask = Value;
    }
    ~MaskPatch()
    {
        for (auto It = m_Saved.rbegin(); It != m_Saved.rend(); ++It)
            *It->first = It->second;
    }

private:
    std::vector<std::pair<UINT*, UINT>> m_Saved;
};

// ---------------------------------------------------------------------------
// Hooks. Struct-returning methods use the Windows COM ABI: the caller passes a
// pointer to the result after This, and the method returns that pointer.
// ---------------------------------------------------------------------------

UINT STDMETHODCALLTYPE Hook_GetNodeCount(ID3D12Device*)
{
    return SimNodeCount();
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue(ID3D12Device* This, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppQueue)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateCommandQueue)(This, pDesc, riid, ppQueue);
    if (!CheckSingleNodeMask("CreateCommandQueue", "NodeMask", pDesc->NodeMask))
        return E_INVALIDARG;
    D3D12_COMMAND_QUEUE_DESC Local = *pDesc;
    Local.NodeMask                 = ToPhysicalMask(pDesc->NodeMask);
    const HRESULT hr               = Orig<PFN>(This, kSlot_CreateCommandQueue)(This, &Local, riid, ppQueue);
    OnQueueCreated(hr, ppQueue, pDesc->NodeMask);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandQueue1(ID3D12Device* This, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID CreatorID, REFIID riid, void** ppQueue)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateCommandQueue1)(This, pDesc, CreatorID, riid, ppQueue);
    if (!CheckSingleNodeMask("CreateCommandQueue1", "NodeMask", pDesc->NodeMask))
        return E_INVALIDARG;
    D3D12_COMMAND_QUEUE_DESC Local = *pDesc;
    Local.NodeMask                 = ToPhysicalMask(pDesc->NodeMask);
    const HRESULT hr               = Orig<PFN>(This, kSlot_CreateCommandQueue1)(This, &Local, CreatorID, riid, ppQueue);
    OnQueueCreated(hr, ppQueue, pDesc->NodeMask);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(ID3D12Device* This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPSO)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateGraphicsPipelineState)(This, pDesc, riid, ppPSO);
    if (!CheckNodeSetMask("CreateGraphicsPipelineState", "NodeMask", pDesc->NodeMask))
        return E_INVALIDARG;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC Local = *pDesc;
    Local.NodeMask                           = ToPhysicalMask(pDesc->NodeMask);
    return Orig<PFN>(This, kSlot_CreateGraphicsPipelineState)(This, &Local, riid, ppPSO);
}

HRESULT STDMETHODCALLTYPE Hook_CreateComputePipelineState(ID3D12Device* This, const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPSO)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateComputePipelineState)(This, pDesc, riid, ppPSO);
    if (!CheckNodeSetMask("CreateComputePipelineState", "NodeMask", pDesc->NodeMask))
        return E_INVALIDARG;
    D3D12_COMPUTE_PIPELINE_STATE_DESC Local = *pDesc;
    Local.NodeMask                          = ToPhysicalMask(pDesc->NodeMask);
    return Orig<PFN>(This, kSlot_CreateComputePipelineState)(This, &Local, riid, ppPSO);
}

HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(ID3D12Device* This, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPSO)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
    std::vector<UINT*> NodeMasks;
    MaskPatch          Patch;
    if (pDesc != nullptr && pDesc->pPipelineStateSubobjectStream != nullptr && FindStreamNodeMasks(*pDesc, NodeMasks))
    {
        for (UINT* pMask : NodeMasks)
        {
            if (!CheckNodeSetMask("CreatePipelineState", "NODE_MASK subobject", *pMask))
                return E_INVALIDARG;
            Patch.Set(pMask, ToPhysicalMask(*pMask));
        }
    }
    return Orig<PFN>(This, kSlot_CreatePipelineState)(This, pDesc, riid, ppPSO);
}

HRESULT STDMETHODCALLTYPE Hook_CreateStateObject(ID3D12Device* This, const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_STATE_OBJECT_DESC*, REFIID, void**);
    MaskPatch Patch;
    if (pDesc != nullptr && pDesc->pSubobjects != nullptr)
    {
        for (UINT i = 0; i < pDesc->NumSubobjects; ++i)
        {
            const D3D12_STATE_SUBOBJECT& Sub = pDesc->pSubobjects[i];
            if (Sub.Type != D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK || Sub.pDesc == nullptr)
                continue;
            UINT* pMask = &static_cast<D3D12_NODE_MASK*>(const_cast<void*>(Sub.pDesc))->NodeMask;
            if (!CheckNodeSetMask("CreateStateObject", "NODE_MASK subobject", *pMask))
                return E_INVALIDARG;
            Patch.Set(pMask, ToPhysicalMask(*pMask));
        }
    }
    return Orig<PFN>(This, kSlot_CreateStateObject)(This, pDesc, riid, ppStateObject);
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(ID3D12Device* This, UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12CommandAllocator* pAllocator,
                                                 ID3D12PipelineState* pInitialState, REFIID riid, void** ppList)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    if (!CheckSingleNodeMask("CreateCommandList", "nodeMask", NodeMask))
        return E_INVALIDARG;
    const HRESULT hr = Orig<PFN>(This, kSlot_CreateCommandList)(This, ToPhysicalMask(NodeMask), Type, pAllocator, pInitialState, riid, ppList);
    OnCommandListCreated(hr, ppList, Type, NodeMask);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList1(ID3D12Device* This, UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, D3D12_COMMAND_LIST_FLAGS Flags, REFIID riid, void** ppList)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
    if (!CheckSingleNodeMask("CreateCommandList1", "nodeMask", NodeMask))
        return E_INVALIDARG;
    const HRESULT hr = Orig<PFN>(This, kSlot_CreateCommandList1)(This, ToPhysicalMask(NodeMask), Type, Flags, riid, ppList);
    OnCommandListCreated(hr, ppList, Type, NodeMask);
    return hr;
}

// DILIGENT_SIM_CROSS_NODE_TIER 0..3 -> NOT_SUPPORTED, TIER_1, TIER_2, TIER_3
D3D12_CROSS_NODE_SHARING_TIER SimCrossNodeSharingTier()
{
    switch (GetConfig().CrossNodeSharingTier)
    {
        case 0: return D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
        case 2: return D3D12_CROSS_NODE_SHARING_TIER_2;
        case 3: return D3D12_CROSS_NODE_SHARING_TIER_3;
        default: return D3D12_CROSS_NODE_SHARING_TIER_1;
    }
}

// Feature queries that name a node (NodeIndex is the first member of each)
// go to node 0; the cross-node sharing tier is the simulated one
HRESULT STDMETHODCALLTYPE Hook_CheckFeatureSupport(ID3D12Device* This, D3D12_FEATURE Feature, void* pData, UINT DataSize)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_FEATURE, void*, UINT);
    const PFN pfn = Orig<PFN>(This, kSlot_CheckFeatureSupport);
    if (pData == nullptr)
        return pfn(This, Feature, pData, DataSize);

    switch (Feature)
    {
        case D3D12_FEATURE_ARCHITECTURE:
        case D3D12_FEATURE_ARCHITECTURE1:
        case D3D12_FEATURE_SERIALIZATION:
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT:
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT:
        case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES:
        {
            if (DataSize < sizeof(UINT))
                break;
            UINT* pNodeIndex = static_cast<UINT*>(pData);
            if (*pNodeIndex >= SimNodeCount())
                return E_INVALIDARG; // what the runtime returns for a node the adapter does not have
            const UINT    NodeIndex = *pNodeIndex;
            *pNodeIndex             = 0;
            const HRESULT hr        = pfn(This, Feature, pData, DataSize);
            *pNodeIndex             = NodeIndex;
            return hr;
        }

        case D3D12_FEATURE_D3D12_OPTIONS:
        {
            const HRESULT hr = pfn(This, Feature, pData, DataSize);
            if (SUCCEEDED(hr) && SimNodeCount() > 1 && DataSize >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
                static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(pData)->CrossNodeSharingTier = SimCrossNodeSharingTier();
            return hr;
        }

        case D3D12_FEATURE_CROSS_NODE:
        {
            const HRESULT hr = pfn(This, Feature, pData, DataSize);
            if (SUCCEEDED(hr) && SimNodeCount() > 1 && DataSize >= sizeof(D3D12_FEATURE_DATA_CROSS_NODE))
                static_cast<D3D12_FEATURE_DATA_CROSS_NODE*>(pData)->SharingTier = SimCrossNodeSharingTier();
            return hr;
        }

        default:
            break;
    }
    return pfn(This, Feature, pData, DataSize);
}

HRESULT STDMETHODCALLTYPE Hook_CreateDescriptorHeap(ID3D12Device* This, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc, REFIID riid, void** ppHeap)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateDescriptorHeap)(This, pDesc, riid, ppHeap);
    if (!CheckSingleNodeMask("CreateDescriptorHeap", "NodeMask", pDesc->NodeMask))
        return E_INVALIDARG;
    D3D12_DESCRIPTOR_HEAP_DESC Local = *pDesc;
    Local.NodeMask                   = ToPhysicalMask(pDesc->NodeMask);
    return Orig<PFN>(This, kSlot_CreateDescriptorHeap)(This, &Local, riid, ppHeap);
}

HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(ID3D12Device* This, UINT NodeMask, const void* pBlob, SIZE_T BlobSize, REFIID riid, void** ppRootSignature)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
    if (!CheckNodeSetMask("CreateRootSignature", "nodeMask", NodeMask))
        return E_INVALIDARG;
    return Orig<PFN>(This, kSlot_CreateRootSignature)(This, ToPhysicalMask(NodeMask), pBlob, BlobSize, riid, ppRootSignature);
}

D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE Hook_GetResourceAllocationInfo(ID3D12Device* This, D3D12_RESOURCE_ALLOCATION_INFO* pRetVal, UINT VisibleMask,
                                                                                 UINT NumDescs, const D3D12_RESOURCE_DESC* pDescs)
{
    using PFN = D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC*);
    CheckNodeSetMask("GetResourceAllocationInfo", "visibleMask", VisibleMask); // no error code to return: reported only
    return Orig<PFN>(This, kSlot_GetResourceAllocationInfo)(This, pRetVal, ToPhysicalMask(VisibleMask), NumDescs, pDescs);
}

D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE Hook_GetResourceAllocationInfo1(ID3D12Device* This, D3D12_RESOURCE_ALLOCATION_INFO* pRetVal, UINT VisibleMask,
                                                                                  UINT NumDescs, const D3D12_RESOURCE_DESC* pDescs, D3D12_RESOURCE_ALLOCATION_INFO1* pInfo1)
{
    using PFN = D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_ALLOCATION_INFO1*);
    CheckNodeSetMask("GetResourceAllocationInfo1", "visibleMask", VisibleMask);
    return Orig<PFN>(This, kSlot_GetResourceAllocationInfo1)(This, pRetVal, ToPhysicalMask(VisibleMask), NumDescs, pDescs, pInfo1);
}

D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE Hook_GetResourceAllocationInfo2(ID3D12Device* This, D3D12_RESOURCE_ALLOCATION_INFO* pRetVal, UINT VisibleMask,
                                                                                  UINT NumDescs, const D3D12_RESOURCE_DESC1* pDescs, D3D12_RESOURCE_ALLOCATION_INFO1* pInfo1)
{
    using PFN = D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_ALLOCATION_INFO1*);
    CheckNodeSetMask("GetResourceAllocationInfo2", "visibleMask", VisibleMask);
    return Orig<PFN>(This, kSlot_GetResourceAllocationInfo2)(This, pRetVal, ToPhysicalMask(VisibleMask), NumDescs, pDescs, pInfo1);
}

D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE Hook_GetCustomHeapProperties(ID3D12Device* This, D3D12_HEAP_PROPERTIES* pRetVal, UINT NodeMask, D3D12_HEAP_TYPE HeapType)
{
    using PFN = D3D12_HEAP_PROPERTIES*(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_HEAP_PROPERTIES*, UINT, D3D12_HEAP_TYPE);
    CheckSingleNodeMask("GetCustomHeapProperties", "nodeMask", NodeMask);
    D3D12_HEAP_PROPERTIES* pResult = Orig<PFN>(This, kSlot_GetCustomHeapProperties)(This, pRetVal, ToPhysicalMask(NodeMask), HeapType);
    // The properties name the requested node, so that passing them back creates the resource there
    if (pResult != nullptr && NodeMask != 0)
    {
        pResult->CreationNodeMask = NodeMask;
        pResult->VisibleNodeMask  = NodeMask;
    }
    return pResult;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pProps, D3D12_HEAP_FLAGS Flags, const D3D12_RESOURCE_DESC* pDesc,
                                                       D3D12_RESOURCE_STATES State, const D3D12_CLEAR_VALUE* pClear, REFIID riid, void** ppResource)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                            const D3D12_CLEAR_VALUE*, REFIID, void**);
    if (pProps == nullptr)
        return Orig<PFN>(This, kSlot_CreateCommittedResource)(This, pProps, Flags, pDesc, State, pClear, riid, ppResource);
    if (!CheckHeapNodeMasks("CreateCommittedResource", pProps->CreationNodeMask, pProps->VisibleNodeMask))
        return E_INVALIDARG;
    const D3D12_HEAP_PROPERTIES Local = PhysicalHeapProperties(*pProps);
    const HRESULT               hr    = Orig<PFN>(This, kSlot_CreateCommittedResource)(This, &Local, Flags, pDesc, State, pClear, riid, ppResource);
    OnResourceCreated(hr, ppResource, *pProps);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource1(ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pProps, D3D12_HEAP_FLAGS Flags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES State, const D3D12_CLEAR_VALUE* pClear, ID3D12ProtectedResourceSession* pSession,
                                                        REFIID riid, void** ppResource)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                            const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
    if (pProps == nullptr)
        return Orig<PFN>(This, kSlot_CreateCommittedResource1)(This, pProps, Flags, pDesc, State, pClear, pSession, riid, ppResource);
    if (!CheckHeapNodeMasks("CreateCommittedResource1", pProps->CreationNodeMask, pProps->VisibleNodeMask))
        return E_INVALIDARG;
    const D3D12_HEAP_PROPERTIES Local = PhysicalHeapProperties(*pProps);
    const HRESULT               hr    = Orig<PFN>(This, kSlot_CreateCommittedResource1)(This, &Local, Flags, pDesc, State, pClear, pSession, riid, ppResource);
    OnResourceCreated(hr, ppResource, *pProps);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource2(ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pProps, D3D12_HEAP_FLAGS Flags, const D3D12_RESOURCE_DESC1* pDesc,
                                                        D3D12_RESOURCE_STATES State, const D3D12_CLEAR_VALUE* pClear, ID3D12ProtectedResourceSession* pSession,
                                                        REFIID riid, void** ppResource)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                            const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
    if (pProps == nullptr)
        return Orig<PFN>(This, kSlot_CreateCommittedResource2)(This, pProps, Flags, pDesc, State, pClear, pSession, riid, ppResource);
    if (!CheckHeapNodeMasks("CreateCommittedResource2", pProps->CreationNodeMask, pProps->VisibleNodeMask))
        return E_INVALIDARG;
    const D3D12_HEAP_PROPERTIES Local = PhysicalHeapProperties(*pProps);
    const HRESULT               hr    = Orig<PFN>(This, kSlot_CreateCommittedResource2)(This, &Local, Flags, pDesc, State, pClear, pSession, riid, ppResource);
    OnResourceCreated(hr, ppResource, *pProps);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource3(ID3D12Device* This, const D3D12_HEAP_PROPERTIES* pProps, D3D12_HEAP_FLAGS Flags, const D3D12_RESOURCE_DESC1* pDesc,
                                                        D3D12_BARRIER_LAYOUT Layout, const D3D12_CLEAR_VALUE* pClear, ID3D12ProtectedResourceSession* pSession,
                                                        UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats, REFIID riid, void** ppResource)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT,
                                            const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
    if (pProps == nullptr)
        return Orig<PFN>(This, kSlot_CreateCommittedResource3)(This, pProps, Flags, pDesc, Layout, pClear, pSession, NumCastableFormats, pCastableFormats, riid, ppResource);
    if (!CheckHeapNodeMasks("CreateCommittedResource3", pProps->CreationNodeMask, pProps->VisibleNodeMask))
        return E_INVALIDARG;
    const D3D12_HEAP_PROPERTIES Local = PhysicalHeapProperties(*pProps);
    const HRESULT               hr    = Orig<PFN>(This, kSlot_CreateCommittedResource3)(This, &Local, Flags, pDesc, Layout, pClear, pSession, NumCastableFormats, pCastableFormats, riid, ppResource);
    OnResourceCreated(hr, ppResource, *pProps);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateHeap(ID3D12Device* This, const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppHeap)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateHeap)(This, pDesc, riid, ppHeap);
    if (!CheckHeapNodeMasks("CreateHeap", pDesc->Properties.CreationNodeMask, pDesc->Properties.VisibleNodeMask))
        return E_INVALIDARG;
    D3D12_HEAP_DESC Local = *pDesc;
    Local.Properties      = PhysicalHeapProperties(pDesc->Properties);
    const HRESULT hr      = Orig<PFN>(This, kSlot_CreateHeap)(This, &Local, riid, ppHeap);
    OnResourceCreated(hr, ppHeap, pDesc->Properties);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateHeap1(ID3D12Device* This, const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pSession, REFIID riid, void** ppHeap)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateHeap1)(This, pDesc, pSession, riid, ppHeap);
    if (!CheckHeapNodeMasks("CreateHeap1", pDesc->Properties.CreationNodeMask, pDesc->Properties.VisibleNodeMask))
        return E_INVALIDARG;
    D3D12_HEAP_DESC Local = *pDesc;
    Local.Properties      = PhysicalHeapProperties(pDesc->Properties);
    const HRESULT hr      = Orig<PFN>(This, kSlot_CreateHeap1)(This, &Local, pSession, riid, ppHeap);
    OnResourceCreated(hr, ppHeap, pDesc->Properties);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(ID3D12Device* This, ID3D12Heap* pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES State,
                                                    const D3D12_CLEAR_VALUE* pClear, REFIID riid, void** ppResource)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    const HRESULT hr = Orig<PFN>(This, kSlot_CreatePlacedResource)(This, pHeap, Offset, pDesc, State, pClear, riid, ppResource);
    OnPlacedResourceCreated(hr, ppResource, pHeap);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource1(ID3D12Device* This, ID3D12Heap* pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC1* pDesc, D3D12_RESOURCE_STATES State,
                                                     const D3D12_CLEAR_VALUE* pClear, REFIID riid, void** ppResource)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
    const HRESULT hr = Orig<PFN>(This, kSlot_CreatePlacedResource1)(This, pHeap, Offset, pDesc, State, pClear, riid, ppResource);
    OnPlacedResourceCreated(hr, ppResource, pHeap);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource2(ID3D12Device* This, ID3D12Heap* pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC1* pDesc, D3D12_BARRIER_LAYOUT Layout,
                                                     const D3D12_CLEAR_VALUE* pClear, UINT32 NumCastableFormats, const DXGI_FORMAT* pCastableFormats, REFIID riid, void** ppResource)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32,
                                            const DXGI_FORMAT*, REFIID, void**);
    const HRESULT hr = Orig<PFN>(This, kSlot_CreatePlacedResource2)(This, pHeap, Offset, pDesc, Layout, pClear, NumCastableFormats, pCastableFormats, riid, ppResource);
    OnPlacedResourceCreated(hr, ppResource, pHeap);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateQueryHeap(ID3D12Device* This, const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppHeap)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateQueryHeap)(This, pDesc, riid, ppHeap);
    if (!CheckSingleNodeMask("CreateQueryHeap", "NodeMask", pDesc->NodeMask))
        return E_INVALIDARG;
    D3D12_QUERY_HEAP_DESC Local = *pDesc;
    Local.NodeMask              = ToPhysicalMask(pDesc->NodeMask);
    return Orig<PFN>(This, kSlot_CreateQueryHeap)(This, &Local, riid, ppHeap);
}

HRESULT STDMETHODCALLTYPE Hook_CreateCommandSignature(ID3D12Device* This, const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature, REFIID riid,
                                                      void** ppSignature)
{
    using PFN = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**);
    if (pDesc == nullptr)
        return Orig<PFN>(This, kSlot_CreateCommandSignature)(This, pDesc, pRootSignature, riid, ppSignature);
    if (!CheckNodeSetMask("CreateCommandSignature", "NodeMask", pDesc->NodeMask))
        return E_INVALIDARG;
    D3D12_COMMAND_SIGNATURE_DESC Local = *pDesc;
    Local.NodeMask                     = ToPhysicalMask(pDesc->NodeMask);
    return Orig<PFN>(This, kSlot_CreateCommandSignature)(This, &Local, pRootSignature, riid, ppSignature);
}

void PatchDeviceVtable(void** Slots, size_t NumSlots)
{
    auto Set = [&](int Slot, auto* pHook) {
        if (static_cast<size_t>(Slot) < NumSlots)
            Slots[Slot] = reinterpret_cast<void*>(pHook);
    };
    Set(kSlot_GetNodeCount, &Hook_GetNodeCount);
    Set(kSlot_CreateCommandQueue, &Hook_CreateCommandQueue);
    Set(kSlot_CreateGraphicsPipelineState, &Hook_CreateGraphicsPipelineState);
    Set(kSlot_CreateComputePipelineState, &Hook_CreateComputePipelineState);
    Set(kSlot_CreateCommandList, &Hook_CreateCommandList);
    Set(kSlot_CheckFeatureSupport, &Hook_CheckFeatureSupport);
    Set(kSlot_CreateDescriptorHeap, &Hook_CreateDescriptorHeap);
    Set(kSlot_CreateRootSignature, &Hook_CreateRootSignature);
    Set(kSlot_GetResourceAllocationInfo, &Hook_GetResourceAllocationInfo);
    Set(kSlot_GetCustomHeapProperties, &Hook_GetCustomHeapProperties);
    Set(kSlot_CreateCommittedResource, &Hook_CreateCommittedResource);
    Set(kSlot_CreateHeap, &Hook_CreateHeap);
    Set(kSlot_CreatePlacedResource, &Hook_CreatePlacedResource);
    Set(kSlot_CreateQueryHeap, &Hook_CreateQueryHeap);
    Set(kSlot_CreateCommandSignature, &Hook_CreateCommandSignature);
    // Methods of derived interfaces: the entries exist only if the object
    // implements the interface; patching garbage entries of a shorter vtable is
    // harmless, since nothing can call them without that interface
    Set(kSlot_CreatePipelineState, &Hook_CreatePipelineState);
    Set(kSlot_CreateCommandList1, &Hook_CreateCommandList1);
    Set(kSlot_CreateCommittedResource1, &Hook_CreateCommittedResource1);
    Set(kSlot_CreateHeap1, &Hook_CreateHeap1);
    Set(kSlot_GetResourceAllocationInfo1, &Hook_GetResourceAllocationInfo1);
    Set(kSlot_CreateStateObject, &Hook_CreateStateObject);
    Set(kSlot_GetResourceAllocationInfo2, &Hook_GetResourceAllocationInfo2);
    Set(kSlot_CreateCommittedResource2, &Hook_CreateCommittedResource2);
    Set(kSlot_CreatePlacedResource1, &Hook_CreatePlacedResource1);
    Set(kSlot_CreateCommandQueue1, &Hook_CreateCommandQueue1);
    Set(kSlot_CreateCommittedResource3, &Hook_CreateCommittedResource3);
    Set(kSlot_CreatePlacedResource2, &Hook_CreatePlacedResource2);
}

} // namespace


bool WrapDevice(ID3D12Device* pDevice)
{
    if (pDevice == nullptr)
        return false;
    const bool WasWrapped = g_Devices.IsWrapped(pDevice);
    if (!g_Devices.Wrap(pDevice))
    {
        LogError("WrapDevice: device %p could not be wrapped", pDevice);
        return false;
    }
    if (!WasWrapped)
        LogInfo("D3D12 device %p wrapped: %u simulated linked nodes", pDevice, SimNodeCount());
    return true;
}

} // namespace D3D12Sim
