# NoGraphicsAPI Device Probe

This opt-in probe builds upstream NoGraphicsAPI through its own CMake project,
then runs the real headless `gpu::create_device({})` path. If device creation
succeeds, it also attempts an experimental resource smoke test:
a 1024x512 `r5g5b5a1_unorm` VRAM texture with transfer, sampled, and color
attachment usage; CPU-visible upload heap; readback heap; command submission;
timeline wait; and byte comparison after texture readback. It does not add
NoGraphicsAPI to the normal psxrecomp runtime build.

The resource smoke is compiled but has **not run successfully on this host**:
device creation returns `unsupported`. Its packed texture format is only a
raw-byte transport test; it does not implement PSX channel order, mask bits,
rasterization, or presentation. Passing this probe would be a prerequisite
for renderer work, not a complete renderer.

The probe intentionally keeps the runtime loader/library separate from the
headers. Use `Vulkan-Headers` 1.4.357+ for compile-time declarations and the
installed Vulkan SDK only for `vulkan-1.lib`.

Reference inputs used for this investigation:

- NoGraphicsAPI: `469ebba558ee46f93c73a9f285979aa00fce7500`
- Vulkan-Headers: `v1.4.357`, `e3b1eec08173d6b825cd3ac88c885a63b621504a`
- Vulkan loader/import lib: `C:\VulkanSDK\1.4.341.1\Lib\vulkan-1.lib`

Setup from `F:\Projects\psxrecomp`:

```bat
git clone https://github.com/sebbbi/NoGraphicsAPI.git _probe-nographics\NoGraphicsAPI
git -C _probe-nographics\NoGraphicsAPI checkout 469ebba558ee46f93c73a9f285979aa00fce7500
git clone --depth 1 --branch v1.4.357 https://github.com/KhronosGroup/Vulkan-Headers.git _probe-nographics\Vulkan-Headers-1.4.357
```

Build and run from a native x64 Visual Studio environment:

```bat
"%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  -S F:\Projects\psxrecomp\_wt-nographics-vulkan\tools\nographics_probe ^
  -B F:\Projects\psxrecomp\_build-nographics-create-device ^
  -G "Visual Studio 17 2022" -A x64 ^
  -DNOGRAPHICSAPI_ROOT=F:\Projects\psxrecomp\_probe-nographics\NoGraphicsAPI ^
  -DVULKAN_HEADERS_ROOT=F:\Projects\psxrecomp\_probe-nographics\Vulkan-Headers-1.4.357 ^
  -DVULKAN_SDK_ROOT=%VULKAN_SDK%
"%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  --build F:\Projects\psxrecomp\_build-nographics-create-device --config Debug
"%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" ^
  --test-dir F:\Projects\psxrecomp\_build-nographics-create-device -C Debug --output-on-failure
```

The executable first prints device names, API/driver versions, coherent
CPU-visible device-local memory types and the four required NoGraphicsAPI
extension names, then calls the real upstream
`gpu::create_device({})` path. The extension-only check is diagnostic; full
eligibility is the `create_device.error` line. On a compatible device, the
`psx_vram_smoke=` line then reports whether the resource path needed for a
NoGraphicsAPI-backed PSX VRAM texture is viable.
The memory diagnostic reports heap capacity, not available allocation budget
or buffer/image compatibility. It does not replace upstream device creation.

```bat
F:\Projects\psxrecomp\_build-nographics-create-device\Debug\nographics_create_device_probe.exe
```

Exit code `0` means NoGraphicsAPI created a headless device and the PSX VRAM
resource smoke passed. Exit code `77` means CTest records the probe as skipped
because the driver/device reported `gpu::Error::unsupported`. Exit code `2`
means device creation succeeded but the PSX VRAM resource smoke failed.
Headless creation and resource upload/readback do not prove Win32 swapchain
presentation.

`NOGRAPHICSAPI_EXPECTED_REVISION` defaults to the commit above and fails if Git
is unavailable, the checkout is at another commit, or the checkout is dirty.
Set `-DNOGRAPHICSAPI_EXPECTED_REVISION=` only when intentionally testing another
revision or a local NoGraphicsAPI patch.

## Upstream shader and host-test verification

The installed SDK tools are too old for upstream's examples. The investigation
prepared separate Slang 2026.14.1 and SPIRV-Tools v2026.3 under
`F:/Projects/psxrecomp/_probe-nographics`; pinned revisions and archive digest
are in [the status report](../../docs/internal/nographics-vulkan-status.md).
With those tools present, the following PowerShell commands build the original
upstream examples and validate their SPIR-V without changing runtime toolchains:

```powershell
$taskProbe = 'F:/Projects/psxrecomp/_probe-nographics'
$taskBuild = 'F:/Projects/psxrecomp/_build-nographics-examples-vs'
$taskCmake = "$env:ProgramFiles/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
& "$taskCmake/cmake.exe" -S "$taskProbe/NoGraphicsAPI" -B $taskBuild `
  -G 'Visual Studio 17 2022' -A x64 `
  "-DVulkan_INCLUDE_DIR=$taskProbe/Vulkan-Headers-1.4.357/include" `
  '-DVulkan_LIBRARY=C:/VulkanSDK/1.4.341.1/Lib/vulkan-1.lib' `
  '-DNOGRAPHICSAPI_BUILD_EXAMPLES=ON' '-DNOGRAPHICSAPI_BUILD_TESTS=ON' `
  "-DNOGRAPHICSAPI_SLANGC=$taskProbe/slang-2026.14.1/bin/slangc.exe" `
  "-DNOGRAPHICSAPI_SPIRV_VAL=$taskProbe/build-spirv-tools-v2026.3/tools/spirv-val.exe"
& "$taskCmake/cmake.exe" --build $taskBuild --config Debug --parallel 8
$env:VK_LAYER_PATH = 'C:/VulkanSDK/1.4.341.1/Bin'
& "$taskCmake/ctest.exe" --test-dir $taskBuild -C Debug --output-on-failure
```

Observed: all three example binaries built, all generated shaders validated,
five host-side tests passed, and both GPU tests skipped. A skipped GPU test is
not evidence of successful NoGraphicsAPI execution or PSX rendering accuracy.
