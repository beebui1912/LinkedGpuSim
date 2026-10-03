/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  NodeMasks
//  ---------
//  The simulated topology (N nodes on one physical node) and the D3D12 rules
//  for node masks, applied to what the application passes in:
//
//    * single-node masks (command queues, command lists, descriptor heaps,
//      query heaps): 0 or exactly one bit, below N;
//    * node sets (pipeline states, root signatures, command signatures):
//      any subset of the N nodes;
//    * heap properties: CreationNodeMask 0 or one bit, VisibleNodeMask a
//      subset of the nodes that includes the creation node.
//
//  A call that breaks a rule is rejected with E_INVALIDARG, as the runtime on
//  linked hardware does, and counted as a validation error.  Every accepted
//  mask is then remapped to the single physical node before the driver sees it.
//
//  The simulated masks of queues, command lists, heaps and resources are kept
//  in the objects' private data, so that later use can be checked: a command
//  list must be executed on a queue of its node, and a copy may only touch
//  resources visible to the node of the command list.

#pragma once

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

UINT SimNodeCount();
UINT AllNodesMask();
bool ValidationEnabled();

// The mask the single physical node accepts
inline UINT ToPhysicalMask(UINT Mask) { return Mask == 0 ? 0u : 1u; }

// 0 means node 0
inline UINT NormalizeSingleNode(UINT Mask) { return Mask == 0 ? 1u : Mask; }

// Rule checks; each reports a validation error and returns false on violation
bool CheckSingleNodeMask(const char* Api, const char* Field, UINT Mask);
bool CheckNodeSetMask(const char* Api, const char* Field, UINT Mask);
bool CheckHeapNodeMasks(const char* Api, UINT CreationNodeMask, UINT VisibleNodeMask);

// Logs "VALIDATION ERROR: ..." (also to stderr) and counts it. After 50
// messages only the count grows.
void ReportValidationError(const char* Fmt, ...);
unsigned GetValidationErrorCount();

// Simulated node mask stored with a queue, command list, descriptor heap, query
// heap (one node) or a pipeline state, root signature, command signature (node
// set); 0 is stored as node 0
void SetObjectNodeMask(IUnknown* pObject, UINT NodeMask);
bool GetObjectNodeMask(IUnknown* pObject, UINT& NodeMask);

// Masks of a resource or heap as created (normalized), kept in D3D12Tracking
void SetResourceNodeMasks(IUnknown* pObject, UINT CreationNodeMask, UINT VisibleNodeMask);
bool GetResourceNodeMasks(const void* pObject, UINT& CreationNodeMask, UINT& VisibleNodeMask);

// D3D12_CROSS_NODE_SHARING_TIER as a level: 0 none, 1 copies, 2 and 3 copies
// and every other access. The access model of the simulator:
//   * a resource may only be accessed by nodes in its VisibleNodeMask;
//   * a node other than the creation node may access it with copy operations
//     from tier 1, and through views, root arguments, vertex/index/stream-out
//     buffers and as render target or depth buffer only from tier 2;
//   * at tier 0 a resource is visible to its creation node only.
UINT CrossNodeSharingLevel();

enum ACCESS_KIND
{
    ACCESS_COPY,   // copy, resolve and query-resolve commands
    ACCESS_SHADER, // views, root CBV/SRV/UAV, vertex/index/stream-out buffers, indirect arguments
    ACCESS_TARGET, // render target, depth-stencil
};

// Reports an access by node ListNodeMask that linked hardware rejects. Resources
// the shim did not see created are not checked. Returns false if reported.
bool CheckResourceAccess(const char* Api, const char* Role, UINT ListNodeMask, const void* pResource, ACCESS_KIND Kind);

} // namespace D3D12Sim

// Number of validation errors so far (for test harnesses running under the shim)
extern "C" __declspec(dllexport) unsigned D3D12Sim_GetValidationErrorCount();
// Simulated node count, 0 if the shim is not active
extern "C" __declspec(dllexport) unsigned D3D12Sim_GetSimulatedNodeCount();
