# LinkedGpuSim

Runs a Diligent (or any D3D12 / Vulkan) application on **one physical GPU as if it were a linked
multi-GPU adapter** with N nodes (a D3D12 linked display adapter, or a Vulkan device group), and
**checks that the application uses the nodes the way real linked hardware requires**.

Linked hardware is rare (SLI/CrossFire-class bridges, multi-GPU boards). Without it, code paths for
linked mode do not run at all. LinkedGpuSim makes them run on an ordinary GPU and reports the
mistakes that would only show on linked hardware: a node mask naming a node that does not exist, a
copy that reads memory its node cannot see, a command list executed on another node's queue, a
device mask outside a command buffer's devices, mapping memory that has one instance per device.

> **Platform:** Windows 10+ (x64). D3D12 shim: DXGI 1.4. Vulkan layer: LunarG Vulkan SDK to build.

## What is simulated, and what is not

| | Simulated faithfully | Not simulated |
|---|---|---|
| Topology | The host adapter is **one** DXGI adapter / one Vulkan device group with N nodes (`ID3D12Device::GetNodeCount`; group devices that share the adapter LUID and differ in `deviceNodeMask`). Other adapters keep one node. | — |
| Node masks (D3D12) | Every API that takes a node mask accepts the N nodes, applies the D3D12 rules, and passes node 0 to the driver. Queues report the node they were created for. | — |
| Cross-node access (D3D12) | `CrossNodeSharingTier` as configured (`--cross-node-tier`): tier 0 = a resource is visible to its creation node only; tier 1 = other visible nodes may copy it; tier 2+ = they may also use it through views, root arguments, IA/SO buffers, render targets. Checked at every draw, dispatch, `ExecuteIndirect`, clear, copy, resolve, query resolve and render pass. Objects with a node (descriptor heaps, query heaps, PSOs, root and command signatures, bundles) must belong to the list's node. | — |
| Device masks (Vulkan) | Every structure with a device mask or device index is checked against the group and mapped to the one real device. | — |
| Execution | Work on each node runs on the real GPU, on its own queue. Cross-node fences and semaphores work. | Parallelism: all nodes share one GPU, so timings show no speed-up. |
| Memory (D3D12) | One memory, as on real hardware (a resource lives on its creation node and is read remotely by visible nodes). `QueryVideoMemoryInfo(NodeIndex)` answers per node: budget = adapter budget / N, usage = what was created on that node. | Bandwidth of the node link. |
| Memory (Vulkan) | Device-local memory has **one instance per device**, as on a real group: the layer records which instances each copy, clear, attachment write and so on reaches, and reports a read of data that is not in the reading device's instance (a missed peer copy). Peer bindings are checked against the reported peer memory features. | Writes through descriptors (storage buffers/images are excluded from the model so unseen writes cannot cause false reports). The data itself is shared: a missed copy is reported, not visible as wrong pixels. |
| Presentation | D3D12: swap chain buffers live on the node of the queue they were created with (`ResizeBuffers1`: per buffer). Vulkan: `presentMask` (every device presents), LOCAL and REMOTE modes, checked at present. | — |
| Checks | Node masks at creation (rejected with `E_INVALIDARG`, as the runtime does); access of every kind above; command lists on another node's queue; Vulkan device masks, device indices, peer memory, stale instances, mapping multi-instance memory. | — |

## Contents

```
LinkedGpuSim/
├── CMakeLists.txt              Standalone build (fetches MinHook)
├── src/                        SimulationApp.exe: launcher, log capture, GPU info panel
├── shim/                       D3D12Sim.dll: D3D12 shim injected into the child
│   └── src/
│       ├── D3D12Hook                 MinHook on D3D12CreateDevice; wraps devices on the host adapter
│       ├── D3D12DeviceWrapper        ID3D12Device..10: node count, node masks of every Create* method
│       ├── D3D12CommandWrappers      queues and command lists: bound state, access checks per command
│       ├── D3D12Tracking             registries: resources (masks, VA range, size), descriptor heaps,
│       │                             descriptors, root-signature layouts, per-node memory usage
│       ├── NodeMasks                 the D3D12 node-mask rules, access model, validation reporting
│       ├── VtableShadow              per-class vtable copies with patched slots
│       ├── D3D12Vtable[Check]        slot indices, verified against the Windows SDK at compile time
│       ├── DXGIHook, DXGIFactoryWrapper   per-node adapter memory, swap chain buffers on their node
│       ├── VirtualDXGIAdapter        optional one-adapter-per-node view (see below)
│       └── ShimConfig, ShimLog
├── shim_vk/                    VkLayer_DiligentGpuSim.dll: Vulkan layer
│   └── src/
│       ├── LayerMain                 instance level: device groups, identities, host by LUID, vkCreateDevice
│       ├── LayerDevice               device level: device masks and indices, presentation, model hooks
│       ├── LayerMemoryModel          memory instances per device, stale-read and peer-access checks
│       ├── LayerConfig               settings, validation reporting
│       ├── PhysicalDeviceThunks      unwrap thunks for physical-device functions the layer does not know
│       └── WrappedPhysicalDevice     dispatchable handles for the simulated devices 1..N-1
├── launcher/                   SimulationLauncher.exe: Win32 dialog that starts SimulationApp
└── tests/                      D3D12SimTest, VkSimTest: self-tests on the real driver (ctest)
```

## Build

```powershell
cmake -S LinkedGpuSim -B LinkedGpuSim/build
cmake --build LinkedGpuSim/build --config Release
ctest --test-dir LinkedGpuSim/build -C Release      # self-tests, need a GPU
```

Binaries land in `build/bin`. The Vulkan layer is built when the Vulkan SDK is found.

## Usage

```
SimulationApp.exe [options] [--] <child.exe> [child-args...]
```

```powershell
# A Diligent application on a simulated 2-node linked adapter (D3D12 shim and Vulkan layer)
SimulationApp.exe --driver-shim -- MyApp.exe --mode D3D12

# 4 nodes on the second adapter, custom log
SimulationApp.exe --driver-shim -n 4 -a 1 -l C:\logs\run.log -- MyApp.exe

# Only print the host GPU panel
SimulationApp.exe --info-only
```

| Option | Description |
|---|---|
| `--driver-shim` | Inject `D3D12Sim.dll` and enable the Vulkan layer in the child. Without it the child only gets the `DILIGENT_SIM_*` environment variables. |
| `-n`, `--nodes N` | Simulated nodes (1..8). Default 2. |
| `-a`, `--adapter INDEX` | Host adapter by DXGI index. Default: first hardware adapter. |
| `--no-validation` | Do not check node masks and device masks (they are still remapped). |
| `--cross-node-tier N` | D3D12 cross-node sharing tier reported to the child (0..3, default 1: copies). |
| `--virtual-adapters` | Also list one DXGI adapter per node, for UIs that show one entry per GPU. Off by default: a real linked adapter is one DXGI adapter with several nodes. |
| `-l`, `--log PATH` / `--no-log` | Log file (default `<exe dir>\SimulationApp.log`) or console only. |
| `--no-debug-capture` | Do not capture `OutputDebugString` (needed when a debugger owns it). |
| `--refresh SEC` | GPU stats snapshot interval in the log; 0 disables. Default 2. |
| `--cwd DIR` | Working directory of the child (`.` = the caller's). Default: the child's executable directory. |
| `--shim-dll PATH` | Explicit `D3D12Sim.dll` (implies `--driver-shim`). |

**Exit code:** the child's exit code; **3** if the child exited with 0 but the shims reported
validation errors (counted from the `[shim] VALIDATION ERROR` lines on the child's stderr). This
makes SimulationApp usable as a test runner.

### Environment of the child

| Variable | Meaning |
|---|---|
| `DILIGENT_SIM_LINKED_NODE_COUNT` | Simulated nodes. |
| `DILIGENT_SIM_LINKED_NODE_MASK` | Mask of the nodes, e.g. `0x00000003`. |
| `DILIGENT_SIM_HOST_ADAPTER`, `DILIGENT_SIM_HOST_ADAPTER_LUID` | Host adapter name and LUID (`0xHIGH_0xLOW`). Only this adapter gets nodes, in D3D12 and Vulkan. |
| `DILIGENT_SIM_VALIDATION` | `0` with `--no-validation`. |
| `DILIGENT_SIM_VIRTUAL_ADAPTERS` | `1` with `--virtual-adapters`. |
| `DILIGENT_SIM_CROSS_NODE_TIER` | `--cross-node-tier`. |
| `DILIGENT_SIM_VK_SUBSET_ALLOCATION` | `0` (set it yourself) reports `subsetAllocation` false for the group: every allocation then has an instance on every device, whatever its device mask. Default 1. |
| `DILIGENT_SIM_VK_PEER_MEMORY_FEATURES` | Peer memory features of device-local heaps (`VkPeerMemoryFeatureFlags`, set it yourself). Default `0xB` (COPY_SRC, COPY_DST, GENERIC_DST: copies both ways and writes, no generic reads of peer memory); COPY_DST is always added (required by the spec). |
| `DILIGENT_SIM_LOG_FILE`, `DILIGENT_SIM_PARENT_PID` | Shared log file, parent PID. |
| `DILIGENT_SIM_VERBOSE` | `1` (set it yourself) traces wrapped objects and remapped calls. |
| `VK_ADD_LAYER_PATH`, `VK_INSTANCE_LAYERS` | With `--driver-shim`: enable the layer. `VK_ADD_LAYER_PATH` adds to the loader's search, so the child's own layers (validation) stay available; existing values are kept. |

### From a test harness

The shims export their error counts, so a test can fail on them directly:

```cpp
if (HMODULE h = GetModuleHandleW(L"D3D12Sim.dll"))
    Errors += reinterpret_cast<unsigned (*)()>(GetProcAddress(h, "D3D12Sim_GetValidationErrorCount"))();
if (HMODULE h = GetModuleHandleW(L"VkLayer_DiligentGpuSim.dll"))
    Errors += reinterpret_cast<unsigned (*)()>(GetProcAddress(h, "VkSim_GetValidationErrorCount"))();
```

The Vulkan layer needs no injection: setting `VK_ADD_LAYER_PATH=<build\bin>`,
`VK_INSTANCE_LAYERS=VK_LAYER_DiligentGraphics_LinkedGpuSim` and `DILIGENT_SIM_LINKED_NODE_COUNT`
before `vkCreateInstance` is enough.

## D3D12 shim (D3D12Sim.dll)

SimulationApp starts the child suspended, loads the DLL into it (`CreateRemoteThread` +
`LoadLibraryW`), then resumes it. The DLL pins itself (it is never unloaded: wrapped objects
dispatch through it) and hooks `d3d12!D3D12CreateDevice` with MinHook.

**Devices.** A device on the host adapter (by LUID) gets a shadow vtable: a copy of its class's
vtable with these methods replaced. Struct-returning methods use the Windows COM ABI (result pointer
after `This`).

| Method | Rule checked | Passed to the driver |
|---|---|---|
| `GetNodeCount` | — | returns N |
| `CreateCommandQueue`, `CreateCommandQueue1` | one node | node 0; the queue is wrapped |
| `CreateCommandList`, `CreateCommandList1` | one node | node 0; the list is wrapped |
| `CreateDescriptorHeap`, `CreateQueryHeap` | one node | node 0 |
| `CreateGraphicsPipelineState`, `CreateComputePipelineState`, `CreatePipelineState` (stream `NODE_MASK` subobject), `CreateStateObject` (`NODE_MASK` subobjects), `CreateRootSignature`, `CreateCommandSignature` | subset of the nodes | node 0 |
| `CreateCommittedResource[1/2/3]`, `CreateHeap[1]`, `GetCustomHeapProperties` | creation node: one node; visible nodes: a subset that includes the creation node | node 0; masks kept with the object |
| `CreatePlacedResource[1/2]` | — | the heap's masks kept with the resource |
| `GetResourceAllocationInfo[1/2]` | subset of the nodes (reported only) | node 0 |
| `CheckFeatureSupport` | node index below N for node-indexed features | node 0; `CrossNodeSharingTier` simulated |
| `Create*View`, `CopyDescriptors[Simple]` | — | unchanged; the descriptor's resource is recorded |

A rejected call returns `E_INVALIDARG`, as the runtime does on linked hardware, and is reported.
At tier 0 a resource or heap whose `VisibleNodeMask` names another node than its creation node is
rejected the same way.

**Queues** report the node mask they were created with from `GetDesc()` (applications such as
Diligent derive a context's node from it). `ExecuteCommandLists` reports command lists of another node.

**Command lists** keep the state a draw or dispatch depends on: descriptor heaps, root signatures
(layouts parsed with the root signature deserializer), descriptor tables, root CBV/SRV/UAV (GPU
virtual addresses resolved to resources), vertex/index/stream-out buffers, render targets and the
depth buffer. Every draw, dispatch and `ExecuteIndirect` checks each resource reachable through
that state against the access model; so do clears, copies, resolves, `ResolveQueryData` and
`BeginRenderPass`. Each violation is reported once per list and resource.

Simulated masks are stored with the objects and dropped when the object is released (a lifetime
token in its private data), so an object allocated at the address of a released one starts clean.
A device created again at a released device's address is wrapped again.

**DXGI.** Adapters returned by any factory answer `QueryVideoMemoryInfo` and
`SetVideoMemoryReservation` for node indices below N on the host adapter, like a linked adapter:
each node's local budget is the adapter's divided by N, its usage the resources and heaps created on
it plus a share of what the shim cannot attribute. Swap chain buffers created on a queue of node k
are on node k (visible to it only); `ResizeBuffers1` places each buffer on the node given for it.

**Virtual DXGI adapters** (`--virtual-adapters`, off by default): `EnumAdapters[1]` lists one proxy
adapter per node (description `[Simulated Node k]`, memory divided by N) before the real adapters,
and `D3D12CreateDevice` on a proxy creates the device on the real adapter. This exists for
Diligent's `GpuInfoPanel`, which lists DXGI adapters; it is not how linked hardware looks.

## Vulkan layer (VkLayer_DiligentGpuSim.dll)

- **Device groups.** The host physical device (by LUID) is in a group of N devices:
  `[host, node 1, ..., node N-1]`, where nodes 1..N-1 are dispatchable wrappers of the host. Every
  other physical device stays in its own group. The handles are the same in every enumeration.
  As on a real group, `vkEnumeratePhysicalDevices` lists every device of the group (the host, then
  its wrappers), `subsetAllocation` is true (configurable).
- **Identities.** Like the nodes of a Windows linked adapter, the devices share the adapter's
  `deviceLUID` and report `deviceNodeMask = 1 << k`; their `deviceUUID`s differ. Names and heap
  sizes are the host's (`--virtual-adapters` adds `[Simulated Node k]` and splits the heaps).
- **Physical-device functions.** The layer unwraps the handle in every function it implements. For
  physical-device functions it does not know (extensions newer than the layer), `vkGetInstanceProcAddr`
  and `vk_layerGetPhysicalDeviceProcAddr` return a small x64 thunk that replaces a wrapper with the
  host handle and jumps to the next layer's function.
- **Memory.** Device-local heaps report `VK_MEMORY_HEAP_MULTI_INSTANCE_BIT`, as on a real group,
  and the layer models one instance per device (see `LayerMemoryModel.hpp`): commands are recorded
  with their device mask and evaluated at submission; a read of a region whose newest write did not
  reach the reading device's instance is reported once per resource, device and kind. Peer bindings
  (`VkBind*MemoryDeviceGroupInfo`) are checked against the peer memory features, and access to a
  device without an instance (allocation device mask) is reported.
- **Presentation.** As on a linked adapter with the display on its first GPU,
  `vkGetDeviceGroupPresentCapabilitiesKHR` reports that device 0 presents, from its own instance
  (LOCAL) or any other device's (REMOTE); swapchain present modes and present masks are checked
  against that.
- **Logical device.** `vkCreateDevice` with the simulated group gives the driver an ordinary
  single-device `VkDevice` (`physicalDeviceCount` 1: a group listing one physical device twice is
  invalid). On that device the layer checks and maps:

  | Function | Checked | Passed to the driver |
  |---|---|---|
  | `vkQueueSubmit` | `VkDeviceGroupSubmitInfo` masks (valid, within the command buffer's begin mask) and semaphore device indices | masks 1, indices 0 |
  | `vkQueueSubmit2[KHR]` | `VkCommandBufferSubmitInfo::deviceMask`, `VkSemaphoreSubmitInfo::deviceIndex` | 1 / 0 |
  | `vkBeginCommandBuffer` | `VkDeviceGroupCommandBufferBeginInfo::deviceMask` | 1 |
  | `vkCmdSetDeviceMask[KHR]` | valid, within the begin mask | 1 |
  | `vkCmdBeginRenderPass[2]`, `vkCmdBeginRendering` | `VkDeviceGroupRenderPassBeginInfo` | mask 1, area of device 0 |
  | `vkAllocateMemory` | `VkMemoryAllocateFlagsInfo::deviceMask` | 1 |
  | `vkMapMemory[2]` | memory with an instance on several devices cannot be mapped (VUID-vkMapMemory-memory-00683) | unchanged |
  | `vkBindBufferMemory2`, `vkBindImageMemory2` | `deviceIndexCount` 0 or N, indices below N | no device indices |
  | `vkQueueBindSparse`, `vkQueuePresentKHR`, `vkAcquireNextImage2KHR` | device indices / masks | 0 / 1 |
  | `vkGetDeviceGroupPeerMemoryFeatures` | indices below N and different | answered by the layer (configured features for device-local heaps, all for the others) |

  A device created without the group is not changed.

Structures in the application's `pNext` chains are const and may be referenced from elsewhere, so
the layer patches them in place for the call and restores them afterwards.

## Self-tests

`tests/` runs both shims against the real driver, with the D3D12 debug layer and the Khronos
validation layer enabled: node masks of both nodes are accepted, invalid ones rejected, misuse
reported, GPU work on node 1 completes, and the Khronos layer reports nothing. They also cover the
access model (each access kind at tiers 1 and 2, objects of another node), per-node DXGI memory,
swap chain buffers, the Vulkan identities and peer features, stale reads of memory instances (a
missed copy is reported, a peer copy clears it) and presentation capabilities.

| Test | Checks (Intel Arc B580 + A380, Windows 10 22H2) |
|---|---|
| `D3D12SimTest` (tier 1) | 71, all pass |
| `D3D12SimTest_Tier2` | 72, all pass |
| `VkSimTest` | 62, all pass, 0 Khronos validation errors |

Against the first version of the shims the same tests found: struct-returning hooks that crashed
(`GetResourceAllocationInfo`, `GetCustomHeapProperties`), queues reporting node 0, pipeline-state and
root-signature node masks rejected by the runtime, devices on every adapter getting nodes, a crash at
process exit (vtables restored on freed devices); in Vulkan, adapters missing from the device groups,
changing handles, and a `vkCreateDevice` with a duplicated physical device and a duplicated `pNext`
structure (both flagged by the Khronos layer). The second round (realism) found: the D3D12 tier
reported off by one (`D3D12_CROSS_NODE_SHARING_TIER_1` is 2), views and root arguments not checked
at all, DXGI memory queries failing for node 1, swap chain buffers on no node, Vulkan group devices
missing from `vkEnumeratePhysicalDevices` and sharing one UUID and node mask, and a crash in
physical-device functions the layer did not wrap.

## Use with RenderEX

RenderEX's GPU tests have a linked mode (`--linked`): nodes 0 and 1 of one device as the two GPUs.
Under the simulator they run on one GPU:

```powershell
SimulationApp.exe --driver-shim --refresh 0 --no-log -- RenderEXGPUTests.exe --linked --backend d3d12
SimulationApp.exe --driver-shim --refresh 0 --no-log -- RenderEXGPUTests.exe --linked --backend vk
```

RenderEX registers both as CTest entries when it finds `SimulationApp.exe` (CMake cache variable
`RENDEREX_LINKED_GPU_SIM`, default `../LinkedGpuSim/build/bin/SimulationApp.exe`).

Run the D3D12 case at each tier (`--cross-node-tier 0|1|2`): at tier 0 RenderEX moves data between
the nodes through system memory, from tier 1 with cross-node copies. Without `--linked` the same
tests treat the two Vulkan group devices as two separate GPUs (they differ in `deviceNodeMask`).
Making Diligent and RenderEX pass under the realistic simulator took: identifying a GPU by LUID
*and* node mask, querying memory per node, honouring `CrossNodeSharingTier`, and node-local
dynamic upload pages in Diligent's D3D12 backend.

## GUI launcher

`SimulationLauncher.exe` is a small dialog that fills in host adapter, node count, child executable,
arguments and log file, and starts `SimulationApp.exe` in a new console. It does not offer the newer
options (`--no-validation`, `--virtual-adapters`, `--cross-node-tier`); run SimulationApp directly
for those.

## Limitations

- Windows only.
- Timing: all nodes run on one GPU; performance numbers say nothing about linked hardware.
- Vulkan: memory instances are modelled, not separate: a missed peer copy is reported but the data
  is still there. Writes through descriptors are not seen (storage resources are not modelled).
- D3D12: every descriptor in the bound tables' ranges counts as used (unbounded ranges: the first
  1024), since the shim cannot see which ones a shader reads; a bindless table that holds another
  node's resources the shader never touches is reported. Resources created before the shim was
  loaded are not checked.
- `OutputDebugString` capture needs the DBWIN objects (no debugger or DebugView holding them); the
  pipes and the validation exit code do not depend on it.
