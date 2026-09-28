/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  VirtualDXGIAdapter
//  ------------------
//  Proxy COM object that presents ONE simulated linked-node adapter.  Every
//  IDXGIAdapter3 method is a pass-through to a real IDXGIAdapter3 backing
//  the primary physical adapter, except:
//
//    * GetDesc / GetDesc1 / GetDesc2   - modifies the returned Description to
//                                        append "[Simulated Node k]" and
//                                        divides DedicatedVideoMemory by
//                                        NodeCount (last node keeps the
//                                        remainder so totals match).
//    * QueryVideoMemoryInfo            - divides CurrentUsage / Budget /
//                                        AvailableForReservation /
//                                        CurrentReservation by NodeCount.
//    * SetVideoMemoryReservation       - remaps NodeIndex to 0 (single
//                                        physical node) before delegating.
//
//  The LUID is deliberately left unchanged so per-process GPU utilization
//  queried via PDH still matches through Diligent's GpuInfoPanel.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace D3D12Sim
{

class VirtualDXGIAdapter final : public IDXGIAdapter3
{
public:
    // Private IID: QueryInterface on a virtual adapter with this IID returns
    // the underlying real IDXGIAdapter1*.  Used by the D3D12CreateDevice hook
    // to substitute the real adapter before calling into the driver.
    //  {E7A3F4B2-1D5C-4E8A-9B01-F2C3D4E5F6A7}
    static constexpr GUID IID_RevealReal =
        {0xE7A3F4B2, 0x1D5C, 0x4E8A, {0x9B, 0x01, 0xF2, 0xC3, 0xD4, 0xE5, 0xF6, 0xA7}};

    // Creates a virtual adapter and returns it typed as IDXGIAdapter1* (the
    // base COM interface most enumerations return).  The caller receives one
    // reference; QueryInterface / AddRef / Release manage the rest.
    static IDXGIAdapter1* Create(IDXGIAdapter1* pReal, unsigned NodeIndex, unsigned NodeCount);

    // Returns the real backing adapter (owned by the proxy - no ref added).
    IDXGIAdapter1* GetRealAdapter() const { return m_pReal1.Get(); }

    // ---- IUnknown ---------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // ---- IDXGIObject ------------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // ---- IDXGIAdapter -----------------------------------------------------
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override;

    // ---- IDXGIAdapter1 ----------------------------------------------------
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override;

    // ---- IDXGIAdapter2 ----------------------------------------------------
    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* pDesc) override;

    // ---- IDXGIAdapter3 ----------------------------------------------------
    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void    STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) override;
    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, UINT64 Reservation) override;
    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void    STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) override;

private:
    VirtualDXGIAdapter(IDXGIAdapter1* pReal, unsigned NodeIndex, unsigned NodeCount);
    ~VirtualDXGIAdapter();

    // Divides a value by NodeCount; the last node absorbs any rounding remainder.
    UINT64 SplitU64(UINT64 Value) const;
    SIZE_T SplitSize(SIZE_T Value) const;

    // Rewrites Description + DedicatedVideoMemory in a DXGI_ADAPTER_DESC*.
    void ModifyDesc(DXGI_ADAPTER_DESC*  pDesc) const;
    void ModifyDesc(DXGI_ADAPTER_DESC1* pDesc) const;
    void ModifyDesc(DXGI_ADAPTER_DESC2* pDesc) const;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_pReal1;
    Microsoft::WRL::ComPtr<IDXGIAdapter2> m_pReal2; // May be null on older systems.
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_pReal3; // May be null on older systems.

    unsigned            m_NodeIndex = 0;
    unsigned            m_NodeCount = 1;
    std::atomic<ULONG>  m_RefCount{1};
};

} // namespace D3D12Sim
