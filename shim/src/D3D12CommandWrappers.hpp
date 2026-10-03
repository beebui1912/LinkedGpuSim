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
//  Graphics command lists: CopyBufferRegion, CopyTextureRegion and
//  CopyResource check that the resources are visible to the list's node
//  (VisibleNodeMask), which is what cross-node copies need on linked hardware.
//  Other uses of resources (views, draws, dispatches) are not checked.

#pragma once

struct IUnknown;

namespace D3D12Sim
{

void WrapCommandQueue(IUnknown* pQueue, unsigned NodeMask);
void WrapCommandList(IUnknown* pList, unsigned NodeMask);

} // namespace D3D12Sim
