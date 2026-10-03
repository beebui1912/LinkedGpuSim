/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "D3D12Tracking.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>

#include <dxgi.h>

#include "ShimLog.hpp"

namespace D3D12Sim
{

namespace
{

// {7A1C2E40-3B5D-4C6E-8F70-91A2B3C4D501..05}: lifetime tokens per tracker kind
constexpr GUID kResourceLifetime = {0x7a1c2e40, 0x3b5d, 0x4c6e, {0x8f, 0x70, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0x01}};
constexpr GUID kHeapLifetime     = {0x7a1c2e40, 0x3b5d, 0x4c6e, {0x8f, 0x70, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0x02}};
constexpr GUID kRootSigLifetime  = {0x7a1c2e40, 0x3b5d, 0x4c6e, {0x8f, 0x70, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0x03}};

constexpr UINT kMaxNodes = 32;

std::mutex g_Mutex;

std::unordered_map<const void*, ResourceInfo> g_Resources;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, const void*> g_BuffersByVA; // start address -> buffer
UINT64                                        g_NodeBytes[kMaxNodes][2] = {}; // [node][local]

struct HeapEntry
{
    DescriptorHeapInfo Info;
    const void*        pHeap = nullptr;
};
std::map<SIZE_T, HeapEntry> g_DescHeapsByCpu; // CPU start -> heap
std::map<UINT64, SIZE_T>    g_DescHeapsByGpu; // GPU start -> CPU start

std::unordered_map<SIZE_T, DescriptorInfo> g_Descriptors;

std::unordered_map<const void*, std::shared_ptr<const RootLayout>> g_RootSignatures;

UINT NodeIndexOf(UINT Mask)
{
    UINT Index = 0;
    while (Index + 1 < kMaxNodes && (Mask & (1u << Index)) == 0)
        ++Index;
    return Index;
}

// Releases its callback when the object holding it in its private data is destroyed
class LifetimeToken final : public IUnknown
{
public:
    LifetimeToken(const void* pObject, void (*OnDestroy)(const void*)) :
        m_pObject{pObject}, m_OnDestroy{OnDestroy} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown))
        {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_RefCount; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG Count = --m_RefCount;
        if (Count == 0)
        {
            m_OnDestroy(m_pObject);
            delete this;
        }
        return Count;
    }

private:
    std::atomic<ULONG> m_RefCount{1};
    const void*        m_pObject;
    void (*m_OnDestroy)(const void*);
};

void OnResourceDestroyed(const void* pObject)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_Resources.find(pObject);
    if (It == g_Resources.end())
        return;
    const ResourceInfo& Info = It->second;
    g_NodeBytes[NodeIndexOf(Info.Creation)][Info.Local ? 1 : 0] -= Info.Bytes;
    if (Info.VA != 0)
    {
        auto VAIt = g_BuffersByVA.find(Info.VA);
        if (VAIt != g_BuffersByVA.end() && VAIt->second == pObject)
            g_BuffersByVA.erase(VAIt);
    }
    g_Resources.erase(It);
}

void OnDescriptorHeapDestroyed(const void* pHeap)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    for (auto It = g_DescHeapsByCpu.begin(); It != g_DescHeapsByCpu.end(); ++It)
    {
        if (It->second.pHeap != pHeap)
            continue;
        const DescriptorHeapInfo& Info = It->second.Info;
        for (UINT i = 0; i < Info.Count; ++i)
            g_Descriptors.erase(Info.CpuStart + SIZE_T{i} * Info.Increment);
        if (Info.GpuStart != 0)
            g_DescHeapsByGpu.erase(Info.GpuStart);
        g_DescHeapsByCpu.erase(It);
        return;
    }
}

void OnRootSignatureDestroyed(const void* pRootSignature)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    g_RootSignatures.erase(pRootSignature);
}

} // namespace

bool OnObjectDestroyed(IUnknown* pObject, const GUID& Kind, void (*OnDestroy)(const void*))
{
    if (pObject == nullptr)
        return false;
    ID3D12Object* pD3D12 = nullptr;
    IDXGIObject*  pDXGI  = nullptr;
    if (FAILED(pObject->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&pD3D12))) &&
        FAILED(pObject->QueryInterface(__uuidof(IDXGIObject), reinterpret_cast<void**>(&pDXGI))))
        return false;

    UINT Size     = 0;
    bool Existing = pD3D12 != nullptr ? SUCCEEDED(pD3D12->GetPrivateData(Kind, &Size, nullptr)) : SUCCEEDED(pDXGI->GetPrivateData(Kind, &Size, nullptr));
    bool Ok       = true;
    if (!Existing || Size == 0)
    {
        auto* pToken = new LifetimeToken{pObject, OnDestroy};
        Ok           = pD3D12 != nullptr ? SUCCEEDED(pD3D12->SetPrivateDataInterface(Kind, pToken)) : SUCCEEDED(pDXGI->SetPrivateDataInterface(Kind, pToken));
        pToken->Release(); // the object holds the only reference now
    }
    if (pD3D12 != nullptr)
        pD3D12->Release();
    if (pDXGI != nullptr)
        pDXGI->Release();
    return Ok;
}

// ---- Resources ------------------------------------------------------------------

void RegisterResource(IUnknown* pObject, const ResourceInfo& Info)
{
    if (pObject == nullptr)
        return;
    {
        std::lock_guard<std::mutex> Lock{g_Mutex};
        auto                        It = g_Resources.find(pObject);
        if (It != g_Resources.end())
            g_NodeBytes[NodeIndexOf(It->second.Creation)][It->second.Local ? 1 : 0] -= It->second.Bytes;
        g_Resources[pObject] = Info;
        g_NodeBytes[NodeIndexOf(Info.Creation)][Info.Local ? 1 : 0] += Info.Bytes;
        if (Info.VA != 0)
            g_BuffersByVA[Info.VA] = pObject;
    }
    OnObjectDestroyed(pObject, kResourceLifetime, &OnResourceDestroyed);
}

bool FindResource(const void* pObject, ResourceInfo& Info)
{
    if (pObject == nullptr)
        return false;
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_Resources.find(pObject);
    if (It == g_Resources.end())
        return false;
    Info = It->second;
    return true;
}

const void* FindResourceByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, ResourceInfo& Info)
{
    if (VA == 0)
        return nullptr;
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_BuffersByVA.upper_bound(VA);
    if (It == g_BuffersByVA.begin())
        return nullptr;
    --It;
    auto ResIt = g_Resources.find(It->second);
    if (ResIt == g_Resources.end() || VA >= ResIt->second.VA + ResIt->second.Size)
        return nullptr;
    Info = ResIt->second;
    return It->second;
}

UINT64 GetTrackedNodeBytes(UINT NodeIndex, bool Local)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    return NodeIndex < kMaxNodes ? g_NodeBytes[NodeIndex][Local ? 1 : 0] : 0;
}

UINT64 GetTrackedTotalBytes(bool Local)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    UINT64                      Total = 0;
    for (UINT n = 0; n < kMaxNodes; ++n)
        Total += g_NodeBytes[n][Local ? 1 : 0];
    return Total;
}

// ---- Descriptors ----------------------------------------------------------------

void RegisterDescriptorHeap(IUnknown* pHeap, const DescriptorHeapInfo& Info)
{
    if (pHeap == nullptr || Info.Count == 0)
        return;
    {
        std::lock_guard<std::mutex> Lock{g_Mutex};
        g_DescHeapsByCpu[Info.CpuStart] = HeapEntry{Info, pHeap};
        if (Info.GpuStart != 0)
            g_DescHeapsByGpu[Info.GpuStart] = Info.CpuStart;
    }
    OnObjectDestroyed(pHeap, kHeapLifetime, &OnDescriptorHeapDestroyed);
}

bool FindDescriptorHeapByCpu(SIZE_T Cpu, DescriptorHeapInfo& Info)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_DescHeapsByCpu.upper_bound(Cpu);
    if (It == g_DescHeapsByCpu.begin())
        return false;
    --It;
    const DescriptorHeapInfo& H = It->second.Info;
    if (Cpu >= H.CpuStart + SIZE_T{H.Count} * H.Increment)
        return false;
    Info = H;
    return true;
}

bool FindDescriptorHeapByGpu(UINT64 Gpu, DescriptorHeapInfo& Info, SIZE_T& Cpu)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_DescHeapsByGpu.upper_bound(Gpu);
    if (It == g_DescHeapsByGpu.begin())
        return false;
    --It;
    auto HeapIt = g_DescHeapsByCpu.find(It->second);
    if (HeapIt == g_DescHeapsByCpu.end())
        return false;
    const DescriptorHeapInfo& H = HeapIt->second.Info;
    if (Gpu >= H.GpuStart + UINT64{H.Count} * H.Increment)
        return false;
    Info = H;
    Cpu  = H.CpuStart + static_cast<SIZE_T>(Gpu - H.GpuStart);
    return true;
}

void SetDescriptor(SIZE_T Cpu, const DescriptorInfo& Info)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    g_Descriptors[Cpu] = Info;
}

bool GetDescriptor(SIZE_T Cpu, DescriptorInfo& Info)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_Descriptors.find(Cpu);
    if (It == g_Descriptors.end())
        return false;
    Info = It->second;
    return true;
}

void CopyDescriptors(SIZE_T Dst, SIZE_T Src, UINT Count, UINT Increment)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    for (UINT i = 0; i < Count; ++i)
    {
        const SIZE_T Offset = SIZE_T{i} * Increment;
        auto         It     = g_Descriptors.find(Src + Offset);
        if (It != g_Descriptors.end())
            g_Descriptors[Dst + Offset] = It->second;
        else
            g_Descriptors.erase(Dst + Offset);
    }
}

// ---- Root signatures ------------------------------------------------------------

void RegisterRootSignature(IUnknown* pRootSignature, const void* pBlob, SIZE_T BlobSize)
{
    if (pRootSignature == nullptr || pBlob == nullptr)
        return;
    ID3D12VersionedRootSignatureDeserializer* pDeserializer = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(pBlob, BlobSize, IID_PPV_ARGS(&pDeserializer))))
        return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pDesc = nullptr;
    if (FAILED(pDeserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &pDesc)) || pDesc == nullptr)
    {
        pDeserializer->Release();
        return;
    }
    auto Layout = std::make_shared<RootLayout>();
    for (UINT p = 0; p < pDesc->Desc_1_1.NumParameters; ++p)
    {
        const D3D12_ROOT_PARAMETER1& Src = pDesc->Desc_1_1.pParameters[p];
        RootParameter                Param;
        Param.Type = Src.ParameterType;
        if (Src.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            UINT Next = 0;
            for (UINT r = 0; r < Src.DescriptorTable.NumDescriptorRanges; ++r)
            {
                const D3D12_DESCRIPTOR_RANGE1& R = Src.DescriptorTable.pDescriptorRanges[r];
                RootRange                      Range;
                Range.Type   = R.RangeType;
                Range.Count  = R.NumDescriptors;
                Range.Offset = R.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND ? Next : R.OffsetInDescriptorsFromTableStart;
                Next         = (Range.Offset == UINT_MAX || Range.Count == UINT_MAX) ? UINT_MAX : Range.Offset + Range.Count;
                Param.Ranges.push_back(Range);
            }
        }
        Layout->push_back(std::move(Param));
    }
    pDeserializer->Release();
    {
        std::lock_guard<std::mutex> Lock{g_Mutex};
        g_RootSignatures[pRootSignature] = std::move(Layout);
    }
    OnObjectDestroyed(pRootSignature, kRootSigLifetime, &OnRootSignatureDestroyed);
}

std::shared_ptr<const RootLayout> FindRootSignature(const void* pRootSignature)
{
    std::lock_guard<std::mutex> Lock{g_Mutex};
    auto                        It = g_RootSignatures.find(pRootSignature);
    return It != g_RootSignatures.end() ? It->second : nullptr;
}

} // namespace D3D12Sim
