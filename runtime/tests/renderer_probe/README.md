# Renderer probe

Opt-in Windows GPU regression test and renderer microbenchmark. This links the
production software, OpenGL, and Vulkan backends through `gpu_render.c`. It
creates a hidden SDL window and real GPU contexts; it does not require a ROM or
BIOS. `stubs.c` supplies unrelated runtime services (UI, netplay, timing,
interpolation and display configuration), not renderer implementations.

Build with the same SDL/toolchain as the runtime. Example in PowerShell, with
the repository as the current directory:

```powershell
# Set taskTc to your installed native toolchain if it is elsewhere.
$taskTc = "$env:USERPROFILE/.local/share/retcomm/toolchains/cmake-clang-v1/latest"
$taskPython = "$env:LOCALAPPDATA/Programs/Python/Python312/python.exe"
$env:VULKAN_SDK = 'C:/VulkanSDK/1.4.341.1'
& "$taskTc/bin/cmake.exe" -S runtime/tests/renderer_probe -B ../_build-renderer-probe -G Ninja `
  "-DCMAKE_C_COMPILER=$taskTc/bin/clang.exe" "-DCMAKE_MAKE_PROGRAM=$taskTc/bin/ninja.exe" `
  '-DCMAKE_BUILD_TYPE=Release' "-DPython3_EXECUTABLE=$taskPython" `
  "-DSDL3_DIR=$taskTc/deps/lib/cmake/SDL3"
& "$taskTc/bin/cmake.exe" --build ../_build-renderer-probe
$env:VK_LAYER_PATH = "$env:VULKAN_SDK/Bin"
& $taskPython runtime/tests/renderer_probe/benchmark.py `
  --exe ../_build-renderer-probe/renderer_probe.exe --out ../_renderer-results `
  --scales 1 2 4 --warmup 20 --repeats 7 --iters 100 --validate
```

Use native Windows executable paths when MSYS shims are on PATH. SDL3 must be
available at runtime (a static SDL3 toolchain works without a DLL).

For an A/B comparison, build a second probe with the same options and
`-DPSX_RENDERER_PROBE_VULKAN_SOURCE=<absolute path to original gpu_vk_renderer.c>`.
Pass its executable using `--baseline`. Keep headers, shaders, harness, compiler,
and build configuration identical. The baseline skips the wide regression test
because master at `b213858c` has no `wide_dump_full` hook. It executes the same
timed workload. An older renderer requiring different headers/shaders needs its
own compatible checkout instead of this source override.

Each iteration resets state, uploads textures/CLUTs, draws flat/shaded/textured
primitives (4/8/15-bit textures), exercises texture windows, mask bits, all four
blend modes, VRAM copies and render-to-texture feedback, then reads back all
1024x512 16-bit VRAM words. Readback forces GPU completion. Wall time includes
CPU driver overhead, uploads, rendering, synchronization and readback. It
excludes initialization, wide checks, capture-file writes, presentation, vsync
and game CPU emulation. These are milliseconds per synthetic iteration, not
game FPS. Frequent transfers deliberately stress this workload; performance
need not represent a game that keeps rendering on the GPU.

The runner executes backends serially, verifies backend/scale selection, saves
raw VRAM captures, enforces repeatability with SHA-256, and reports exact pixel
differences. A successful process is **not** an assertion of software/GPU pixel
parity. Compare the differences explicitly. The native VRAM captures also do
not prove supersampled image quality.

Outside timing, the probe checks full wide-surface dimensions and band-readback
pitch. Vulkan additionally checks upload coherence in both vertical framebuffer
bands, preservation of reveal margins, and rejection of insufficient full-dump
capacity without changing caller data. It uses the actual context scale.
`--validate` runs Vulkan core and synchronization validation separately from
timing and rejects validation errors. Install the Khronos validation layer;
timed runs remove the runner's validation environment overrides.

Not covered: GP0 packet decoding, BIOS/game execution, 24-bit FMV, presentation
and swapchain recreation, screenshots through the debug server, netplay,
rewind, interpolation, perspective corrections, or every PSX edge case.
The experimental NoGraphicsAPI runtime backend is opt-in via
`-DPSX_NOGRAPHICS_DLL=<absolute path to psx_nographics.dll>` when configuring this
probe. See [its build instructions](../../nographics/README.md). Use
`--backend vulkan_nographics --scale 1 --contracts --present` for extra pixel
contracts and presentation/resize calls outside timing. `--contracts` also
exposes known differences/limitations in other backends (native Vulkan currently
fails the wrapped-transfer case; software drops texture STP on write).

For a four-backend measurement, add `--nographics --scales 1` to `benchmark.py`.
Use a Release DLL for timing. For Debug validation, select the local 1.4.357
layer documented by the NoGraphicsAPI device probe; SDK 1.4.341 cannot validate
the device-address command extension. The runner rejects NoGraphicsAPI validation
diagnostics as failures. The separate resource-only probe remains in
`tools/nographics_probe`.
