param(
    [string]$Version = "v0.3.2-alpha",
    [string]$BuildDir = "runtime/build-release",
    # Drop channel = "developer" packages. Off by default so a local package
    # still carries in-progress work; CI sets EXCLUDE_DEV_MODS=1 instead.
    [switch]$ExcludeDevMods
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir
$StageRoot = Join-Path $Root "release-stage"
$Stage = Join-Path $StageRoot "PSXRecomp-windows-x64"
$ZipPath = Join-Path $Root ("PSXRecomp-{0}-windows-x64.zip" -f $Version)
$MingwBin = "C:\msys64\mingw64\bin"

$env:PATH = "$MingwBin;$env:PATH"

cmake -S (Join-Path $Root "runtime") -B $BuildPath -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DPSX_DEBUG_TOOLS=OFF `
    -DPSX_RECOMP_UI=OFF
cmake --build $BuildPath --target psx-runtime -j $env:NUMBER_OF_PROCESSORS

if (Test-Path $StageRoot) {
    $resolvedRoot = (Resolve-Path $Root).Path.TrimEnd('\')
    $resolvedStage = (Resolve-Path $StageRoot).Path.TrimEnd('\')
    if (-not $resolvedStage.StartsWith($resolvedRoot + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete stage path outside repo root: $resolvedStage"
    }
    Remove-Item -LiteralPath $StageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force $Stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage "saves") | Out-Null

Copy-Item (Join-Path $BuildPath "PSXRecomp.exe") (Join-Path $Stage "PSXRecomp.exe")
Copy-Item (Join-Path $Root "README.md") $Stage
Copy-Item (Join-Path $Root "LICENSE") $Stage
$BundledBiosSrc = Join-Path $BuildPath "bios"
$BundledBiosDst = Join-Path $Stage "bios"
New-Item -ItemType Directory -Force $BundledBiosDst | Out-Null
Copy-Item (Join-Path $BundledBiosSrc "openbios.bin") $BundledBiosDst
Copy-Item (Join-Path $BundledBiosSrc "OpenBIOS.LICENSE") $BundledBiosDst
Copy-Item (Join-Path $Root "THIRD_PARTY_ATTRIBUTION.md") $Stage
if (Test-Path (Join-Path $Root "RELEASE_NOTES.md")) {
    Copy-Item (Join-Path $Root "RELEASE_NOTES.md") $Stage
}

# The Release build is statically linked (PSX_STATIC_RUNTIME defaults ON for
# MinGW Release in runtime.cmake), so the exe imports ONLY Windows system DLLs
# so there is no SDL DLL / libgcc_s_seh-1.dll / libstdc++-6.dll to bundle. Shipping
# those side-by-side was the cause of the 0xc000007b launch crash on user
# machines that had a mismatched copy earlier on the DLL search path.
#
# Assert self-containment rather than trust it: fail packaging if the exe
# imports any non-system DLL.
$objdump = Join-Path $MingwBin "objdump.exe"
$imports = & $objdump -p (Join-Path $Stage "PSXRecomp.exe") |
    Select-String "DLL Name: (.+)" | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
$systemDlls = @("kernel32.dll","user32.dll","gdi32.dll","shell32.dll","msvcrt.dll",
                "advapi32.dll","ws2_32.dll","comdlg32.dll","dbghelp.dll","ole32.dll",
                "oleaut32.dll","winmm.dll","imm32.dll","version.dll","setupapi.dll",
                "dinput8.dll","rpcrt4.dll","hid.dll","cfgmgr32.dll","opengl32.dll")
$nonSystem = $imports | Where-Object { $systemDlls -notcontains $_.ToLower() }
if ($nonSystem) {
    throw "Release exe is NOT self-contained; imports non-system DLL(s): $($nonSystem -join ', ')"
}
Write-Host "Verified self-contained: imports only system DLLs ($($imports.Count) total)"

# No baked build-machine paths: an absolute BIOS default baked into the exe
# makes it silently load the BUILDER'S BIOS wherever that path exists, so the
# clean-install picker flow is never exercised where releases are validated.
$exeBytes = [System.IO.File]::ReadAllBytes((Join-Path $Stage "PSXRecomp.exe"))
$exeText  = [System.Text.Encoding]::ASCII.GetString($exeBytes)
$bakedBios = [regex]::Matches($exeText, '[A-Za-z]:[/\\][ -~]*?SCPH1001\.BIN') | ForEach-Object { $_.Value } | Select-Object -Unique
if ($bakedBios) {
    throw "Release exe contains baked absolute BIOS path(s): $($bakedBios -join '; '); build with a relative DEFAULT_BIOS_PATH"
}
Write-Host "Verified no baked absolute BIOS path in the exe"

# Mod catalog. runtime.cmake stages <exe>/mods for every non-oracle target, so
# a build that lacks it has broken wiring -- fail rather than ship a package
# whose Mods page is silently empty.
#
# Staged BEFORE the stray-file scan below on purpose: mods/state.toml is the
# BUILD MACHINE's per-user enable/disable state and must never ship (preloaded
# catalogs are default-disabled, so a dev's selections would become everyone's
# defaults). Strip it here -- a dev machine legitimately has one, so that is a
# strip, not a failure -- and let the stray scan confirm none survived.
$ModsSrc = Join-Path $BuildPath "mods"
$ModsDst = Join-Path $Stage "mods"
if (!(Test-Path $ModsSrc)) {
    throw "Build has no mods/ next to the exe: $ModsSrc (rebuild the runtime target)"
}
Copy-Item -Recurse -Force $ModsSrc $ModsDst
Get-ChildItem $ModsDst -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -eq "state.toml" -or $_.Name -eq "state.toml.tmp" } |
    Remove-Item -Force
$manifestCount = (Get-ChildItem $ModsDst -Recurse -File -Filter "manifest.toml" `
    -ErrorAction SilentlyContinue | Measure-Object).Count
if ($manifestCount -lt 1) {
    throw "Staged mods/ contains no manifest.toml; the catalog would ship empty"
}
# Developer-channel packages (channel = "developer") are unfinished work that
# ships with local builds but must never be published. Prune by PACKAGE
# directory; never rewrite a manifest during packaging.
$ExcludeDev = $ExcludeDevMods -or ($env:EXCLUDE_DEV_MODS -eq "1")
if ($ExcludeDev) {
    Write-Host "Excluding developer-channel mods"
    Get-ChildItem (Join-Path $ModsDst "packages") -Recurse -File -Filter "manifest.toml" `
        -ErrorAction SilentlyContinue | ForEach-Object {
        if ((Get-Content $_.FullName) -match '^\s*channel\s*=\s*"developer"\s*$') {
            $verDir = $_.Directory
            $pkgDir = $verDir.Parent
            Remove-Item -Recurse -Force $verDir.FullName
            if (-not (Get-ChildItem $pkgDir.FullName -ErrorAction SilentlyContinue)) {
                Remove-Item -Force $pkgDir.FullName
            }
            Write-Host "  excluded developer package: $($pkgDir.Name)/$($verDir.Name)"
        }
    }
    $devLeft = Get-ChildItem $Stage -Recurse -File -Filter "manifest.toml" `
        -ErrorAction SilentlyContinue |
        Where-Object { (Get-Content $_.FullName) -match '^\s*channel\s*=\s*"developer"\s*$' }
    if ($devLeft) {
        throw "developer manifest(s) survived pruning: $(($devLeft | ForEach-Object FullName) -join '; ')"
    }
    $manifestCount = (Get-ChildItem $ModsDst -Recurse -File -Filter "manifest.toml" `
        -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($manifestCount -lt 1) {
        throw "every staged package was developer-channel; the catalog would ship empty"
    }
}
Write-Host "Staged mod catalog: $manifestCount manifest(s)"

# No user-machine or copyrighted files may ride along in the stage. OpenBIOS
# is intentionally present and redistributable; retail SCPH images remain
# forbidden.
$strayPatterns = @("SCPH*.BIN","*.cue","*.iso","*.mcd","bios.cfg","disc.cfg",
                   "settings.toml","keybinds.ini","overlay_captures.json",
                   "state.toml")
$stray = foreach ($pat in $strayPatterns) { Get-ChildItem $Stage -Recurse -File -Filter $pat -ErrorAction SilentlyContinue }
if ($stray) {
    throw "Stage contains files that must never ship: $(($stray | ForEach-Object FullName) -join '; ')"
}
$savesFiles = Get-ChildItem (Join-Path $Stage "saves") -Recurse -File -ErrorAction SilentlyContinue
if ($savesFiles) {
    throw "Stage saves/ directory must be empty, contains: $(($savesFiles | ForEach-Object FullName) -join '; ')"
}
$bundledBios = Join-Path $Stage "bios/openbios.bin"
$bundledLicense = Join-Path $Stage "bios/OpenBIOS.LICENSE"
if (!(Test-Path $bundledBios) -or
    (Get-Item $bundledBios).Length -ne 524288 -or
    !(Test-Path $bundledLicense)) {
    throw "Stage must contain the 512 KiB OpenBIOS image and its MIT notice"
}
Write-Host "Verified bundled OpenBIOS + notice; no retail BIOS/disc/save/sidecar files"

@"
; PSXRecomp input mapping. PSX buttons are active when any listed source is pressed.
; Sources use SDL/Xbox names: a,b,x,y,back,start,leftshoulder,rightshoulder,
; lefttrigger,righttrigger,leftstick,rightstick (stick clicks -> L3/R3),
; dpup,dpdown,dpleft,dpright,leftx-/leftx+/lefty-/lefty+.

[controller]
enabled = true
device = 0
deadzone = 12000

[mapping]
up = dpup,lefty-
down = dpdown,lefty+
left = dpleft,leftx-
right = dpright,leftx+
cross = a
circle = b
square = x
triangle = y
l1 = leftshoulder
r1 = rightshoulder
l2 = lefttrigger
r2 = righttrigger
l3 = leftstick
r3 = rightstick
start = start
select = back
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "input.ini")

@"
PSXRecomp $Version

This package includes the MIT-licensed OpenBIOS from PCSX-Redux. Its notice is
in bios/OpenBIOS.LICENSE. It does not include a retail PlayStation BIOS, game
disc image, generated game source, save data, or copyrighted Sony/game assets.

First launch:
1. Run PSXRecomp.exe.
2. OpenBIOS runs by default. No external BIOS is required.

You can select your legally obtained SCPH1001.BIN instead. The selected path is
saved in bios.cfg next to the executable; clear it to return to OpenBIOS.

This framework package is BIOS-only. Game-specific PSXRecomp releases use the
same BIOS picker and additionally prompt for the required game disc image.

Keyboard and Xbox-style controller defaults are documented in README.md.
Controller mappings are configurable in input.ini.

Memory cards are stored in the saves directory.
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "START_HERE.txt")

if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath -Force

Write-Host "Wrote $ZipPath"
