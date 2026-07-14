param(
    [string]$Version = "v0.1.0-alpha",
    [string]$BuildDir = "runtime/build-release"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir
$StageRoot = Join-Path $Root "release-stage"
$Stage = Join-Path $StageRoot "PSXRecomp-windows-x64"
$ZipPath = Join-Path $Root ("PSXRecomp-{0}-windows-x64.zip" -f $Version)
$MingwBin = "C:\msys64\mingw64\bin"

$env:PATH = "$MingwBin;$env:PATH"

cmake -S (Join-Path $Root "runtime") -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=OFF
cmake --build $BuildPath --target psx-runtime -j $env:NUMBER_OF_PROCESSORS

if (Test-Path $StageRoot) {
    Remove-Item -Recurse -Force $StageRoot
}
New-Item -ItemType Directory -Force $Stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage "saves") | Out-Null

Copy-Item (Join-Path $BuildPath "psx-runtime.exe") (Join-Path $Stage "PSXRecomp.exe")
Copy-Item (Join-Path $Root "README.md") $Stage
Copy-Item (Join-Path $Root "LICENSE") $Stage
if (Test-Path (Join-Path $Root "RELEASE_NOTES.md")) {
    Copy-Item (Join-Path $Root "RELEASE_NOTES.md") $Stage
}

# The Release build is statically linked (PSX_STATIC_RUNTIME defaults ON for
# MinGW Release in runtime.cmake), so the exe imports ONLY Windows system DLLs
# — no SDL2.dll / libgcc_s_seh-1.dll / libstdc++-6.dll to bundle. Shipping
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
                "dinput8.dll","rpcrt4.dll","hid.dll","cfgmgr32.dll")
$nonSystem = $imports | Where-Object { $systemDlls -notcontains $_.ToLower() }
if ($nonSystem) {
    throw "Release exe is NOT self-contained — imports non-system DLL(s): $($nonSystem -join ', ')"
}
Write-Host "Verified self-contained: imports only system DLLs ($($imports.Count) total)"

# No baked build-machine paths: an absolute BIOS default baked into the exe
# makes it silently load the BUILDER'S BIOS wherever that path exists, so the
# clean-install picker flow is never exercised where releases are validated.
$exeBytes = [System.IO.File]::ReadAllBytes((Join-Path $Stage "PSXRecomp.exe"))
$exeText  = [System.Text.Encoding]::ASCII.GetString($exeBytes)
$bakedBios = [regex]::Matches($exeText, '[A-Za-z]:[/\\][ -~]*?SCPH1001\.BIN') | ForEach-Object { $_.Value } | Select-Object -Unique
if ($bakedBios) {
    throw "Release exe contains baked absolute BIOS path(s): $($bakedBios -join '; ') — build with a relative DEFAULT_BIOS_PATH"
}
Write-Host "Verified no baked absolute BIOS path in the exe"

# No user-machine or copyrighted files may ride along in the stage.
$strayPatterns = @("SCPH*.BIN","*.cue","*.iso","*.mcd","bios.cfg","disc.cfg",
                   "settings.toml","keybinds.ini","overlay_captures.json")
$stray = foreach ($pat in $strayPatterns) { Get-ChildItem $Stage -Recurse -File -Filter $pat -ErrorAction SilentlyContinue }
if ($stray) {
    throw "Stage contains files that must never ship: $(($stray | ForEach-Object FullName) -join '; ')"
}
$savesFiles = Get-ChildItem (Join-Path $Stage "saves") -Recurse -File -ErrorAction SilentlyContinue
if ($savesFiles) {
    throw "Stage saves/ directory must be empty, contains: $(($savesFiles | ForEach-Object FullName) -join '; ')"
}
Write-Host "Verified stage carries no BIOS/disc/save/sidecar files"

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

This package does not include a PlayStation BIOS, game disc image, generated
game source, save data, or any copyrighted Sony/game assets.

First launch:
1. Run PSXRecomp.exe.
2. Select your legally obtained SCPH1001.BIN BIOS when prompted.

The selected BIOS path is saved in bios.cfg next to the executable. Delete that
file if you want to pick a different BIOS later.

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
