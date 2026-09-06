# NoGraphicsAPI / Vulkan investigation — 2026-09-06

**Status: NoGraphicsAPI device creation and resource transfers now run; the full
PSX renderer port is not delivered.** The user-approved NVIDIA 616.86 hotfix
removed the extension blocker. RGBA8 color and R16_UINT raw VRAM resource
round trips pass. The installer requests a reboot before further performance
testing. Existing Vulkan fixes and prior-driver measurements remain saved;
no NoGraphicsAPI performance gain has been measured.

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

Initial extension enumeration, before the approved hotfix:

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
not add missing driver extensions. These observations preceded the hotfix.

Follow-up eligibility check: the probe now prints the NVIDIA driver version
directly from Vulkan (`610.74.0.0`) and checks coherent host-visible device-local
memory types. RTX 3080 Ti type 5 exposes a 12,670,992,384-byte device-local heap;
the AMD adapter also exposes qualifying types. This clears the memory-type
existence check, but does not prove allocation budget, format compatibility or
the remaining feature gates that upstream checks after extensions.

The resource test initially used a packed 5551 color target. After device
creation became available, that format/usage was unsupported. The actual
classic Vulkan renderer uses an RGBA8 color target and an R16_UINT raw mirror;
the probe now requires both of those resource roles instead. Both complete
upload/readback and byte comparison in the Release probe (exit 0). These checks
do not implement PSX texture interpretation or rasterization.
GPT-5.5 review confirmed the transfer scope: the probe does not exercise sampled
reads, color rendering, storage-image shaders, odd/subrectangle copies, nonzero
copy offsets or presentation. Those operations still need targeted validation
as the renderer is ported.

### Approved hotfix installation

NVIDIA's [616.64 release notes](https://us.download.nvidia.com/Windows/616.64/616.64-win11-win10-release-notes.pdf)
list RTX 3080 Ti support. NoGraphicsAPI reports RTX 30 compatibility with that
release. NVIDIA's separate [Vulkan developer-driver history](https://developer.nvidia.com/vulkan-driver)
also records introduction of device-address commands in beta 595.92. Branch
version numbers alone do not establish extension support on the installed
general-release driver.

The [616.86 hotfix](https://nvidia.custhelp.com/app/answers/detail/a_id/5906), based
on 616.64, fixes virtual-display creation and RDP regressions after 616.56.
This machine has Meta Virtual Monitor, making those fixes relevant. 616.86 is
an optional beta hotfix with abbreviated QA, not a WHQL release.

The official installer was downloaded from NVIDIA's linked URL and verified
with Windows Authenticode: **Valid**, signer **NVIDIA Corporation**. It is saved
outside the source checkout at
`F:/Projects/psxrecomp/_probe-nographics/drivers/616.86-desktop-notebook-win10-win11-64bit-international-dch.hf.exe`.
Size: 984,119,224 bytes. Recorded SHA-256:
`e14b1c806bd7d7657c612da6144a3412ed9f055a067548ea05ecd30a7d7ed7e4`.
The user explicitly approved installation. The extracted, signed `setup.exe`
ran with `-s -noreboot Display.Driver`. It completed at
`2026-09-06T16:38:18Z` with code 1, which NVIDIA documents as
[success, reboot required](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/windows.html).
No machine reboot was initiated. Windows reports version `32.0.16.1686`, device
status OK, and ConfigManager error code 0. Vulkan reports `616.86.0.0`, API
1.4.351, all four required extensions, and `create_device.error=none`.
Meta Virtual Monitor remains present with status OK. The former device gate
is cleared; a reboot remains required by the installer.

The old SDK 1.4.341.1 validation layer does not recognize the device-address
command extension and its feature structure, and crashes during the transfer
probe. It cannot validate this API. A separate local validation layer now builds
from `vulkan-sdk-1.4.357.0`, commit
`f4874eee15c78d7bdb2b7e60659d539f14741500`, using native VS2022/MSVC and its
pinned dependencies. No system SDK settings were changed.

With that layer selected explicitly and `VK_LAYER_VALIDATE_SYNC=1`, the Debug
device/resource CTest passes both byte round trips with no core or synchronization
diagnostics. The test fails on upstream's validation-message prefix even if the
process returns zero. Release CTest also passes. Logs are saved outside Git at
`_probe-nographics/hotfix-debug-validation.log` and
`_probe-nographics/hotfix-upstream-validation.log`. The separate
`_probe-nographics/hotfix-validation-loader.log` confirms the local layer DLL
was inserted into the device call chain. Performance baselines must
be rerun after reboot; the table below remains from driver 610.74.

### Shader toolchain prepared

The installed SDK's Slang `2026.1-52-gc8ddf20bb` and SPIRV-Tools 2026.1 are
below upstream's example requirements. Separate local tools now provide:

- Slang 2026.14.1, official Windows x64 release archive, SHA-256
  `5ed0a59d650a0af0aca45d5db4e083b3d8fb5cea05748747dd95dfbe9c580658`,
  verified against the GitHub release asset digest.
- SPIRV-Tools v2026.3, commit `b707790a898e44038547df54580022fc1cf89c3d`,
  built locally with SPIRV-Headers `29981f65241605e08b0ede4cfeb999fe3b723c6a`.

The unmodified pinned upstream project builds all three examples (triangle,
cube, deferred renderer), compiling and validating their vertex, fragment,
compute and mesh shaders. Before the hotfix, five host-side tests passed and
two GPU tests skipped. After the hotfix, all seven Debug tests pass with the
1.4.357 validation layer and synchronization validation enabled, without
validation diagnostics. Examples have not rendered on this host.
Build location: `F:/Projects/psxrecomp/_build-nographics-examples-vs`.
Commands are in the [probe README](../../tools/nographics_probe/README.md).
These checks prepare the actual NoGraphicsAPI path; they do not validate a PSX
shader port. All backend baselines must be rerun after a driver change to avoid
attributing driver-only performance changes to the new renderer.

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

## Performance evidence (driver 610.74, before hotfix)

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

1. Complete the installer-requested reboot and repeat device/resource tests.
   Device creation, both raw resource roles and all seven upstream tests already
   pass on the active 616.86 driver with the updated validation layer before reboot.
2. Integrate a supported C++ toolchain boundary and port the PSX renderer and
   shaders to NoGraphicsAPI, including VRAM synchronization and Win32 presentation.
3. Resolve pixel discrepancies with focused/reference captures; validate FMV,
   mask/feedback edge cases, swapchain changes, wide presentation, netplay and
   other runtime features using actual game workloads.
4. Compare identical completed frames and game traces with validation disabled,
   measuring frame time, CPU submission cost and GPU time. Only then claim a
   performance gain or a full Vulkan implementation.

The owning issue remains open because its acceptance criteria are not met.
