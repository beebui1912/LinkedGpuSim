/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  D3D12CommandWrappers
//  --------------------
//  Command queues: GetDesc() reports the node mask the queue was created with
//  (the driver only knows node 0), and ExecuteCommandLists() checks that every
//  command list belongs to the queue's node.
//
//  Graphics command lists check every resource access against the node of the
//  list (NodeMasks.hpp, CheckResourceAccess): copies, resolves, clears, render
//  targets (OMSetRenderTargets, BeginRenderPass), and at each draw, dispatch and
//  ExecuteIndirect the resources reachable through the bound root signature
//  (descriptor tables, root CBV/SRV/UAV), vertex/index/stream-out buffers and
//  indirect argument buffers. They also check that descriptor heaps and query
//  heaps belong to the list's node, and that pipeline states, root signatures,
//  command signatures and bundles were created for it.

#pragma once

struct IUnknown;

namespace D3D12Sim
{

void WrapCommandQueue(IUnknown* pQueue, unsigned NodeMask);
void WrapCommandList(IUnknown* pList, unsigned NodeMask);

// A command signature: its node set and whether its commands are dispatches
void RegisterCommandSignature(IUnknown* pSignature, unsigned NodeMask, bool Compute);

} // namespace D3D12Sim
