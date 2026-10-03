/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "NodeMasks.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "ShimConfig.hpp"
#include "ShimLog.hpp"

namespace D3D12Sim
{

namespace
{

std::atomic<unsigned> g_ValidationErrors{0};
constexpr unsigned    kMaxReportedErrors = 50;

// {5B8B6B1E-6C1F-4F34-9D3B-2C1E7A4C0D11}: UINT, simulated node mask of a queue or command list
constexpr GUID kNodeMaskGuid = {0x5b8b6b1e, 0x6c1f, 0x4f34, {0x9d, 0x3b, 0x2c, 0x1e, 0x7a, 0x4c, 0x0d, 0x11}};
// {5B8B6B1E-6C1F-4F34-9D3B-2C1E7A4C0D12}: UINT[2], simulated creation/visible masks of a heap or resource
constexpr GUID kHeapMasksGuid = {0x5b8b6b1e, 0x6c1f, 0x4f34, {0x9d, 0x3b, 0x2c, 0x1e, 0x7a, 0x4c, 0x0d, 0x12}};

bool IsPowerOfTwo(UINT V) { return V != 0 && (V & (V - 1)) == 0; }

bool SetData(IUnknown* pObject, const GUID& Guid, UINT Size, const void* pData)
{
    if (pObject == nullptr)
        return false;
    ID3D12Object* pD3D12Object = nullptr;
    if (FAILED(pObject->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&pD3D12Object))))
        return false;
    const HRESULT hr = pD3D12Object->SetPrivateData(Guid, Size, pData);
    pD3D12Object->Release();
    return SUCCEEDED(hr);
}

bool GetData(IUnknown* pObject, const GUID& Guid, UINT Size, void* pData)
{
    if (pObject == nullptr)
        return false;
    ID3D12Object* pD3D12Object = nullptr;
    if (FAILED(pObject->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&pD3D12Object))))
        return false;
    UINT          DataSize = Size;
    const HRESULT hr       = pD3D12Object->GetPrivateData(Guid, &DataSize, pData);
    pD3D12Object->Release();
    return SUCCEEDED(hr) && DataSize == Size;
}

} // namespace

UINT SimNodeCount() { return GetConfig().SimNodeCount; }
UINT AllNodesMask() { return (1u << SimNodeCount()) - 1u; }
bool ValidationEnabled() { return GetConfig().Validate; }

void ReportValidationError(const char* Fmt, ...)
{
    const unsigned Index = g_ValidationErrors.fetch_add(1) + 1;
    if (Index > kMaxReportedErrors)
        return;
    char    Buf[1024];
    va_list ap;
    va_start(ap, Fmt);
    std::vsnprintf(Buf, sizeof(Buf), Fmt, ap);
    va_end(ap);
    LogError("VALIDATION ERROR: %s", Buf);
    std::fprintf(stderr, "[shim] VALIDATION ERROR: %s\n", Buf);
    if (Index == kMaxReportedErrors)
        LogError("VALIDATION ERROR: further messages are suppressed (still counted)");
}

unsigned GetValidationErrorCount() { return g_ValidationErrors.load(); }

bool CheckSingleNodeMask(const char* Api, const char* Field, UINT Mask)
{
    if (!ValidationEnabled() || Mask == 0 || (IsPowerOfTwo(Mask) && (Mask & ~AllNodesMask()) == 0))
        return true;
    ReportValidationError("%s: %s 0x%X must be 0 or name exactly one of the %u nodes", Api, Field, Mask, SimNodeCount());
    return false;
}

bool CheckNodeSetMask(const char* Api, const char* Field, UINT Mask)
{
    if (!ValidationEnabled() || (Mask & ~AllNodesMask()) == 0)
        return true;
    ReportValidationError("%s: %s 0x%X names nodes the device does not have (%u nodes)", Api, Field, Mask, SimNodeCount());
    return false;
}

bool CheckHeapNodeMasks(const char* Api, UINT CreationNodeMask, UINT VisibleNodeMask)
{
    if (!CheckSingleNodeMask(Api, "CreationNodeMask", CreationNodeMask) || !CheckNodeSetMask(Api, "VisibleNodeMask", VisibleNodeMask))
        return false;
    if (ValidationEnabled() && VisibleNodeMask != 0 && (VisibleNodeMask & NormalizeSingleNode(CreationNodeMask)) == 0)
    {
        ReportValidationError("%s: VisibleNodeMask 0x%X must include the creation node (CreationNodeMask 0x%X)", Api, VisibleNodeMask, CreationNodeMask);
        return false;
    }
    return true;
}

void SetObjectNodeMask(IUnknown* pObject, UINT NodeMask)
{
    const UINT Mask = NormalizeSingleNode(NodeMask);
    SetData(pObject, kNodeMaskGuid, sizeof(Mask), &Mask);
}

bool GetObjectNodeMask(IUnknown* pObject, UINT& NodeMask)
{
    return GetData(pObject, kNodeMaskGuid, sizeof(NodeMask), &NodeMask);
}

void SetResourceNodeMasks(IUnknown* pObject, UINT CreationNodeMask, UINT VisibleNodeMask)
{
    const UINT Creation   = NormalizeSingleNode(CreationNodeMask);
    const UINT Masks[2] = {Creation, VisibleNodeMask != 0 ? VisibleNodeMask : Creation};
    SetData(pObject, kHeapMasksGuid, sizeof(Masks), Masks);
}

bool GetResourceNodeMasks(IUnknown* pObject, UINT& CreationNodeMask, UINT& VisibleNodeMask)
{
    UINT Masks[2] = {};
    if (!GetData(pObject, kHeapMasksGuid, sizeof(Masks), Masks))
        return false;
    CreationNodeMask = Masks[0];
    VisibleNodeMask  = Masks[1];
    return true;
}

} // namespace D3D12Sim

extern "C" __declspec(dllexport) unsigned D3D12Sim_GetValidationErrorCount()
{
    return D3D12Sim::GetValidationErrorCount();
}

extern "C" __declspec(dllexport) unsigned D3D12Sim_GetSimulatedNodeCount()
{
    return D3D12Sim::SimNodeCount();
}
