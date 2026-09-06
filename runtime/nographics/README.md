# Experimental NoGraphicsAPI renderer

This is a separate GPU renderer, loaded from `psx_nographics.dll`. It does not
replace `gpu_vk_renderer.c`. PSX primitive work runs in NoGraphicsAPI compute
shaders against GPU VRAM; a graphics shader presents through its Win32 swapchain.

The current implementation supports canonical 1x rendering, flat/shaded/textured
primitives, 4/8/16-bit texture lookup, CLUTs, mask checks, semi-transparency,
texture windows, VRAM transfers/copies, readback, CPU image presentation and
swapchain resizing. It uses snapshots for texture feedback and overlapping
copies, and batches primitive commands between transfers/readbacks.

It is not feature-equivalent to native Vulkan. SSAA, raster bilinear filtering,
PGXP/perspective correction, native-wide composition, OSD overlays and selectable
present modes are not implemented. Rasterization differences remain; no full
game compatibility or hardware accuracy claim is made. OpenGL remains the
default. See [validation and measurements](../../docs/internal/nographics-vulkan-status.md).

## Build

Use Windows x64 with MSVC/C++20 for the DLL. The normal MinGW runtime loads its
versioned C ABI, so C++ objects and allocation ownership never cross toolchains.
Shaders are compiled with Slang, validated with SPIRV-Tools and embedded in the DLL.

The pinned dependency and local tool preparation are documented in
[the device probe](../../tools/nographics_probe/README.md). Example with those
tools installed (from the worktree root):

```powershell
$taskProbe = 'F:/Projects/psxrecomp/_probe-nographics'
$taskVs = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
& "$taskVs/cmake.exe" -S runtime/nographics -B ../_build-nographics-renderer-root-vs `
  -G 'Visual Studio 17 2022' -A x64 `
  "-DNOGRAPHICSAPI_ROOT=$taskProbe/NoGraphicsAPI" `
  "-DVULKAN_HEADERS_ROOT=$taskProbe/Vulkan-Headers-1.4.357" `
  '-DVULKAN_SDK_ROOT=C:/VulkanSDK/1.4.341.1' `
  "-DNOGRAPHICSAPI_SLANGC=$taskProbe/slang-2026.14.1/bin/slangc.exe" `
  "-DNOGRAPHICSAPI_SPIRV_VAL=$taskProbe/build-spirv-tools-v2026.3/tools/spirv-val.exe"
& "$taskVs/cmake.exe" --build ../_build-nographics-renderer-root-vs `
  --config Release --target psx_nographics_runtime
```

Add these options to the normal runtime/game configure command:

```text
-DPSX_ENABLE_NOGRAPHICS=ON
-DPSX_NOGRAPHICS_DLL=F:/Projects/psxrecomp/_build-nographics-renderer-root-vs/Release/psx_nographics.dll
```

The runtime build stages the DLL beside its executable, refreshing it on every
build. Alternatively, the build helper can build this standalone project itself;
see [BUILDING.md](../../docs/BUILDING.md#supplemental-nographicsapi-dll).

## Select

Games with `[video] offer_vulkan = true` offer both Vulkan choices when the
NoGraphicsAPI DLL is enabled and present. Set `offer_vulkan_nographics = false`
to hide just the experimental choice. The launcher uses:

- **Vulkan (Native)**: ID 2, `renderer = "vulkan"` (unchanged).
- **Vulkan (NoGraphicsAPI, Experimental)**: ID 3, `renderer = "vulkan_nographics"`.

Unsupported devices or incompatible DLLs fall back to software with a diagnostic.
Disabled builds or missing DLLs fall back to OpenGL before initialization.
File presence allows the launcher choice;
actual device eligibility is checked at initialization. Netplay retains its
existing software-authoritative fallback. Effective 1x/nearest settings are
reported when the experimental backend starts.

## Validate

Build the [renderer probe](../tests/renderer_probe/README.md) with the same
`PSX_NOGRAPHICS_DLL` option. A Debug DLL plus the local 1.4.357 validation layer
checks real NoGraphicsAPI calls. Run `--backend vulkan_nographics --contracts
--present --scale 1`; this tests wrapped transfers/copies, masks, CLUTs, blending,
triangle-edge ownership, CPU mirror coherence, then the synthetic workload and
presentation/resize entry points. The probe requires the requested backend to
be active, so fallback cannot count as success. Presentation calls passing do
not establish visual correctness of every swapchain pixel.

Use the Release DLL and `benchmark.py --nographics --scales 1` for comparable
completed-work measurements. Every backend must run on the same driver with
validation disabled. The NVIDIA 616.86 installer requested a reboot; measurements
made before that reboot are provisional.
