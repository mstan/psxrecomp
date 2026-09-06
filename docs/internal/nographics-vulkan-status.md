# NoGraphicsAPI / Vulkan investigation — 2026-09-06

**Status: partial implementation; full NoGraphicsAPI renderer blocked and not
delivered.** NoGraphicsAPI builds, but cannot create a device on either installed
GPU. Existing Vulkan correctness defects were fixed and reproducible renderer
measurements were collected. No NoGraphicsAPI performance gain was measured.

Worktree: `F:/Projects/psxrecomp/_wt-nographics-vulkan`, branch
`feat/nographics-vulkan`, created from freshly fetched `origin/master`
`b213858c4cd286f15bbe80bfbc36cf90764b442f`. Owning issue: `beads-eio.3.119`.
Three GPT-5.5 subagents audited compatibility, renderer coverage and benchmarking;
the orchestrator reviewed their changes and independently built and ran the
probes. Initial benchmark assumptions and test defects were rejected/corrected.

## NoGraphicsAPI feasibility

[Upstream NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI), pinned at
`469ebba558ee46f93c73a9f285979aa00fce7500`, requires Vulkan 1.4 plus descriptor
heaps, device-address commands, untyped shader pointers and mesh shaders. It
also imposes feature/memory requirements checked by its real device-creation
path. The wrapper simplifies host API work; it does not provide PSX rasterization,
VRAM feedback, blending/mask semantics or game integration.

The opt-in [device probe](../../tools/nographics_probe/README.md) builds the
actual upstream CMake target. Native VS2022/MSVC x64 build passed using
Vulkan-Headers v1.4.357 (`e3b1eec08173d6b825cd3ac88c885a63b621504a`) and the
installed Vulkan SDK 1.4.341.1 import library. The older installed headers lack
the required declarations. Upstream rejects the MinGW compiler used by the
current psxrecomp runtime toolchain; a supported C++ build/ABI boundary would
also need integration before adding it to that runtime.

Actual local extension enumeration and upstream `gpu::create_device({})`:

| Requirement | RTX 3080 Ti (NVIDIA 610.74) | AMD Radeon integrated GPU |
|---|---|---|
| Vulkan API | 1.4.341 | 1.4.315 |
| `VK_EXT_descriptor_heap` | yes | no |
| `VK_KHR_device_address_commands` | **no** | **no** |
| `VK_KHR_shader_untyped_pointers` | yes | no |
| `VK_EXT_mesh_shader` | yes | yes |

Loader API: 1.4.341. Result: `create_device.error=unsupported`, exit 77; CTest
correctly reports **skipped**, not a successful device test. A separate direct
Vulkan enumeration confirmed the same matrix. Installing newer headers does
not add missing driver extensions. No system driver was changed.

The probe also compiles an experimental texture upload/readback round trip for
a future compatible device. That path has **not executed here**. It transports
raw packed bytes; it does not implement PSX texture interpretation or rendering.

## Changes to the existing Vulkan backend

Master's status comment was stale: geometry, textures/CLUT, mask bits, blending,
supersampling and wide composition already had implementations. This change:

- Adds the missing full wide-surface readback hook, with strict capacity checks.
- Refreshes the wide center from canonical VRAM before wide presentation or
  readback, so CPU uploads/copies reach both vertical framebuffer bands while
  retaining the revealed margins.
- Adds transfer-destination usage to shared staging buffers used for readback.
- Adds transfer-destination usage for the initial stencil clear and transitions
  both depth/stencil aspects when separate layouts are not enabled.

The latter fixes follow Vulkan's [copy destination usage requirement](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html)
and [combined depth/stencil barrier rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html).
NoGraphicsAPI is not selected by the runtime; these are fixes to classic Vulkan.

## Validation

Release runtime build passed (Vulkan enabled, UI/netplay/rewind off, no-BIOS setup
host). Nine focused existing GPU/presentation tests passed. The new opt-in
[renderer probe](../../runtime/tests/renderer_probe/README.md) passed wide
dimensions/pitch and Vulkan upload/coherence/capacity checks at 1x, 2x and 4x.
Core and synchronization validation produced no errors in those Vulkan runs.

The original Vulkan source in the same harness fails the wide hook test and
reproduces five validation error types:
`VkImageMemoryBarrier-image-03320`,
`vkCmdClearDepthStencilImage-pRanges-02659`,
`vkCmdCopyImageToBuffer-dstBuffer-00191`, `vkCmdDraw-None-09600`, and
`VkImageMemoryBarrier-oldLayout-01213`.

All seven repeated raw VRAM captures were stable per backend and scale. Fixed
Vulkan exactly matched original Vulkan in the timed canonical workload (zero
differing pixels). Software vs OpenGL differed at 19,723 of 524,288 pixels;
software vs Vulkan at 18,849; OpenGL vs Vulkan at 6,141, at each tested scale.
These differences are unresolved: no hardware-golden accuracy claim is made.
The 1x VRAM readback also does not establish supersampled image fidelity.

## Performance evidence

Host: Windows, AMD Ryzen 7 9800X3D, RTX 3080 Ti / NVIDIA 610.74. Release Clang
22.1.8 build using the retcomm `cmake-clang-v1/latest` toolchain and bundled SDL3.
Each backend ran serially: 20 warmups, seven batches of 100 iterations. Each
iteration uploads/draws a fixed synthetic scene and completes a full 1 MiB
VRAM readback. Validation, file writes, initialization, wide checks, presentation
and game emulation are outside timing. Results are host wall time, not GPU
timestamp measurements or game FPS.

Median milliseconds per iteration (batch minimum–maximum in parentheses):

| Scale | Software | OpenGL | Vulkan, fixed | Vulkan, master |
|---|---:|---:|---:|---:|
| 1x | 0.193 (0.191–0.217) | 0.576 (0.549–0.766) | 4.417 (4.202–5.634) | 4.532 (4.342–4.949) |
| 2x | 3.429 (3.385–3.624) | 0.722 (0.698–0.743) | 4.846 (4.483–6.005) | 4.610 (4.369–4.768) |
| 4x | 13.069 (12.978–13.150) | 0.941 (0.902–1.029) | 4.535 (4.409–6.048) | 4.437 (4.386–5.225) |

Classic Vulkan took about 7.7x, 6.7x and 4.8x the OpenGL time respectively in
this transfer-heavy scene. It beat software at 4x. The original/fixed Vulkan
ranges overlap; this experiment does not establish a speed improvement from
the correctness fixes. Wide refresh overhead is not measured by the timed
scene. These results cannot predict NoGraphicsAPI performance.

Portable samples/hashes are in [the JSON evidence](nographics-vulkan-results.json).
Full local logs and raw captures are under
`F:/Projects/psxrecomp/_probe-nographics/results`; original validation evidence
is in `_build-nographics-vtable/root-baseline-validation.log` and
`root-baseline-wide.log`. Build/run and A/B instructions are in the probe README.

## Work still required

1. Run the real NoGraphicsAPI device/resource probe on a driver/device supplying
   all required extensions and features. No specific replacement driver has
   been independently validated here.
2. Integrate a supported C++ toolchain boundary and port the PSX renderer and
   shaders to NoGraphicsAPI, including VRAM synchronization and Win32 presentation.
3. Resolve pixel discrepancies with focused/reference captures; validate FMV,
   mask/feedback edge cases, swapchain changes, wide presentation, netplay and
   other runtime features using actual game workloads.
4. Compare identical completed frames and game traces with validation disabled,
   measuring frame time, CPU submission cost and GPU time. Only then claim a
   performance gain or a full Vulkan implementation.

The owning issue remains open because its acceptance criteria are not met.
