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
| Topology | The host adapter reports N nodes (`ID3D12Device::GetNodeCount`, one Vulkan device group of N devices). Other adapters keep one node. | — |
| Node masks (D3D12) | Every API that takes a node mask accepts the N nodes, applies the D3D12 rules, and passes node 0 to the driver. Queues report the node they were created for. | — |
| Device masks (Vulkan) | Every structure with a device mask or device index is checked against the group and mapped to the one real device. | — |
| Execution | Work on each node runs on the real GPU, on its own queue. Cross-node fences and semaphores work. | Parallelism: all nodes share one GPU, so timings show no speed-up. |
| Memory | D3D12: one memory, as on real hardware (a resource lives on its creation node and is read remotely by visible nodes). | Bandwidth of the node link. Vulkan: real groups give each device its **own instance** of device-local memory; here all devices share one, so data written on one device is visible on another without peer binding. Use the D3D12 path or real hardware to verify data flow between Vulkan devices. |
| Checks | Node masks at creation (rejected with `E_INVALIDARG`, as the runtime does); copies of resources not visible to the list's node; command lists on another node's queue; Vulkan device masks, device indices, peer-memory queries, mapping multi-instance memory. | Resource use through views (SRV/RTV/UAV) in draws and dispatches is not checked for node visibility. |

## Contents

```
LinkedGpuSim/
├── CMakeLists.txt              Standalone build (fetches MinHook)
├── src/                        SimulationApp.exe: launcher, log capture, GPU info panel
├── shim/                       D3D12Sim.dll: D3D12 shim injected into the child
│   └── src/
│       ├── D3D12Hook                 MinHook on D3D12CreateDevice; wraps devices on the host adapter
│       ├── D3D12DeviceWrapper        ID3D12Device..10: node count, node masks of every Create* method
│       ├── D3D12CommandWrappers      queues (GetDesc, ExecuteCommandLists) and command lists (copies)
│       ├── NodeMasks                 the D3D12 node-mask rules, validation reporting, per-object masks
│       ├── VtableShadow              per-class vtable copies with patched slots
│       ├── D3D12Vtable[Check]        slot indices, verified against the Windows SDK at compile time
│       ├── DXGIHook, DXGIFactoryWrapper, VirtualDXGIAdapter   optional virtual adapters (see below)
│       └── ShimConfig, ShimLog
├── shim_vk/                    VkLayer_DiligentGpuSim.dll: Vulkan layer
│   └── src/
│       ├── LayerMain                 instance level: device groups, host by LUID, vkCreateDevice
│       ├── LayerDevice               device level: device masks and indices, memory mapping checks
│       ├── LayerConfig               settings, validation reporting
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

A rejected call returns `E_INVALIDARG`, as the runtime does on linked hardware, and is reported.

**Queues** report the node mask they were created with from `GetDesc()` (applications such as
Diligent derive a context's node from it). `ExecuteCommandLists` reports command lists of another node.
**Command lists** report `CopyBufferRegion`, `CopyTextureRegion` and `CopyResource` that touch a
resource not visible to the list's node.

Simulated masks are stored in the objects' private data, so an object allocated at the address of a
released one starts clean. A device created again at a released device's address is wrapped again.

**Virtual DXGI adapters** (`--virtual-adapters`, off by default): `EnumAdapters[1]` lists one proxy
adapter per node (description `[Simulated Node k]`, memory divided by N) before the real adapters,
and `D3D12CreateDevice` on a proxy creates the device on the real adapter. This exists for
Diligent's `GpuInfoPanel`, which lists DXGI adapters; it is not how linked hardware looks.

## Vulkan layer (VkLayer_DiligentGpuSim.dll)

- **Device groups.** The host physical device (by LUID) is in a group of N devices:
  `[host, node 1, ..., node N-1]`, where nodes 1..N-1 are dispatchable wrappers of the host. Every
  other physical device stays in its own group. The handles are the same in every enumeration.
  Every physical-device function unwraps the handle; properties get `[Simulated Node k]`.
- **Memory.** Device-local heaps report `VK_MEMORY_HEAP_MULTI_INSTANCE_BIT`, as on a real group.
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
  | `vkGetDeviceGroupPeerMemoryFeatures` | indices below N and different | answered by the layer (all features) |

  A device created without the group is not changed.

Structures in the application's `pNext` chains are const and may be referenced from elsewhere, so
the layer patches them in place for the call and restores them afterwards.

## Self-tests

`tests/` runs both shims against the real driver, with the D3D12 debug layer and the Khronos
validation layer enabled: node masks of both nodes are accepted, invalid ones rejected, misuse
reported, GPU work on node 1 completes, and the Khronos layer reports nothing.

| Test | Checks (Intel Arc B580 + A380, Windows 10 22H2) |
|---|---|
| `D3D12SimTest` | 42, all pass |
| `VkSimTest` | 39, all pass, 0 Khronos validation errors |

Against the first version of the shims the same tests found: struct-returning hooks that crashed
(`GetResourceAllocationInfo`, `GetCustomHeapProperties`), queues reporting node 0, pipeline-state and
root-signature node masks rejected by the runtime, devices on every adapter getting nodes, a crash at
process exit (vtables restored on freed devices); in Vulkan, adapters missing from the device groups,
changing handles, and a `vkCreateDevice` with a duplicated physical device and a duplicated `pNext`
structure (both flagged by the Khronos layer).

## Use with RenderEX

RenderEX's GPU tests have a linked mode (`--linked`): nodes 0 and 1 of one device as the two GPUs.
Under the simulator they run on one GPU:

```powershell
SimulationApp.exe --driver-shim --refresh 0 --no-log -- RenderEXGPUTests.exe --linked --backend d3d12
SimulationApp.exe --driver-shim --refresh 0 --no-log -- RenderEXGPUTests.exe --linked --backend vk
```

RenderEX registers both as CTest entries when it finds `SimulationApp.exe` (CMake cache variable
`RENDEREX_LINKED_GPU_SIM`, default `../LinkedGpuSim/build/bin/SimulationApp.exe`).

## GUI launcher

`SimulationLauncher.exe` is a small dialog that fills in host adapter, node count, child executable,
arguments and log file, and starts `SimulationApp.exe` in a new console. It does not offer the newer
options (`--no-validation`, `--virtual-adapters`, `--cross-node-tier`); add them to the arguments
field's command line by running SimulationApp directly.

## Limitations

- Windows only.
- Timing: all nodes run on one GPU; performance numbers say nothing about linked hardware.
- Vulkan: one memory instance for all devices of the group (see the table at the top).
- D3D12: node visibility is checked for copies only, not for views used in draws and dispatches.
- `OutputDebugString` capture needs the DBWIN objects (no debugger or DebugView holding them); the
  pipes and the validation exit code do not depend on it.
