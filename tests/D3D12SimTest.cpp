/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12SimTest - checks D3D12Sim.dll on the real driver.
//
//  Loads the shim into this process (as SimulationApp's injection does), with
//  the D3D12 debug layer enabled as Diligent does, and checks that a 2-node
//  device behaves like linked hardware would: node masks of both nodes are
//  accepted, invalid ones rejected, cross-node misuse reported, and GPU work
//  submitted on "node 1" completes.  Exit code 0 = all checks passed.
//
//  Usage: D3D12SimTest [--shim PATH]   (default: D3D12Sim.dll next to the exe)

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{

int  g_Checks   = 0;
UINT g_Tier     = 1; // simulated cross-node sharing tier (--tier N)
int g_Failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        ++g_Checks;                                                          \
        if (!(cond))                                                         \
        {                                                                    \
            ++g_Failures;                                                    \
            std::printf("  FAILED line %d: %s\n", __LINE__, #cond);          \
        }                                                                    \
    } while (false)

using PFN_GetCount = unsigned (*)();
PFN_GetCount g_GetValidationErrors = nullptr;

unsigned ValidationErrors() { return g_GetValidationErrors != nullptr ? g_GetValidationErrors() : 0; }

// Runs F and reports whether the validation error count grew by Expected
template <typename F>
bool ExpectValidationErrors(unsigned Expected, F&& Fn)
{
    const unsigned Before = ValidationErrors();
    Fn();
    return ValidationErrors() - Before == Expected;
}

ComPtr<ID3D12Device> CreateDevice(IDXGIAdapter1* pAdapter)
{
    ComPtr<ID3D12Device> pDevice;
    D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice));
    return pDevice;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 Size)
{
    D3D12_RESOURCE_DESC D{};
    D.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    D.Width            = Size;
    D.Height           = 1;
    D.DepthOrArraySize = 1;
    D.MipLevels        = 1;
    D.SampleDesc.Count = 1;
    D.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return D;
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* pDevice, UINT Creation, UINT Visible, HRESULT* pHr = nullptr, UINT64 Size = 64 * 1024)
{
    D3D12_HEAP_PROPERTIES Props{};
    Props.Type             = D3D12_HEAP_TYPE_DEFAULT;
    Props.CreationNodeMask = Creation;
    Props.VisibleNodeMask  = Visible;
    D3D12_RESOURCE_DESC    Desc = BufferDesc(Size);
    Desc.Flags                  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> pBuffer;
    const HRESULT hr = pDevice->CreateCommittedResource(&Props, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pBuffer));
    if (pHr != nullptr)
        *pHr = hr;
    return pBuffer;
}

// Struct-returning methods, guarded: a wrong calling convention in a hook
// faults here instead of ending the test
bool SafeGetAllocationInfo(ID3D12Device* pDevice, UINT VisibleMask, D3D12_RESOURCE_ALLOCATION_INFO& Info)
{
    const D3D12_RESOURCE_DESC Desc = BufferDesc(64 * 1024);
    __try
    {
        Info = pDevice->GetResourceAllocationInfo(VisibleMask, 1, &Desc);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SafeGetCustomHeapProperties(ID3D12Device* pDevice, UINT NodeMask, D3D12_HEAP_PROPERTIES& Props)
{
    __try
    {
        Props = pDevice->GetCustomHeapProperties(NodeMask, D3D12_HEAP_TYPE_DEFAULT);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

ComPtr<ID3D12CommandQueue> CreateQueue(ID3D12Device* pDevice, UINT NodeMask, HRESULT* pHr = nullptr)
{
    D3D12_COMMAND_QUEUE_DESC Desc{};
    Desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Desc.NodeMask = NodeMask;
    ComPtr<ID3D12CommandQueue> pQueue;
    const HRESULT              hr = pDevice->CreateCommandQueue(&Desc, IID_PPV_ARGS(&pQueue));
    if (pHr != nullptr)
        *pHr = hr;
    return pQueue;
}

struct CommandList
{
    ComPtr<ID3D12CommandAllocator>    pAllocator;
    ComPtr<ID3D12GraphicsCommandList> pList;
};

CommandList CreateList(ID3D12Device* pDevice, UINT NodeMask)
{
    CommandList L;
    pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&L.pAllocator));
    if (L.pAllocator)
        pDevice->CreateCommandList(NodeMask, D3D12_COMMAND_LIST_TYPE_DIRECT, L.pAllocator.Get(), nullptr, IID_PPV_ARGS(&L.pList));
    return L;
}

bool WaitForQueue(ID3D12Device* pDevice, ID3D12CommandQueue* pQueue)
{
    ComPtr<ID3D12Fence> pFence;
    if (FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence))))
        return false;
    pQueue->Signal(pFence.Get(), 1);
    HANDLE Event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    pFence->SetEventOnCompletion(1, Event);
    const bool Done = ::WaitForSingleObject(Event, 5000) == WAIT_OBJECT_0;
    ::CloseHandle(Event);
    return Done && pFence->GetCompletedValue() == 1;
}

ComPtr<ID3DBlob> CompileComputeShader()
{
    const char       Source[] = "RWByteAddressBuffer Out : register(u0); [numthreads(1,1,1)] void main() { Out.Store(0, 1); }";
    ComPtr<ID3DBlob> pCode, pErrors;
    D3DCompile(Source, sizeof(Source) - 1, "cs", nullptr, nullptr, "main", "cs_5_0", 0, 0, &pCode, &pErrors);
    return pCode;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* pDevice, UINT NodeMask, HRESULT* pHr = nullptr)
{
    D3D12_ROOT_PARAMETER Param{};
    Param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    Param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC Desc{};
    Desc.NumParameters = 1;
    Desc.pParameters   = &Param;
    ComPtr<ID3DBlob> pBlob, pErrors;
    D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &pBlob, &pErrors);
    ComPtr<ID3D12RootSignature> pRS;
    const HRESULT               hr = pBlob ? pDevice->CreateRootSignature(NodeMask, pBlob->GetBufferPointer(), pBlob->GetBufferSize(), IID_PPV_ARGS(&pRS)) : E_FAIL;
    if (pHr != nullptr)
        *pHr = hr;
    return pRS;
}

// Compute PSO through a pipeline state stream (ID3D12Device2), with a NODE_MASK subobject
HRESULT CreateStreamPSO(ID3D12Device* pDevice, ID3D12RootSignature* pRS, ID3DBlob* pCS, UINT NodeMask)
{
    struct alignas(void*) RSSub { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* p; };
    struct alignas(void*) CSSub { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS; D3D12_SHADER_BYTECODE Code; };
    struct alignas(void*) NodeSub { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK; D3D12_NODE_MASK Mask; };
    struct Stream
    {
        RSSub   RS;
        CSSub   CS;
        NodeSub Node;
    } S;
    S.RS.p           = pRS;
    S.CS.Code        = {pCS->GetBufferPointer(), pCS->GetBufferSize()};
    S.Node.Mask      = {NodeMask};
    D3D12_PIPELINE_STATE_STREAM_DESC Desc{sizeof(S), &S};
    ComPtr<ID3D12Device2> pDevice2;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pDevice2))))
        return E_NOINTERFACE;
    ComPtr<ID3D12PipelineState> pPSO;
    const HRESULT               hr = pDevice2->CreatePipelineState(&Desc, IID_PPV_ARGS(&pPSO));
    // The node mask in the caller's stream is restored after the call
    if (S.Node.Mask.NodeMask != NodeMask)
        return E_UNEXPECTED;
    return hr;
}

UINT DebugLayerErrors(ID3D12Device* pDevice)
{
    ComPtr<ID3D12InfoQueue> pQueue;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))))
        return 0;
    UINT Errors = 0;
    for (UINT64 i = 0, n = pQueue->GetNumStoredMessages(); i < n; ++i)
    {
        SIZE_T Size = 0;
        pQueue->GetMessage(i, nullptr, &Size);
        std::string Buf(Size, '\0');
        auto*       pMsg = reinterpret_cast<D3D12_MESSAGE*>(Buf.data());
        if (SUCCEEDED(pQueue->GetMessage(i, pMsg, &Size)) && pMsg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        {
            ++Errors;
            std::printf("  [debug layer] %.*s\n", static_cast<int>(pMsg->DescriptionByteLength), pMsg->pDescription);
        }
    }
    pQueue->ClearStoredMessages();
    return Errors;
}

void TestLinkedDevice(ID3D12Device* pDevice)
{
    std::printf("Node count and struct-returning methods\n");
    CHECK(pDevice->GetNodeCount() == 2);

    D3D12_RESOURCE_ALLOCATION_INFO Info{};
    const bool                     InfoOk = SafeGetAllocationInfo(pDevice, 0x3, Info);
    CHECK(InfoOk);
    CHECK(InfoOk && Info.SizeInBytes >= 64 * 1024);

    D3D12_HEAP_PROPERTIES Props{};
    const bool            PropsOk = SafeGetCustomHeapProperties(pDevice, 0x2, Props);
    CHECK(PropsOk);
    CHECK(PropsOk && Props.CreationNodeMask == 0x2 && Props.VisibleNodeMask == 0x2);

    std::printf("Feature queries\n");
    D3D12_FEATURE_DATA_ARCHITECTURE Arch{};
    Arch.NodeIndex = 1;
    CHECK(SUCCEEDED(pDevice->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &Arch, sizeof(Arch))));
    CHECK(Arch.NodeIndex == 1);
    Arch.NodeIndex = 2;
    CHECK(pDevice->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &Arch, sizeof(Arch)) == E_INVALIDARG);
    D3D12_FEATURE_DATA_D3D12_OPTIONS Options{};
    CHECK(SUCCEEDED(pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &Options, sizeof(Options))));
    CHECK(Options.CrossNodeSharingTier == (g_Tier >= 2 ? D3D12_CROSS_NODE_SHARING_TIER_2 : D3D12_CROSS_NODE_SHARING_TIER_1));

    std::printf("Command queues\n");
    HRESULT hr     = E_FAIL;
    auto    pQueue0 = CreateQueue(pDevice, 0x1, &hr);
    CHECK(SUCCEEDED(hr) && pQueue0);
    auto pQueue1 = CreateQueue(pDevice, 0x2, &hr);
    CHECK(SUCCEEDED(hr) && pQueue1);
    CHECK(pQueue1 && pQueue1->GetDesc().NodeMask == 0x2); // the node the queue was created for
    CHECK(pQueue0 && pQueue0->GetDesc().NodeMask == 0x1);
    CHECK(ExpectValidationErrors(1, [&] { CreateQueue(pDevice, 0x4, &hr); }) && hr == E_INVALIDARG); // no node 2
    CHECK(ExpectValidationErrors(1, [&] { CreateQueue(pDevice, 0x3, &hr); }) && hr == E_INVALIDARG); // two nodes

    std::printf("Pipeline states and root signatures shared by both nodes\n");
    auto pRS = CreateRootSignature(pDevice, 0x3, &hr);
    CHECK(SUCCEEDED(hr) && pRS);
    auto pCS = CompileComputeShader();
    CHECK(pCS);
    if (pRS && pCS)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc{};
        PSODesc.pRootSignature = pRS.Get();
        PSODesc.CS             = {pCS->GetBufferPointer(), pCS->GetBufferSize()};
        PSODesc.NodeMask       = 0x3;
        ComPtr<ID3D12PipelineState> pPSO;
        CHECK(SUCCEEDED(pDevice->CreateComputePipelineState(&PSODesc, IID_PPV_ARGS(&pPSO))));
        CHECK(SUCCEEDED(CreateStreamPSO(pDevice, pRS.Get(), pCS.Get(), 0x3)));
        CHECK(ExpectValidationErrors(1, [&] { hr = CreateStreamPSO(pDevice, pRS.Get(), pCS.Get(), 0x5); }) && hr == E_INVALIDARG);
    }

    std::printf("Heap node masks\n");
    auto pOnNode0 = CreateBuffer(pDevice, 0x1, 0x1, &hr);
    CHECK(SUCCEEDED(hr));
    auto pOnNode1 = CreateBuffer(pDevice, 0x2, 0x2, &hr);
    CHECK(SUCCEEDED(hr));
    auto pShared = CreateBuffer(pDevice, 0x1, 0x3, &hr);
    CHECK(SUCCEEDED(hr));
    CHECK(ExpectValidationErrors(1, [&] { CreateBuffer(pDevice, 0x3, 0x3, &hr); }) && hr == E_INVALIDARG); // created on two nodes
    CHECK(ExpectValidationErrors(1, [&] { CreateBuffer(pDevice, 0x2, 0x1, &hr); }) && hr == E_INVALIDARG); // invisible to its creator
    CHECK(ExpectValidationErrors(1, [&] { CreateBuffer(pDevice, 0x1, 0x5, &hr); }) && hr == E_INVALIDARG); // no node 2

    std::printf("Cross-node copies and execution\n");
    if (pQueue0 && pQueue1 && pOnNode0 && pOnNode1 && pShared)
    {
        // Node 1 copies from a buffer visible to both nodes: valid, and it runs on the GPU
        CommandList L1 = CreateList(pDevice, 0x2);
        CHECK(L1.pList);
        CHECK(ExpectValidationErrors(0, [&] { L1.pList->CopyBufferRegion(pOnNode1.Get(), 0, pShared.Get(), 0, 256); }));
        L1.pList->Close();
        ID3D12CommandList* Lists[] = {L1.pList.Get()};
        CHECK(ExpectValidationErrors(0, [&] { pQueue1->ExecuteCommandLists(1, Lists); }));
        CHECK(WaitForQueue(pDevice, pQueue1.Get()));

        // Node 1 reads a buffer only node 0 can see: reported
        CommandList L2 = CreateList(pDevice, 0x2);
        CHECK(ExpectValidationErrors(1, [&] { L2.pList->CopyBufferRegion(pOnNode1.Get(), 0, pOnNode0.Get(), 0, 256); }));
        L2.pList->Close();
        CommandList L2b = CreateList(pDevice, 0x2);
        CHECK(ExpectValidationErrors(1, [&] { L2b.pList->CopyResource(pOnNode0.Get(), pOnNode1.Get()); }));
        L2b.pList->Close();

        // A node-1 list on the node-0 queue: reported
        CommandList L3 = CreateList(pDevice, 0x2);
        L3.pList->Close();
        ID3D12CommandList* Wrong[] = {L3.pList.Get()};
        CHECK(ExpectValidationErrors(1, [&] { pQueue0->ExecuteCommandLists(1, Wrong); }));
        CHECK(WaitForQueue(pDevice, pQueue0.Get()));
    }
}

// Tables of one UAV (u0) in a root signature for both nodes
ComPtr<ID3D12RootSignature> CreateTableRootSignature(ID3D12Device* pDevice)
{
    D3D12_DESCRIPTOR_RANGE Range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER   Param{};
    Param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Param.DescriptorTable.NumDescriptorRanges = 1;
    Param.DescriptorTable.pDescriptorRanges   = &Range;
    D3D12_ROOT_SIGNATURE_DESC Desc{1, &Param};
    ComPtr<ID3DBlob>          pBlob, pErrors;
    D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &pBlob, &pErrors);
    ComPtr<ID3D12RootSignature> pRS;
    if (pBlob)
        pDevice->CreateRootSignature(0x3, pBlob->GetBufferPointer(), pBlob->GetBufferSize(), IID_PPV_ARGS(&pRS));
    return pRS;
}

ComPtr<ID3D12PipelineState> CreateComputePSO(ID3D12Device* pDevice, ID3D12RootSignature* pRS, ID3DBlob* pCS, UINT NodeMask)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC Desc{};
    Desc.pRootSignature = pRS;
    Desc.CS             = {pCS->GetBufferPointer(), pCS->GetBufferSize()};
    Desc.NodeMask       = NodeMask;
    ComPtr<ID3D12PipelineState> pPSO;
    pDevice->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&pPSO));
    return pPSO;
}

ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(ID3D12Device* pDevice, D3D12_DESCRIPTOR_HEAP_TYPE Type, bool ShaderVisible, UINT NodeMask)
{
    D3D12_DESCRIPTOR_HEAP_DESC Desc{Type, 4, ShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, NodeMask};
    ComPtr<ID3D12DescriptorHeap> pHeap;
    pDevice->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&pHeap));
    return pHeap;
}

void CreateRawUAV(ID3D12Device* pDevice, ID3D12Resource* pBuffer, D3D12_CPU_DESCRIPTOR_HANDLE Handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC Desc{};
    Desc.Format              = DXGI_FORMAT_R32_TYPELESS;
    Desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
    Desc.Buffer.NumElements  = 16;
    Desc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;
    pDevice->CreateUnorderedAccessView(pBuffer, nullptr, &Desc, Handle);
}

ComPtr<ID3D12Resource> CreateRenderTarget(ID3D12Device* pDevice, UINT Creation, UINT Visible)
{
    D3D12_HEAP_PROPERTIES Props{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, Creation, Visible};
    D3D12_RESOURCE_DESC   Desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> pTexture;
    pDevice->CreateCommittedResource(&Props, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&pTexture));
    return pTexture;
}

// What linked hardware enforces beyond copies: shader access and render targets
// across nodes need tier 2; descriptor heaps, query heaps and pipeline states
// belong to their nodes
void TestAccessModel(ID3D12Device* pDevice)
{
    std::printf("Cross-node access through root views and descriptor tables (tier %u)\n", g_Tier);
    auto pCS      = CompileComputeShader();
    auto pRootRS  = CreateRootSignature(pDevice, 0x3);
    auto pTableRS = CreateTableRootSignature(pDevice);
    CHECK(pCS && pRootRS && pTableRS);
    if (!pCS || !pRootRS || !pTableRS)
        return;
    auto pRootPSO  = CreateComputePSO(pDevice, pRootRS.Get(), pCS.Get(), 0x3);
    auto pTablePSO = CreateComputePSO(pDevice, pTableRS.Get(), pCS.Get(), 0x3);
    auto pNode0PSO = CreateComputePSO(pDevice, pRootRS.Get(), pCS.Get(), 0x1);
    auto pLocal    = CreateBuffer(pDevice, 0x2, 0x2);
    auto pRemote   = CreateBuffer(pDevice, 0x1, 0x3); // node 0's memory, visible to node 1
    auto pQueue1   = CreateQueue(pDevice, 0x2);
    CHECK(pRootPSO && pTablePSO && pNode0PSO && pLocal && pRemote && pQueue1);
    if (!pRootPSO || !pTablePSO || !pNode0PSO || !pLocal || !pRemote || !pQueue1)
        return;
    const unsigned RemoteShaderErrors = g_Tier >= 2 ? 0 : 1;

    // Root UAV: local is fine and runs; remote needs tier 2
    {
        CommandList L = CreateList(pDevice, 0x2);
        L.pList->SetComputeRootSignature(pRootRS.Get());
        L.pList->SetPipelineState(pRootPSO.Get());
        L.pList->SetComputeRootUnorderedAccessView(0, pLocal->GetGPUVirtualAddress());
        CHECK(ExpectValidationErrors(0, [&] { L.pList->Dispatch(1, 1, 1); }));
        L.pList->SetComputeRootUnorderedAccessView(0, pRemote->GetGPUVirtualAddress() + 256); // inside the buffer
        CHECK(ExpectValidationErrors(RemoteShaderErrors, [&] { L.pList->Dispatch(1, 1, 1); }));
        L.pList->Close();
        if (g_Tier >= 2)
        {
            ID3D12CommandList* Lists[] = {L.pList.Get()};
            pQueue1->ExecuteCommandLists(1, Lists);
            CHECK(WaitForQueue(pDevice, pQueue1.Get()));
        }
    }

    // Descriptor table: the UAV is found through the shader-visible heap of node 1
    {
        auto pHeap1 = CreateDescriptorHeap(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true, 0x2);
        auto pHeap0 = CreateDescriptorHeap(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true, 0x1);
        auto pCpu   = CreateDescriptorHeap(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, false, 0x2);
        CHECK(pHeap1 && pHeap0 && pCpu);
        if (pHeap1 && pHeap0 && pCpu)
        {
            const UINT Inc = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            // Views are created in a CPU heap and copied into the shader-visible heap, as engines do
            D3D12_CPU_DESCRIPTOR_HANDLE CpuStart = pCpu->GetCPUDescriptorHandleForHeapStart();
            CreateRawUAV(pDevice, pLocal.Get(), CpuStart);
            CreateRawUAV(pDevice, pRemote.Get(), {CpuStart.ptr + Inc});
            D3D12_CPU_DESCRIPTOR_HANDLE Visible = pHeap1->GetCPUDescriptorHandleForHeapStart();
            pDevice->CopyDescriptorsSimple(2, Visible, CpuStart, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            CommandList            L        = CreateList(pDevice, 0x2);
            ID3D12DescriptorHeap*  Heaps1[] = {pHeap1.Get()};
            ID3D12DescriptorHeap*  Heaps0[] = {pHeap0.Get()};
            CHECK(ExpectValidationErrors(1, [&] { L.pList->SetDescriptorHeaps(1, Heaps0); })); // node 0's heap
            CHECK(ExpectValidationErrors(0, [&] { L.pList->SetDescriptorHeaps(1, Heaps1); }));
            L.pList->SetComputeRootSignature(pTableRS.Get());
            L.pList->SetPipelineState(pTablePSO.Get());
            const D3D12_GPU_DESCRIPTOR_HANDLE Gpu = pHeap1->GetGPUDescriptorHandleForHeapStart();
            L.pList->SetComputeRootDescriptorTable(0, Gpu);
            CHECK(ExpectValidationErrors(0, [&] { L.pList->Dispatch(1, 1, 1); }));
            L.pList->SetComputeRootDescriptorTable(0, {Gpu.ptr + Inc});
            CHECK(ExpectValidationErrors(RemoteShaderErrors, [&] { L.pList->Dispatch(1, 1, 1); }));
            L.pList->Close();
        }
    }

    std::printf("Objects of one node\n");
    {
        CommandList L = CreateList(pDevice, 0x2);
        CHECK(ExpectValidationErrors(1, [&] { L.pList->SetPipelineState(pNode0PSO.Get()); })); // created for node 0 only
        D3D12_QUERY_HEAP_DESC   QDesc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0x1};
        ComPtr<ID3D12QueryHeap> pQueries0;
        pDevice->CreateQueryHeap(&QDesc, IID_PPV_ARGS(&pQueries0));
        QDesc.NodeMask = 0x2;
        ComPtr<ID3D12QueryHeap> pQueries1;
        pDevice->CreateQueryHeap(&QDesc, IID_PPV_ARGS(&pQueries1));
        CHECK(pQueries0 && pQueries1);
        if (pQueries0 && pQueries1)
        {
            CHECK(ExpectValidationErrors(1, [&] { L.pList->EndQuery(pQueries0.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); }));
            CHECK(ExpectValidationErrors(0, [&] { L.pList->EndQuery(pQueries1.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); }));
        }
        L.pList->Close();
    }

    std::printf("Render targets of another node\n");
    {
        auto pLocalRT  = CreateRenderTarget(pDevice, 0x2, 0x2);
        auto pRemoteRT = CreateRenderTarget(pDevice, 0x1, 0x3);
        auto pRtvHeap  = CreateDescriptorHeap(pDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, false, 0x2);
        CHECK(pLocalRT && pRemoteRT && pRtvHeap);
        if (pLocalRT && pRemoteRT && pRtvHeap)
        {
            const UINT                  Inc   = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE Rtv0  = pRtvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE Rtv1  = {Rtv0.ptr + Inc};
            pDevice->CreateRenderTargetView(pLocalRT.Get(), nullptr, Rtv0);
            pDevice->CreateRenderTargetView(pRemoteRT.Get(), nullptr, Rtv1);
            const float Color[4] = {0, 0, 0, 1};
            CommandList L        = CreateList(pDevice, 0x2);
            CHECK(ExpectValidationErrors(0, [&] { L.pList->ClearRenderTargetView(Rtv0, Color, 0, nullptr); }));
            CHECK(ExpectValidationErrors(RemoteShaderErrors, [&] { L.pList->ClearRenderTargetView(Rtv1, Color, 0, nullptr); }));
            L.pList->Close();
        }
    }
}

// A linked adapter reports video memory per node; swap chain buffers live on a node
void TestDxgi(ID3D12Device* pDevice, LUID HostLuid)
{
    std::printf("Per-node video memory\n");
    ComPtr<IDXGIFactory4> pFactory;
    CreateDXGIFactory1(IID_PPV_ARGS(&pFactory)); // created after the shim: wrapped
    ComPtr<IDXGIAdapter3> pAdapter;
    CHECK(pFactory && SUCCEEDED(pFactory->EnumAdapterByLuid(HostLuid, IID_PPV_ARGS(&pAdapter))));
    if (!pAdapter)
        return;
    DXGI_QUERY_VIDEO_MEMORY_INFO Node0{}, Node1{}, Invalid{};
    CHECK(SUCCEEDED(pAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Node0)));
    CHECK(SUCCEEDED(pAdapter->QueryVideoMemoryInfo(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Node1)));
    CHECK(pAdapter->QueryVideoMemoryInfo(2, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Invalid) == DXGI_ERROR_INVALID_CALL);
    CHECK(Node0.Budget > 0 && Node0.Budget == Node1.Budget);
    {
        constexpr UINT64 Size = 64ull << 20;
        auto             pBig = CreateBuffer(pDevice, 0x2, 0x2, nullptr, Size); // charged to node 1
        CHECK(pBig);
        DXGI_QUERY_VIDEO_MEMORY_INFO After0{}, After1{};
        pAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &After0);
        pAdapter->QueryVideoMemoryInfo(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &After1);
        std::printf("  node 1 usage %llu -> %llu MB, node 0 %llu -> %llu MB, budget per node %llu MB\n", Node1.CurrentUsage >> 20, After1.CurrentUsage >> 20,
                    Node0.CurrentUsage >> 20, After0.CurrentUsage >> 20, After0.Budget >> 20);
        CHECK(After1.CurrentUsage >= Node1.CurrentUsage + Size);
        CHECK(After0.CurrentUsage < Node0.CurrentUsage + Size / 2);
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO Released{};
    pAdapter->QueryVideoMemoryInfo(1, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Released);
    CHECK(Released.CurrentUsage < Node1.CurrentUsage + (32ull << 20)); // the charge goes away with the buffer

    std::printf("Swap chain buffers live on the node of their queue\n");
    WNDCLASSW Class{};
    Class.lpfnWndProc   = DefWindowProcW;
    Class.hInstance     = GetModuleHandleW(nullptr);
    Class.lpszClassName = L"D3D12SimTest";
    RegisterClassW(&Class);
    HWND hWnd = CreateWindowExW(0, Class.lpszClassName, L"D3D12SimTest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, Class.hInstance, nullptr);
    auto pQueue1 = CreateQueue(pDevice, 0x2);
    CHECK(hWnd != nullptr && pQueue1);
    if (hWnd == nullptr || !pQueue1)
        return;
    DXGI_SWAP_CHAIN_DESC1 Desc{64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, DXGI_SCALING_STRETCH,
                               DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0};
    ComPtr<IDXGISwapChain1> pSwapChain;
    CHECK(SUCCEEDED(pFactory->CreateSwapChainForHwnd(pQueue1.Get(), hWnd, &Desc, nullptr, nullptr, &pSwapChain)));
    if (pSwapChain)
    {
        ComPtr<ID3D12Resource> pBackBuffer;
        CHECK(SUCCEEDED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))));
        auto pTarget = CreateBuffer(pDevice, 0x1, 0x1);
        if (pBackBuffer && pTarget)
        {
            // Node 0 cannot even see a buffer of node 1's swap chain
            CommandList                 L = CreateList(pDevice, 0x1);
            D3D12_TEXTURE_COPY_LOCATION Src{pBackBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            D3D12_TEXTURE_COPY_LOCATION Dst{pTarget.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            Dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
            CHECK(ExpectValidationErrors(1, [&] { L.pList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr); }));
            L.pList->Close();
        }
    }
    pSwapChain.Reset();
    DestroyWindow(hWnd);
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep the output of a crashing shim
    // Locate the shim
    wchar_t ExePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, ExePath, MAX_PATH);
    std::wstring ShimPath = ExePath;
    ShimPath              = ShimPath.substr(0, ShimPath.find_last_of(L"\\/") + 1) + L"D3D12Sim.dll";
    for (int a = 1; a + 1 < argc; ++a)
    {
        if (std::strcmp(argv[a], "--shim") == 0)
            ShimPath = std::wstring(argv[a + 1], argv[a + 1] + std::strlen(argv[a + 1]));
        if (std::strcmp(argv[a], "--tier") == 0)
            g_Tier = static_cast<UINT>(std::atoi(argv[a + 1]));
    }

    // Host = first hardware adapter; a second one, if any, must keep one node
    ComPtr<IDXGIFactory4> pFactory;
    CreateDXGIFactory1(IID_PPV_ARGS(&pFactory));
    ComPtr<IDXGIAdapter1> pHost, pOther;
    for (UINT i = 0; pFactory; ++i)
    {
        ComPtr<IDXGIAdapter1> A;
        if (pFactory->EnumAdapters1(i, &A) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 D{};
        A->GetDesc1(&D);
        if (D.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        (!pHost ? pHost : pOther) = A;
        if (pOther)
            break;
    }
    if (!pHost)
    {
        std::printf("No hardware adapter\n");
        return 2;
    }
    DXGI_ADAPTER_DESC1 HostDesc{};
    pHost->GetDesc1(&HostDesc);
    std::printf("Host adapter: %ls\n", HostDesc.Description);

    wchar_t Luid[64];
    std::swprintf(Luid, 64, L"0x%08X_0x%08X", static_cast<unsigned>(HostDesc.AdapterLuid.HighPart), static_cast<unsigned>(HostDesc.AdapterLuid.LowPart));
    ::SetEnvironmentVariableW(L"DILIGENT_SIM_LINKED_NODE_COUNT", L"2");
    ::SetEnvironmentVariableW(L"DILIGENT_SIM_HOST_ADAPTER_LUID", Luid);
    ::SetEnvironmentVariableW(L"DILIGENT_SIM_CROSS_NODE_TIER", std::to_wstring(g_Tier).c_str());

    HMODULE hShim = ::LoadLibraryW(ShimPath.c_str());
    if (hShim == nullptr)
    {
        std::printf("Could not load %ls\n", ShimPath.c_str());
        return 2;
    }
    g_GetValidationErrors = reinterpret_cast<PFN_GetCount>(::GetProcAddress(hShim, "D3D12Sim_GetValidationErrorCount"));
    if (g_GetValidationErrors == nullptr)
        std::printf("note: the shim does not export D3D12Sim_GetValidationErrorCount (validation checks will fail)\n");

    ComPtr<ID3D12Debug> pDebug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebug))))
        pDebug->EnableDebugLayer();

    {
        auto pDevice = CreateDevice(pHost.Get());
        CHECK(pDevice);
        if (pDevice)
        {
            TestLinkedDevice(pDevice.Get());
            TestAccessModel(pDevice.Get());
            TestDxgi(pDevice.Get(), HostDesc.AdapterLuid);
            CHECK(DebugLayerErrors(pDevice.Get()) == 0);
        }
    }

    std::printf("Devices created again (possibly at the same address) are wrapped again\n");
    for (int i = 0; i < 4; ++i)
    {
        auto pDevice = CreateDevice(pHost.Get());
        CHECK(pDevice && pDevice->GetNodeCount() == 2);
    }

    if (pOther)
    {
        std::printf("A device on another adapter keeps its real node count\n");
        auto pDevice = CreateDevice(pOther.Get());
        CHECK(pDevice && pDevice->GetNodeCount() == 1);
    }

    std::printf("%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
