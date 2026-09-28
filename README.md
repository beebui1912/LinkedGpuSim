# DiligentGpuSimulation

Standalone launcher that wraps a Diligent sample executable (typically
[`Tutorial31_LinkedMultiGPU`](../DiligentSamples/Tutorials/Tutorial31_LinkedMultiGPU/readme.md))
and:

- presents a **simulated linked multi-GPU device group** built from a single
  physical GPU (the same concept Tutorial31 uses via
  `EngineCreateInfo::GpuMode = GPU_MODE_LINKED` + `NodeCount / NodeMask`),
- runs the sample as a child process (`SimulationApp.exe <sample.exe>`) and
  passes through arguments,
- **captures the child's logs** (stdout, stderr, and `OutputDebugString`)
  to the console and to a UTF-8 log file for debugging,
- prints a console GPU info panel modelled after
  [`GpuInfoPanel.cpp`](../DiligentSamples/Tutorials/Common/src/GpuInfoPanel.cpp),
  showing "Simulation GPU (linked device group)" with per-node VRAM shares
  and per-process GPU utilization.

This module sits **at the same level as `DiligentFX`** in the DiligentEngine
tree and does not modify any existing DiligentEngine source. It builds
standalone via its own `CMakeLists.txt`.

> **Platform:** Windows only.  Requires DXGI 1.4 (Windows 10+) and PDH.

## Contents

```
DiligentGpuSimulation/
├── CMakeLists.txt              Standalone Windows build (fetches MinHook)
├── readme.md                   (this file)
├── src/                        SimulationApp.exe (console launcher)
│   ├── main.cpp                     Entry + CommandLineToArgvW
│   ├── SimulationApp.hpp/.cpp       Orchestrator + CLI parser
│   ├── ProcessLauncher.hpp/.cpp     CreateProcessW + pipes + env
│   ├── ShimInjector.hpp/.cpp        CreateRemoteThread(LoadLibraryW)
│   ├── LogCapture.hpp/.cpp          LogSink + PipeReader + DBWIN
│   ├── LinkedGpuSimulator.hpp/.cpp  DXGI + PDH host GPU enumeration
│   └── GpuInfoConsole.hpp/.cpp      Text panel formatter
├── shim/                       D3D12Sim.dll (injected shim)
│   ├── CMakeLists.txt
│   └── src/
│       ├── DllMain.cpp                    installs hooks at PROCESS_ATTACH
│       ├── D3D12Hook.hpp/.cpp             MinHook on D3D12CreateDevice
│       ├── D3D12DeviceWrapper.hpp/.cpp    per-orig-vtable shadow + 10 stubs
│       ├── D3D12Vtable.hpp                slot indices
│       ├── DXGIHook.hpp/.cpp              MinHook on CreateDXGIFactory*
│       ├── DXGIFactoryWrapper.hpp/.cpp    per-orig-vtable shadow +
│       │                                  EnumAdapters1 / EnumAdapters
│       ├── VirtualDXGIAdapter.hpp/.cpp    proxy IDXGIAdapter3 COM object
│       ├── ShimConfig.hpp/.cpp            env-driven config
│       └── ShimLog.hpp/.cpp               console + file + OutputDebugString
├── shim_vk/                    VkLayer_DiligentGpuSim.dll (full Vulkan layer)
│   ├── CMakeLists.txt
│   ├── manifest/VkLayer_DiligentGpuSim.json.in
│   └── src/
│       ├── LayerMain.cpp                 layer + all vkGetPhysicalDevice* intercepts
│       ├── LayerDispatch.hpp             per-instance / per-device tables
│       ├── WrappedPhysicalDevice.hpp/.cpp dispatchable-handle wrapper + registry
│       ├── LayerLog.hpp/.cpp             matches shim-side logger
│       └── VkLayer_DiligentGpuSim.def    DLL export list
└── launcher/                   SimulationLauncher.exe (Win32 dialog)
    ├── CMakeLists.txt
    └── src/
        ├── WinMain.cpp
        ├── LauncherDialog.hpp/.cpp
        ├── LauncherDialog.rc
        └── resource.h
```

## Build

Configure from anywhere:

```powershell
cmake -S DiligentGpuSimulation -B DiligentGpuSimulation/build
cmake --build DiligentGpuSimulation/build --config Release
```

The binary lands at `DiligentGpuSimulation/build/bin/SimulationApp.exe`
(or `SimulationAppd.exe` for a Debug build).

You may also integrate the module into a parent CMake project (optional -
no changes required to the existing DiligentEngine CMake):

```cmake
add_subdirectory(DiligentGpuSimulation)
```

## Usage

```
SimulationApp.exe [options] [--] <child.exe> [child-args...]
```

Common invocations:

```powershell
# Launch Tutorial31 with a 2-node simulated linked group.
SimulationApp.exe Tutorial31_LinkedMultiGPU.exe

# Full path + a 4-node simulation + custom log file.
SimulationApp.exe -n 4 -l C:\logs\t31.log ^
    ..\build\DiligentSamples\Tutorials\Tutorial31_LinkedMultiGPU\Release\Tutorial31_LinkedMultiGPU.exe

# Just print the panel; useful to inspect the host GPU without running anything.
SimulationApp.exe --info-only

# Pass arguments through to the sample (everything after -- is forwarded).
SimulationApp.exe -n 2 -- Tutorial31_LinkedMultiGPU.exe --mode D3D12
```

### Options

| Option | Description |
|---|---|
| `-n`, `--nodes N` | Number of simulated linked-GPU nodes (1..8). Default: 2. |
| `-a`, `--adapter INDEX` | Preferred host DXGI adapter index. Default: first non-software. |
| `-l`, `--log PATH` | Log file path (UTF-8, BOM-prefixed). Default: `<exe-dir>\SimulationApp.log`. |
| `--no-log` | Disable file logging (console only). |
| `--no-debug-capture` | Do not capture `OutputDebugString` output (DebugView-style). |
| `--info-only` | Print the info panel and exit; no child process. |
| `--refresh SEC` | Interval between periodic GPU stat snapshots written to the log (0 = disabled). Default: 2.0. |
| `-h`, `--help` | Print help. |

### What the child sees

`SimulationApp` sets the following environment variables in the child. Existing
samples ignore them; new code (e.g. a future opt-in adapter in the sample
framework) can read them to *behave as if* the adapter reported multiple linked
nodes.

| Variable | Meaning |
|---|---|
| `DILIGENT_SIM_LINKED_NODE_COUNT` | Number of simulated linked nodes (1..8). |
| `DILIGENT_SIM_LINKED_NODE_MASK`  | Hex bitmask of the simulated nodes, e.g. `0x00000003`. |
| `DILIGENT_SIM_HOST_ADAPTER`      | Name of the physical adapter chosen as the host. |
| `DILIGENT_SIM_HOST_ADAPTER_LUID` | Host adapter LUID as `HIGH_LOW` hex. |
| `DILIGENT_SIM_LOG_FILE`          | Shared log file path (child can append too). |
| `DILIGENT_SIM_PARENT_PID`        | PID of the SimulationApp parent. |

## How the "simulated linked device group" is built

1. `LinkedGpuSimulator` enumerates all physical adapters through
   `IDXGIFactory4::EnumAdapters1` and keeps an `IDXGIAdapter3` reference to each
   so it can query per-process VRAM usage via `QueryVideoMemoryInfo` on the
   `DXGI_MEMORY_SEGMENT_GROUP_LOCAL` segment.
2. It sets up a PDH query on the `\GPU Engine(*)\Utilization Percentage`
   counter set, then filters instance names of the form
   `pid_1234_luid_0xHIGH_0xLOW_...` to compute the child process's GPU
   utilization per adapter LUID.
3. It picks a primary host adapter (the first non-software one, unless
   `--adapter` overrides), then constructs a `SimGpuGroup` where:
   - `NodeCount = N`
   - `NodeMask  = (1 << N) - 1`
   - Each virtual node reports `TotalVRAM/N` and its share of the live usage;
     the last node absorbs any rounding remainder so the sum still equals the
     host adapter's real values.
4. `GpuInfoConsole` renders that group as text (the launcher's equivalent of
   the ImGui `GpuInfoPanel`).

This is exactly the shape `Tutorial31_LinkedMultiGPU` expects from
`GraphicsAdapterInfo::NodeCount / NodeMask` when running on real
LDA / device-group hardware, which is why the panel format matches.

## How child logs get captured

`SimulationApp` reads two channels of the child in parallel, tees each into a
shared `LogSink`, and writes every line to the console and to the log file
with a wall-clock timestamp and a source tag:

- **Stdout / stderr pipes** — the child is launched with `STARTUPINFO`
  redirection, so anything it writes to `stdout` or `stderr` shows up
  immediately in the launcher.
- **`OutputDebugString`** — a DebugView-style consumer opens the standard
  `DBWIN_BUFFER` shared memory and the `DBWIN_BUFFER_READY` /
  `DBWIN_DATA_READY` events, filters messages by the child PID, and
  forwards them to the same sink.  If another debugger is already
  attached (for example, when running the child under a Visual Studio
  debug session), this reader skips itself and warns in the log.

The log file is opened with a UTF-8 BOM and shared-for-read, so it can be
tailed while the child is still running.

## Example log line format

```
[14:07:22.318] [child.out] Tutorial31: adapter 'NVIDIA GeForce RTX 4080' ...
[14:07:22.401] [child.dbg] Diligent Engine: D3D12 device created successfully
[14:07:24.001] === GPU stats snapshot (child PID 12984) ===
============================================================
 Simulation GPU (linked device group)
============================================================
 Host adapter : NVIDIA GeForce RTX 4080
 ...
```

## GUI launcher (SimulationLauncher.exe)

If typing command lines isn't your thing, `SimulationLauncher.exe` (built next
to `SimulationApp.exe`) is a small Win32 dialog that populates a couple of
fields and spawns `SimulationApp.exe` in a new console window:

- **Host GPU** - dropdown populated from `IDXGIFactory1::EnumAdapters1`
  (each entry shows the DXGI description, dedicated VRAM in MB and
  Discrete/Integrated/Software).  Its selection maps to `-a <index>`.
- **Simulated linked nodes** - dropdown 1..8, maps to `-n <count>`.
- **Child executable** - text box + Browse button (backed by
  `IFileOpenDialog`).  Filter defaults to `*.exe`.
- **Arguments passed to the child** - free-form (default `--mode D3D12` so
  Diligent samples take the D3D12 backend).
- **Log file** - empty leaves it at `SimulationApp`'s default.
- **Inject driver-level shim** - toggles `--driver-shim`.
- **Capture OutputDebugString** - toggles `--no-debug-capture` (inverted).
- **Verbose shim tracing** - sets `DILIGENT_SIM_VERBOSE=1` in the child's env.

Clicking **Launch** spawns `SimulationApp.exe` with those args via
`CreateProcess(..., CREATE_NEW_CONSOLE, ...)` so the user sees the log
window pop up alongside the child.  The dialog stays open so multiple
runs can be launched back-to-back.

## Driver-level shim (--driver-shim, D3D12 only)

`SimulationApp.exe --driver-shim <child.exe>` goes a step further than the
console panel: it injects **`D3D12Sim.dll`** (built next to the launcher) into
the child at `CREATE_SUSPENDED` time via `CreateRemoteThread(LoadLibraryW)`,
and inside the child that DLL:

1. Uses [MinHook](https://github.com/TsudaKageyu/minhook) to install an inline
   hook on `d3d12!D3D12CreateDevice`.  Every device that D3D12 returns to the
   caller (Diligent creates one per enumerated adapter plus one for the chosen
   adapter, and the D3D12 debug layer may re-wrap the final one with its own
   vtable) is intercepted.
2. Builds a per-orig-vtable **shadow vtable** - a heap-allocated copy of the
   real vtable with a small set of slots replaced by our stubs.  The shadow's
   `-1` header stores the original vtable so the stubs can call through it
   without a map lookup.  The device's own vtable pointer is swapped in place
   to point at the shadow.  Multiple devices sharing the same original vtable
   share one shadow.
3. The replaced slots:

   | Slot | Method | What the stub does |
   |---|---|---|
   |  7 | `GetNodeCount`             | Returns `DILIGENT_SIM_LINKED_NODE_COUNT` (default 2). |
   |  8 | `CreateCommandQueue`       | `desc.NodeMask` any-nonzero -> 1 |
   | 12 | `CreateCommandList`        | `nodeMask` any-nonzero -> 1 |
   | 14 | `CreateDescriptorHeap`     | `desc.NodeMask` any-nonzero -> 1 |
   | 25 | `GetResourceAllocationInfo`| `visibleMask` any-nonzero -> 1 |
   | 26 | `GetCustomHeapProperties`  | `nodeMask` any-nonzero -> 1; returned struct re-exposes the requested node bits |
   | 27 | `CreateCommittedResource`  | `HeapProps.CreationNodeMask` / `VisibleNodeMask` any-nonzero -> 1 |
   | 28 | `CreateHeap`               | `desc.Properties.CreationNodeMask` / `VisibleNodeMask` any-nonzero -> 1 |
   | 39 | `CreateQueryHeap`          | `desc.NodeMask` any-nonzero -> 1 |
   | 41 | `CreateCommandSignature`   | `desc.NodeMask` any-nonzero -> 1 |

The net effect on Tutorial31 running in D3D12 mode:

- `EngineFactoryD3D12` fills `GraphicsAdapterInfo::NodeCount = 2` and
  `NodeMask = 0x3` from the spoofed `GetNodeCount()`.
- Tutorial31's `if (AdapterInfo.NodeCount <= 1) return;` early-out no longer
  fires, so it takes the **real** linked path: 2 immediate contexts, per-node
  render targets, per-node fences, split/compose rendering.
- Every resource Diligent creates with `CreationNodeMask = 0x2` (bit for the
  simulated node 1) is silently downgraded to `0x1` before it reaches the
  driver.  On a single-node physical GPU the D3D12 runtime therefore accepts
  every call.

Verified with `Tutorial31_LinkedMultiGPU.exe --mode D3D12` on an Intel iGPU:
5 devices wrapped through 2 distinct vtables (base + debug layer),
26 `CreateCommittedResource` remaps, 2 `CreateDescriptorHeap` remaps,
1 `CreateCommandQueue` remap, zero D3D12 errors, zero Diligent errors.

### Utilization display: virtual nodes vs the real primary

`GpuInfoPanel.cpp` matches PDH GPU-Engine samples to adapter entries by
LUID and stops at the first match.  If virtual nodes shared the primary's
LUID, all of the process's GPU utilization would be attributed to virtual
node 0 (the first entry seen), leaving virtual node 1 and the real primary
reading 0% - a misleading display.

To avoid this, each `VirtualDXGIAdapter` reports a synthetic LUID:

```cpp
AdapterLuid.HighPart = 0xFEEDFACE
AdapterLuid.LowPart  = 0xC0DE0000 | NodeIndex
```

Real Windows adapter LUIDs are tiny integers (HighPart is almost always
0, LowPart is a few thousand), so the `0xFEEDFACE` sentinel never
collides with a real PDH counter LUID.  The result:

- Real primary entry shows the process's actual GPU utilization.
- Virtual node entries stay at 0% (they never match a PDH sample).

Diligent's own internals are unaffected: it derives the runtime adapter
LUID from `ID3D12Device::GetAdapterLuid()` (which we don't intercept), so
`EnumAdapterByLuid` still finds the real hardware adapter.

Displaying proportional per-virtual-node utilization would require a
separate PDH `pdh!PdhGetFormattedCounterArrayW` hook to rewrite counter
instance names on the fly; that's a follow-up (see PDH-hook TODO in the
shim source).

### DXGI shim - so GpuInfoPanel sees the simulated group too

`GpuInfoPanel.cpp` (in DiligentSamples/Tutorials/Common) enumerates adapters
directly via **DXGI**, not through Diligent's engine, so the D3D12-only shim
above leaves it seeing the raw physical adapter list.  To make the panel
inside the child render the simulated linked group without touching a line of
`GpuInfoPanel.cpp`, `D3D12Sim.dll` additionally hooks:

- `dxgi!CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2` -
  every returned factory has its vtable patched.
- `IDXGIFactory1::EnumAdapters1` (slot 12) and `IDXGIFactory::EnumAdapters`
  (slot 7) - injected with a hook that returns:

  | Index          | Adapter                                             |
  |----------------|-----------------------------------------------------|
  | `0 .. N-1`     | `VirtualDXGIAdapter` proxies over the primary GPU   |
  | `N ..`         | The real DXGI adapter list, starting from index 0   |

Each `VirtualDXGIAdapter` is a fresh COM object implementing
`IDXGIAdapter3` (via C++ inheritance).  Every method delegates to the real
primary adapter *except*:

- `GetDesc` / `GetDesc1` / `GetDesc2`: appends `[Simulated Node k]` to
  `Description` and divides `DedicatedVideoMemory`,
  `DedicatedSystemMemory`, `SharedSystemMemory` by `N` (last node absorbs
  rounding remainder).
- `QueryVideoMemoryInfo`: divides `Budget`, `CurrentUsage`,
  `AvailableForReservation` and `CurrentReservation` by `N`.
- `SetVideoMemoryReservation`: rewrites `NodeIndex` to 0.

The adapter LUID is deliberately left untouched so `GpuInfoPanel`'s
per-process GPU utilization query (which matches PDH "GPU Engine"
instances by LUID) still finds the virtual nodes.

To keep D3D12 device creation happy, virtual proxies also implement a
private `QueryInterface` case (see `VirtualDXGIAdapter::IID_RevealReal`)
that returns the underlying real `IDXGIAdapter1*`.  The
`D3D12CreateDevice` hook probes for this IID and quietly substitutes the
real adapter before calling the driver, so:

- `GetHardwareAdapter` inside DiligentCore probes a virtual node -> real
  probe succeeds via the substitution -> real device is created.
- `GpuInfoPanel` still sees N distinct virtual adapters in its list.

The real primary adapter is intentionally re-exposed at index `N` in the
enum list as a safety fallback: if `D3D12CreateDevice` probing on virtual
proxies ever fails, Diligent's loop finds the real primary and proceeds
normally.  The visible cost is that the physical GPU appears twice in
`GpuInfoPanel` (once as each simulated node, once as itself) - live-tested
this cosmetic duplication is preferable to breaking `CreateCommandQueue`
which we observed when the real primary was hidden.

Live snapshot from Tutorial31 running under `SimulationApp --driver-shim`:

```
[shim] Built DXGI factory shadow #1 for orig vtable ...  (128 slots copied)
[shim] EnumAdapters1(0) -> virtual node 0
[shim] EnumAdapters1(1) -> virtual node 1
[shim] EnumAdapters1(2) -> real adapter 0
[shim] EnumAdapters1(3) -> real adapter 1
[shim] D3D12CreateDevice: unwrapping virtual proxy ... -> real adapter ...
[shim] GetNodeCount -> 2 (simulated)
[shim] CreateCommittedResource: Creation 0x2 -> 0x1, Visible 0x3 -> 0x1
```

### Environment variables the shim reads

The shim honours the same `DILIGENT_SIM_*` variables SimulationApp exports,
plus:

| Variable | Meaning |
|---|---|
| `DILIGENT_SIM_LINKED_NODE_COUNT`  | Simulated NodeCount (1..8). |
| `DILIGENT_SIM_LINKED_NODE_MASK`   | Simulated NodeMask (defaults to `(1<<N)-1`). |
| `DILIGENT_SIM_LOG_FILE`           | Shared log file; the shim appends here too. |
| `DILIGENT_SIM_VERBOSE`            | `1` = log every remap (useful for verification). |

## Vulkan parity with D3D12 - the Diligent-side patch

Vulkan and D3D12 initially behaved differently under the shim because their
Diligent factories filled `AdapterInfo.NodeCount` at different points in the
lifecycle:

- **D3D12** (`EngineFactoryD3D12.cpp:771`) queries
  `d3d12Device->GetNodeCount()` **inside** `GetGraphicsAdapterInfo`, so
  Tutorial31's `ModifyEngineInitInfo` sees `NodeCount = 2` upfront.
- **Vulkan** (`EngineFactoryVk.cpp:611`, pre-patch) hardcoded
  `AdapterInfo.NodeCount = 1;` and deferred the real query to
  `CreateDeviceAndContextsVk` - after Tutorial31 had already given up.

That asymmetry is now patched inside `DiligentCore/Graphics/GraphicsEngineVulkan/src/EngineFactoryVk.cpp`.
`GetPhysicalDeviceGraphicsAdapterInfo` gained an optional
`const VulkanUtilities::Instance*` parameter and now:

1. Queries `vkEnumeratePhysicalDeviceGroups` when an instance is available
   (Volk-enabled builds).
2. If this adapter is a member of a group with `physicalDeviceCount > 1`,
   sets `NodeCount` and `NodeMask` accordingly.
3. Bumps every queue family's `MaxDeviceContexts` up to `physicalDeviceCount`
   so samples that request one immediate context per linked node (which
   Tutorial31 does) pass `VerifyEngineCreateInfo`.

Both call sites - `EnumerateAdapters` and `CreateDeviceAndContextsVk` - pass
`Instance.get()`, so the same code path fires for the initial adapter
enumeration and for the subsequent validation inside device creation.

### End-to-end proof

```
[shim-vk] vkEnumeratePhysicalDeviceGroups: synthesized 1 group with 2 physical devices
         (primary=0x..., 1 wrapped nodes).
Diligent Engine: Info: Vulkan adapter 'Intel(R) Graphics' belongs to a linked device group
         with 2 physical devices.
Diligent Engine: Info: Tutorial31: adapter 'Intel(R) Graphics' exposes 2 linked GPU nodes.
         Enabling Linked multi-GPU with 2 views.
[shim-vk] vkCreateDevice: rewrote VkDeviceGroupDeviceCreateInfo
         (physicalDeviceCount=2, wrappedEntries=1).
[shim-vk] vkCreateDevice: device 0x... bound (real physical 0x...).
Tutorial31: Linked Multi-GPU (Vulkan, API 256020) - 4.3 ms (231.0 fps)  ← window title
```

Zero D3D12 errors.  Zero Vulkan validation errors.  Zero Diligent errors.
Tutorial31 renders two split strips in Vulkan mode just like it does in
D3D12.

### How the shim helps the fix work

The Vulkan layer synthesizes each linked group as `[real_primary, wrapper_1, ..., wrapper_{N-1}]`
- index `0` is the real handle so `Diligent`'s pointer-equality check inside
`EnumerateAdapters` finds the adapter in the group, and indices `1..N-1` are
dispatchable wrappers so the Vulkan spec's "distinct handles in a group"
rule is satisfied.  `Layer_vkCreateDevice` then unwraps any wrapper it sees
in `VkDeviceGroupDeviceCreateInfo::pPhysicalDevices` back to the real handle
before calling the ICD.

## Vulkan layer (VkLayer_DiligentGpuSim.dll) - Phase 4

Vulkan doesn't have an equivalent of `D3D12CreateDevice` you can inline-hook:
the linked-adapter concept lives in `vkEnumeratePhysicalDeviceGroups` +
`VkDeviceGroupDeviceCreateInfo`, and physical-device handles are validated
by the ICD when `vkCreateDevice` is called.  The clean way to simulate a
Vulkan device group is a **Vulkan layer** (`VK_LAYER_*`).

The shim ships a working layer skeleton with:

- `VkLayer_DiligentGpuSim.dll` built via the standard layer interface
  (`vkNegotiateLoaderLayerInterfaceVersion` + `vk_layerGetInstanceProcAddr`
  + `vk_layerGetDeviceProcAddr`).
- Full per-instance and per-device dispatch tables populated by chaining
  through the loader (`VkLayerInstanceCreateInfo` / `VkLayerDeviceCreateInfo`
  `pfnNextGetInstanceProcAddr` and friends).
- `VkLayer_DiligentGpuSim.json` manifest generated by CMake next to the DLL.
- **Automatic registration** by SimulationApp: when `--driver-shim` is
  enabled, SimulationApp sets these environment variables in the child:

  ```
  VK_LAYER_PATH      = <bin dir containing VkLayer_DiligentGpuSim.dll + .json>
  VK_INSTANCE_LAYERS = VK_LAYER_DiligentGraphics_LinkedGpuSim
  ```

  The Vulkan loader inside the child then discovers the manifest, loads the
  DLL, and inserts the layer into the instance chain automatically.
- Pass-through intercepts for `vkCreateInstance` / `vkDestroyInstance` /
  `vkCreateDevice` / `vkDestroyDevice` / `vkGetInstanceProcAddr` /
  `vkGetDeviceProcAddr` / `vkEnumeratePhysicalDeviceGroups[KHR]`.
  These forward to the next layer in the chain unchanged today, so a
  Vulkan-mode Tutorial31 keeps running normally *and* the layer's chain
  plumbing is verified end-to-end.

### What the layer actually does now

- **Dispatchable-handle wrapping** via `WrappedPhysicalDevice`: allocates a
  small struct whose first machine word is a copy of the real device's
  dispatch key, followed by `Real`, `ParentInstance`, `NodeIndex` and
  `NodeCount`.  The loader still dispatches correctly on the wrapper; we
  intercept every function that takes a `VkPhysicalDevice` and unwrap
  before chaining down.
- **`vkEnumeratePhysicalDeviceGroups[KHR]` synthesis**: returns one group
  of `DILIGENT_SIM_LINKED_NODE_COUNT` wrapped copies of the primary real
  physical device.
- **`vkCreateDevice` unwrap**: rewrites the outer `VkPhysicalDevice`
  argument and walks `VkDeviceGroupDeviceCreateInfo::pPhysicalDevices` in
  the `pNext` chain to substitute the underlying real handle before
  chaining down.
- **`vkGetPhysicalDeviceProperties[2/2KHR]`**: unwrap, chain, then append
  ` [Simulated Node k]` to `VkPhysicalDeviceProperties::deviceName`.
- **`vkGetPhysicalDeviceMemoryProperties[2/2KHR]`**: unwrap, chain, then
  divide every `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT` heap's `size` by
  `NodeCount`.
- **Full unwrap-and-chain pass-throughs** for every other function that
  takes a `VkPhysicalDevice`: Features/2/2KHR,
  QueueFamilyProperties/2/2KHR, FormatProperties/2/2KHR,
  ImageFormatProperties/2/2KHR, SparseImageFormatProperties/2/2KHR,
  ExternalBuffer/Fence/SemaphoreProperties/KHR,
  EnumerateDeviceExtensionProperties, EnumerateDeviceLayerProperties, plus
  all of `VK_KHR_surface`.

**Independently verified with `vulkaninfo`**:

```
VK_LAYER_DiligentGraphics_LinkedGpuSim  version 1.3.0
Device Groups:
    Intel(R) Graphics [Simulated Node 0] (ID: 0)
    Intel(R) Graphics [Simulated Node 1] (ID: 1)
```

The layer is exercised end-to-end for any Vulkan tool that queries device
groups.  The only thing keeping Tutorial31's Vulkan mode single-cube is
the Diligent-side bootstrap-order issue described in the section above.

## Requirements & caveats

- **D3D12** backend: fully working driver-level simulation.  Force it on
  Diligent samples with `<sample>.exe --mode D3D12`.
- **Vulkan** backend: full linked-group synthesis works and Tutorial31 now
  renders two strips end-to-end.  Requires the Diligent-side patch to
  `DiligentCore/Graphics/GraphicsEngineVulkan/src/EngineFactoryVk.cpp` that
  passes the `VulkanUtilities::Instance` through to
  `GetPhysicalDeviceGraphicsAdapterInfo` and queries
  `vkEnumeratePhysicalDeviceGroups` there (see the "Vulkan parity with
  D3D12 - the Diligent-side patch" section above).
- **Release configuration of the sample is recommended.**  Diligent's own
  `DEV_CHECK_ERR(PSODesc.ImmediateContextMask & (1 << CtxId))` assertion fires
  in Debug builds when a sample creates a PSO with the default
  `ImmediateContextMask = 1` and Diligent tries to use it on the node-1
  context.  In Release builds `DEV_CHECK_ERR` is a no-op and rendering
  proceeds.  Tutorial31 does not set `ImmediateContextMask` explicitly on its
  PSOs; that omission is the sample's own responsibility on real linked
  hardware and this shim does not paper over it.
- **Vulkan support is Phase 2.**  Vulkan device groups need a
  `VK_LAYER_*` ICD-level interception which is not part of this iteration.
- The shim currently patches a fixed slot set on the base `ID3D12Device`
  vtable.  Method-added-later interfaces (`ID3D12Device4::CreateCommittedResource1`
  etc.) are only used by Diligent in code paths Tutorial31 does not exercise,
  so they are not intercepted for now.

## Limitations

- Windows only.  The DXGI/PDH/DBWIN implementations are Win32-specific.
- `OutputDebugString` capture requires no other debugger currently owns the
  DBWIN objects (e.g. no Visual Studio debug session on the child, no running
  DebugView instance).  When it can't be acquired, the pipe capture continues
  to work and the launcher just logs a note.
