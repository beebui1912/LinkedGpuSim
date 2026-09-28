/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "VirtualDXGIAdapter.hpp"

#include <cstdio>
#include <cwchar>

#include "ShimConfig.hpp"
#include "ShimLog.hpp"

namespace D3D12Sim
{

VirtualDXGIAdapter::VirtualDXGIAdapter(IDXGIAdapter1* pReal, unsigned NodeIndex, unsigned NodeCount)
    : m_NodeIndex{NodeIndex}
    , m_NodeCount{NodeCount == 0 ? 1u : NodeCount}
{
    m_pReal1 = pReal;
    if (pReal != nullptr)
    {
        pReal->QueryInterface(__uuidof(IDXGIAdapter2), reinterpret_cast<void**>(m_pReal2.GetAddressOf()));
        pReal->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(m_pReal3.GetAddressOf()));
    }
}

VirtualDXGIAdapter::~VirtualDXGIAdapter() = default;

IDXGIAdapter1* VirtualDXGIAdapter::Create(IDXGIAdapter1* pReal, unsigned NodeIndex, unsigned NodeCount)
{
    if (pReal == nullptr)
        return nullptr;
    auto* p = new VirtualDXGIAdapter(pReal, NodeIndex, NodeCount);
    return static_cast<IDXGIAdapter1*>(p);
}

UINT64 VirtualDXGIAdapter::SplitU64(UINT64 Value) const
{
    const UINT64 Per = Value / m_NodeCount;
    const UINT64 Rem = Value - Per * m_NodeCount;
    // Last node absorbs the remainder so the sum still equals the host figure.
    return Per + ((m_NodeIndex + 1u == m_NodeCount) ? Rem : 0u);
}

SIZE_T VirtualDXGIAdapter::SplitSize(SIZE_T Value) const
{
    return static_cast<SIZE_T>(SplitU64(static_cast<UINT64>(Value)));
}

namespace
{
// Appends " [Simulated Node k]" onto a wchar_t Description buffer of a fixed
// size (128 for DXGI_ADAPTER_DESC*).  Truncates gracefully if there's not
// enough room.
void AppendNodeSuffix(wchar_t* Description, size_t Capacity, unsigned NodeIndex)
{
    if (Description == nullptr || Capacity == 0)
        return;

    // Length of the current NUL-terminated string, clamped to Capacity.
    size_t CurrentLen = 0;
    while (CurrentLen < Capacity && Description[CurrentLen] != L'\0')
        ++CurrentLen;

    wchar_t Suffix[48];
    const int SuffixLenI = std::swprintf(Suffix, static_cast<size_t>(48),
                                         L" [Simulated Node %u]", NodeIndex);
    if (SuffixLenI <= 0)
        return;
    const size_t SuffixLen = static_cast<size_t>(SuffixLenI);

    // Trim the existing string if the suffix wouldn't otherwise fit.
    if (CurrentLen + SuffixLen + 1u > Capacity)
    {
        CurrentLen = (Capacity > SuffixLen + 1u) ? (Capacity - SuffixLen - 1u) : 0u;
        Description[CurrentLen] = L'\0';
    }

    // Manual append: copy Suffix into the buffer and null-terminate.
    for (size_t i = 0; i < SuffixLen && (CurrentLen + i) < Capacity - 1u; ++i)
        Description[CurrentLen + i] = Suffix[i];
    const size_t Written = (CurrentLen + SuffixLen < Capacity - 1u) ? SuffixLen : (Capacity - 1u - CurrentLen);
    Description[CurrentLen + Written] = L'\0';
}
} // namespace

namespace
{
// Synthetic LUID for a virtual node.  Real Windows adapter LUIDs are small
// integers (HighPart is almost always 0, LowPart is a few thousand), so the
// 0xFEEDFACE constant guarantees no collision with anything PDH will report.
//
// Why fake it?  Diligent's GpuInfoPanel matches PDH "GPU Engine" counters
// against adapter LUIDs and stops at the FIRST hit.  If our virtual nodes
// preserved the primary's LUID, all utilization would be attributed to
// virtual node 0 and the real primary would read 0%.  A distinct LUID per
// virtual node makes PDH matching skip us, so the utilization lands on the
// real primary (which is where the work actually runs).
inline LUID MakeVirtualLuid(unsigned NodeIndex)
{
    LUID L{};
    L.HighPart = static_cast<LONG>(0xFEEDFACE);
    L.LowPart  = static_cast<LONG>(0xC0DE0000u | (NodeIndex & 0xFFFFu));
    return L;
}
} // namespace

void VirtualDXGIAdapter::ModifyDesc(DXGI_ADAPTER_DESC1* pDesc) const
{
    if (pDesc == nullptr) return;
    AppendNodeSuffix(pDesc->Description, static_cast<size_t>(128), m_NodeIndex);
    pDesc->DedicatedVideoMemory  = SplitSize(pDesc->DedicatedVideoMemory);
    pDesc->DedicatedSystemMemory = SplitSize(pDesc->DedicatedSystemMemory);
    pDesc->SharedSystemMemory    = SplitSize(pDesc->SharedSystemMemory);
    pDesc->AdapterLuid           = MakeVirtualLuid(m_NodeIndex);
}

void VirtualDXGIAdapter::ModifyDesc(DXGI_ADAPTER_DESC* pDesc) const
{
    if (pDesc == nullptr) return;
    AppendNodeSuffix(pDesc->Description, static_cast<size_t>(128), m_NodeIndex);
    pDesc->DedicatedVideoMemory  = SplitSize(pDesc->DedicatedVideoMemory);
    pDesc->DedicatedSystemMemory = SplitSize(pDesc->DedicatedSystemMemory);
    pDesc->SharedSystemMemory    = SplitSize(pDesc->SharedSystemMemory);
    pDesc->AdapterLuid           = MakeVirtualLuid(m_NodeIndex);
}

void VirtualDXGIAdapter::ModifyDesc(DXGI_ADAPTER_DESC2* pDesc) const
{
    if (pDesc == nullptr) return;
    AppendNodeSuffix(pDesc->Description, static_cast<size_t>(128), m_NodeIndex);
    pDesc->DedicatedVideoMemory  = SplitSize(pDesc->DedicatedVideoMemory);
    pDesc->DedicatedSystemMemory = SplitSize(pDesc->DedicatedSystemMemory);
    pDesc->SharedSystemMemory    = SplitSize(pDesc->SharedSystemMemory);
    pDesc->AdapterLuid           = MakeVirtualLuid(m_NodeIndex);
}

// -----------------------------------------------------------------------
// IUnknown
// -----------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::QueryInterface(REFIID riid, void** ppv)
{
    if (ppv == nullptr) return E_POINTER;

    // Private hand-off IID: the shim's D3D12CreateDevice hook probes for this
    // to unwrap virtual proxies before calling the driver.
    if (riid == IID_RevealReal)
    {
        if (!m_pReal1) { *ppv = nullptr; return E_NOINTERFACE; }
        *ppv = m_pReal1.Get();
        m_pReal1->AddRef();
        return S_OK;
    }

    if (riid == __uuidof(IUnknown)        ||
        riid == __uuidof(IDXGIObject)     ||
        riid == __uuidof(IDXGIAdapter)    ||
        riid == __uuidof(IDXGIAdapter1)   ||
        (m_pReal2 && riid == __uuidof(IDXGIAdapter2)) ||
        (m_pReal3 && riid == __uuidof(IDXGIAdapter3)))
    {
        *ppv = static_cast<IDXGIAdapter3*>(this);
        AddRef();
        return S_OK;
    }

    // Delegate to the real adapter for anything else (e.g. IDXGIAdapter4).
    // NOTE: This means the caller gets the real adapter for those interfaces,
    // which will report real (non-simulated) values.  Diligent's GpuInfoPanel
    // sticks to IDXGIAdapter1 + IDXGIAdapter3 so this is fine.
    if (m_pReal1)
        return m_pReal1->QueryInterface(riid, ppv);

    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE VirtualDXGIAdapter::AddRef()
{
    return m_RefCount.fetch_add(1, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE VirtualDXGIAdapter::Release()
{
    const ULONG NewCount = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1u;
    if (NewCount == 0)
        delete this;
    return NewCount;
}

// -----------------------------------------------------------------------
// IDXGIObject - delegated
// -----------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return m_pReal1 ? m_pReal1->SetPrivateData(Name, DataSize, pData) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return m_pReal1 ? m_pReal1->SetPrivateDataInterface(Name, pUnknown) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return m_pReal1 ? m_pReal1->GetPrivateData(Name, pDataSize, pData) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::GetParent(REFIID riid, void** ppParent)
{
    return m_pReal1 ? m_pReal1->GetParent(riid, ppParent) : E_FAIL;
}

// -----------------------------------------------------------------------
// IDXGIAdapter - GetDesc modified, others delegated
// -----------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::EnumOutputs(UINT Output, IDXGIOutput** ppOutput)
{
    return m_pReal1 ? m_pReal1->EnumOutputs(Output, ppOutput) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::GetDesc(DXGI_ADAPTER_DESC* pDesc)
{
    if (!m_pReal1) return E_FAIL;
    const HRESULT hr = m_pReal1->GetDesc(pDesc);
    if (SUCCEEDED(hr))
        ModifyDesc(pDesc);
    return hr;
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion)
{
    return m_pReal1 ? m_pReal1->CheckInterfaceSupport(InterfaceName, pUMDVersion) : E_FAIL;
}

// -----------------------------------------------------------------------
// IDXGIAdapter1 - GetDesc1 modified
// -----------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::GetDesc1(DXGI_ADAPTER_DESC1* pDesc)
{
    if (!m_pReal1) return E_FAIL;
    const HRESULT hr = m_pReal1->GetDesc1(pDesc);
    if (SUCCEEDED(hr))
        ModifyDesc(pDesc);
    return hr;
}

// -----------------------------------------------------------------------
// IDXGIAdapter2 - GetDesc2 modified
// -----------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::GetDesc2(DXGI_ADAPTER_DESC2* pDesc)
{
    if (!m_pReal2) return E_NOINTERFACE;
    const HRESULT hr = m_pReal2->GetDesc2(pDesc);
    if (SUCCEEDED(hr))
        ModifyDesc(pDesc);
    return hr;
}

// -----------------------------------------------------------------------
// IDXGIAdapter3
// -----------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent, DWORD* pdwCookie)
{
    return m_pReal3 ? m_pReal3->RegisterHardwareContentProtectionTeardownStatusEvent(hEvent, pdwCookie) : E_NOINTERFACE;
}

void STDMETHODCALLTYPE VirtualDXGIAdapter::UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie)
{
    if (m_pReal3) m_pReal3->UnregisterHardwareContentProtectionTeardownStatus(dwCookie);
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::QueryVideoMemoryInfo(
    UINT                            /*NodeIndex*/,
    DXGI_MEMORY_SEGMENT_GROUP       MemorySegmentGroup,
    DXGI_QUERY_VIDEO_MEMORY_INFO*   pVideoMemoryInfo)
{
    if (!m_pReal3 || pVideoMemoryInfo == nullptr) return E_NOINTERFACE;

    // On the real single-node adapter the only valid NodeIndex is 0.
    const HRESULT hr = m_pReal3->QueryVideoMemoryInfo(0, MemorySegmentGroup, pVideoMemoryInfo);
    if (SUCCEEDED(hr))
    {
        pVideoMemoryInfo->Budget                = SplitU64(pVideoMemoryInfo->Budget);
        pVideoMemoryInfo->CurrentUsage          = SplitU64(pVideoMemoryInfo->CurrentUsage);
        pVideoMemoryInfo->AvailableForReservation = SplitU64(pVideoMemoryInfo->AvailableForReservation);
        pVideoMemoryInfo->CurrentReservation    = SplitU64(pVideoMemoryInfo->CurrentReservation);
        LogVerbose("VirtualDXGIAdapter[%u]::QueryVideoMemoryInfo -> budget=%llu used=%llu",
                   m_NodeIndex,
                   static_cast<unsigned long long>(pVideoMemoryInfo->Budget),
                   static_cast<unsigned long long>(pVideoMemoryInfo->CurrentUsage));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::SetVideoMemoryReservation(
    UINT                        /*NodeIndex*/,
    DXGI_MEMORY_SEGMENT_GROUP   MemorySegmentGroup,
    UINT64                      Reservation)
{
    return m_pReal3 ? m_pReal3->SetVideoMemoryReservation(0, MemorySegmentGroup, Reservation) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE VirtualDXGIAdapter::RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent, DWORD* pdwCookie)
{
    return m_pReal3 ? m_pReal3->RegisterVideoMemoryBudgetChangeNotificationEvent(hEvent, pdwCookie) : E_NOINTERFACE;
}

void STDMETHODCALLTYPE VirtualDXGIAdapter::UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie)
{
    if (m_pReal3) m_pReal3->UnregisterVideoMemoryBudgetChangeNotification(dwCookie);
}

} // namespace D3D12Sim
