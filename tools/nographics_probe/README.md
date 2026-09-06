# NoGraphicsAPI Device Probe

This opt-in probe builds upstream NoGraphicsAPI through its own CMake project,
then runs the real headless `gpu::create_device({})` path. If device creation
succeeds, it checks the existing Vulkan renderer's two 1024x512 resource roles:
an `rgba8_unorm` sampled color target and an `r16_uint` sampled/storage raw VRAM
mirror, both with transfer source/destination usage. Each is uploaded from a
CPU-visible heap, copied into a readback heap, synchronized with a timeline,
and compared byte for byte. It does not add
NoGraphicsAPI to the normal psxrecomp runtime build.

Both resource round trips pass on RTX 3080 Ti after the approved NVIDIA 616.86
hotfix installation. The initial packed 5551 test was unsupported and did not
match the renderer's resource contract. The revised test proves raw-byte
transport; it does not implement PSX channel order, mask bits, rasterization,
or presentation. It is a prerequisite for renderer work, not a full renderer.

Debug builds need a validation layer new enough to recognize
`VK_KHR_device_address_commands`. The installed SDK 1.4.341.1 layer reports
unknown extension/structure diagnostics and crashes during transfers. The local
1.4.357 layer described below passes the Debug probe with core and synchronization
validation. CTest fails on upstream validation diagnostics, including when the
executable returns zero.

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
$env:VK_LAYER_PATH = "$taskProbe/build-vvl-root-vs/layers/Release"
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC = '1'
& "$taskCmake/ctest.exe" --test-dir $taskBuild -C Debug --output-on-failure
```

Observed after the hotfix: all three example binaries built, all generated
shaders validated, and all seven Debug tests passed without validation diagnostics.
Before the hotfix, the two GPU tests skipped. These tests do not establish PSX
rendering accuracy or example presentation.

## Local validation layer

The investigation built the unmodified `vulkan-sdk-1.4.357.0` tag of
KhronosGroup/Vulkan-ValidationLayers at
`f4874eee15c78d7bdb2b7e60659d539f14741500`. For a fresh build, use a native
Windows PATH so dependency scripts cannot pick up MSYS compiler/CMake shims.
Keep dependencies under the source's `external` directory for VS source groups.
From a dedicated PowerShell session:

```powershell
$taskProbe = 'F:/Projects/psxrecomp/_probe-nographics'
$taskSource = "$taskProbe/Vulkan-ValidationLayers-1.4.357.0"
$taskCmake = "$env:ProgramFiles/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
$taskPython = 'C:/Users/Matthew/AppData/Local/Programs/Python/Python312'
$env:PATH = "$taskCmake;C:/Program Files/Git/cmd;$taskPython;C:/Windows/System32;C:/Windows"
& 'C:/Program Files/Git/cmd/git.exe' clone --depth 1 --branch vulkan-sdk-1.4.357.0 `
  https://github.com/KhronosGroup/Vulkan-ValidationLayers.git $taskSource
& "$taskCmake/cmake.exe" -S $taskSource -B "$taskProbe/build-vvl-root-vs" `
  -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_BUILD_TYPE=Release' '-DBUILD_TESTS=OFF' `
  '-DUPDATE_DEPS=ON' `
  "-DPython3_EXECUTABLE=$taskPython/python.exe" `
  "-DUPDATE_DEPS_DIR=$taskSource/external/root-native-deps" `
  '-DUPDATE_DEPS_SKIP_EXISTING_INSTALL=ON'
& "$taskCmake/cmake.exe" --build "$taskProbe/build-vvl-root-vs" `
  --config Release --target vvl --parallel 8
$env:VK_LAYER_PATH = "$taskProbe/build-vvl-root-vs/layers/Release"
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC = '1'
& "$taskCmake/ctest.exe" --test-dir 'F:/Projects/psxrecomp/_build-nographics-create-device-vs' `
  -C Debug -V
```

The layer manifest reports API 1.4.357. These process-local settings leave the
installed SDK unchanged. The resource test and upstream GPU tests passed on
driver 616.86 before the installer-requested reboot; repeat them after reboot
before collecting new same-driver performance baselines.
