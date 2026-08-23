# New Project Layout scaffolding (Windows).
#
# Path params (pass on the CLI -- tab-complete in your shell):
#   -Disc <cue>           REQUIRED legal Redump .cue (kept on your dump drive)
#   -Dir <parent>         Parent directory (default: .)
#   -Bios <SCPH1001.BIN>  Optional retail BIOS for Generate
#   -StageDisc            Copy full cue+bins into repo disc/ (large; optional)
#   Default: probe in place and extract boot EXE only into disc/
#
# Other options are prompted when interactive, or set via switches / -Yes.
#
# Usage:
#   powershell -File setup_project.ps1 -Disc C:\dumps\game.cue
#   powershell -File setup_project.ps1 -Disc game.cue -Name Foo -Yes
#   powershell -File setup_project.ps1 -Disc game.cue -StageDisc

[CmdletBinding()]
param(
    [string]$Name = "",
    [Parameter(Mandatory = $true)][string]$Disc,
    [string]$Dir = ".",
    [string]$BootExe = "SLUS_01234",
    [int]$Players = 0,
    [string]$ZipPrefix = "",
    [string]$GithubOwner = "",
    [string]$GithubRepo = "",
    [switch]$EnableRecompUi,
    [switch]$NoRecompUi,
    [switch]$EnableNetplay,
    [switch]$NoNetplay,
    [string]$LobbyUrl = "",
    [switch]$EnableWizard,
    [switch]$NoWizard,
    [switch]$EnableCi,
    [switch]$NoCi,
    [switch]$FetchBoxart,
    [switch]$NoFetchBoxart,
    [switch]$Generate,
    [switch]$NoGenerate,
    [switch]$EnableBuild,
    [switch]$NoBuild,
    [switch]$CreateGithub,
    [switch]$NoGithub,
    [ValidateSet("public", "private", "internal")][string]$GithubVisibility = "private",
    [string]$Description = "",
    [string]$Publisher = "",
    [string]$Year = "",
    [string]$Region = "",
    [switch]$Yes,
    [string]$Bios = "",
    [switch]$StageDisc,
    # mstan/psxrecomp and recomp-ui both use master (not main).
    [string]$PsxrecompRef = "master",
    [string]$RecompUiRef = "master",
    [string]$PsxrecompUrl = "https://github.com/mstan/psxrecomp.git",
    [string]$RecompUiUrl = "https://github.com/mstan/recomp-ui.git"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$TemplateDir = Join-Path $ScriptDir "templates"
$ProbeDisc = Join-Path $ScriptDir "probe_disc.py"
$FillTokens = Join-Path $ScriptDir "fill_tokens.py"
$FetchBoxartPy = Join-Path $ScriptDir "fetch_boxart.py"
$DefaultLobbyHost = "netplay.retcomm.net"

function Normalize-LobbyUrl([string]$In) {
    if ([string]::IsNullOrWhiteSpace($In)) { $In = $DefaultLobbyHost }
    $In = $In.Trim()
    if ($In -match '^(?i)wss?://') { return $In }
    if ($In -match '://') { return $In }
    if ($In -match ':') { return "ws://$In" }
    return "ws://${In}:8765"
}

function Assert-Cmd([string]$cmd) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        throw "Missing required command: $cmd"
    }
}
Assert-Cmd git
Assert-Cmd python

function Test-Interactive {
    return [Environment]::UserInteractive -and -not $Yes -and -not ($env:PSXRECOMP_SETUP_YES -eq "1")
}

function Prompt-Line([string]$Question, [string]$Default = "") {
    if ($Default) {
        $ans = Read-Host "$Question [$Default]"
    } else {
        $ans = Read-Host $Question
    }
    if ([string]::IsNullOrWhiteSpace($ans)) { return $Default }
    return $ans.Trim()
}

function Prompt-Yn([string]$Question, [bool]$DefaultYes) {
    $hint = if ($DefaultYes) { "Y/n" } else { "y/N" }
    while ($true) {
        $ans = Read-Host "$Question [$hint]"
        if ([string]::IsNullOrWhiteSpace($ans)) { return $DefaultYes }
        switch -Regex ($ans.Trim()) {
            '^[yY](es|$)' { return $true }
            '^[nN](o|$)' { return $false }
            default { Write-Host "  please answer y or n" }
        }
    }
}

if (-not (Test-Path -LiteralPath $Disc)) {
    throw "-Disc not found: $Disc (pass the path on the CLI so your shell can tab-complete it)"
}

$interactive = Test-Interactive

if (-not $Name) {
    if (-not $interactive) {
        throw "-Name is required in non-interactive mode (-Yes)"
    }
    $Name = Prompt-Line "Project name (e.g. MyGameRecomp)" ""
    if (-not $Name) { throw "Project name is required" }
}

if ($Players -eq 0) {
    if ($interactive) {
        $p = Prompt-Line "Max players (1-8)" "2"
        $Players = [int]$p
    } else {
        $Players = 2
    }
}
if ($Players -lt 1 -or $Players -gt 8) {
    throw "Players must be 1-8 (got: $Players)"
}

$env:PYTHONPATH = "$ScriptDir" + $(if ($env:PYTHONPATH) { ";$env:PYTHONPATH" } else { "" })
$derivedZip = (python -c "from fill_tokens import derive_zip_prefix; import sys; print(derive_zip_prefix(sys.argv[1]))" $Name).Trim()
if (-not $ZipPrefix) {
    if ($interactive) {
        $ZipPrefix = Prompt-Line "Zip / CI artifact prefix" $derivedZip
    } else {
        $ZipPrefix = $derivedZip
    }
}

if (-not $GithubOwner) {
    if ($interactive) {
        $GithubOwner = Prompt-Line "GitHub owner / org (README download badges)" "TechnicallyComputers"
    } else {
        $GithubOwner = "TechnicallyComputers"
    }
}
$derivedRepo = (python -c "from fill_tokens import sanitize_github_name; import sys; print(sanitize_github_name(sys.argv[1]))" $Name).Trim()
if (-not $GithubRepo) {
    if ($interactive) {
        $GithubRepo = Prompt-Line "GitHub repo name (README download badges)" $derivedRepo
    } else {
        $GithubRepo = $derivedRepo
    }
}
$GithubOwner = (python -c "from fill_tokens import sanitize_github_name; import sys; print(sanitize_github_name(sys.argv[1]))" $GithubOwner).Trim()
$GithubRepo = (python -c "from fill_tokens import sanitize_github_name; import sys; print(sanitize_github_name(sys.argv[1]))" $GithubRepo).Trim()

if (-not $PSBoundParameters.ContainsKey("Description")) {
    if ($interactive) { $Description = Prompt-Line "Short game description (optional)" "" }
}
if (-not $PSBoundParameters.ContainsKey("Publisher")) {
    if ($interactive) { $Publisher = Prompt-Line "Publisher (optional)" "" }
}
if (-not $PSBoundParameters.ContainsKey("Year")) {
    if ($interactive) { $Year = Prompt-Line "Release year (optional)" "" }
}
if (-not $Region) {
    if ($interactive) { $Region = Prompt-Line "Region" "USA" } else { $Region = "USA" }
}

# Tri-state: CLI enable/no, else prompt, else -Yes / non-interactive default off
# (wizard is forced ON below when recomp-ui is enabled unless -NoWizard).
function Resolve-BoolOpt([switch]$Enable, [switch]$No, [bool]$PromptDefault, [string]$Question) {
    if ($No) { return $false }
    if ($Enable) { return $true }
    if ($interactive) { return (Prompt-Yn $Question $PromptDefault) }
    return $false
}

$useRecompUi = Resolve-BoolOpt -Enable:$EnableRecompUi -No:$NoRecompUi -PromptDefault:$true `
    -Question "Include recomp-ui launcher submodule?"

$useWizard = $false
$useNetplay = $false
if (-not $useRecompUi) {
    if ($EnableWizard -or $EnableNetplay) {
        Write-Warning "Wizard/netplay require recomp-ui -- forcing both off."
    }
    Write-Host "  (recomp-ui declined -- skipping wizard/netplay; PSX_RECOMP_UI=OFF)"
} else {
    # Default ON (interactive + non-interactive): setup-host CI needs the wizard.
    $useWizard = Resolve-BoolOpt -Enable:$EnableWizard -No:$NoWizard -PromptDefault:$true `
        -Question "Enable first-run setup wizard + Generate & rebuild?"
    if (-not $interactive -and -not $NoWizard) { $useWizard = $true }
    # 1-player titles cannot use multiplayer netplay -- skip prompts entirely.
    if ($Players -eq 1) {
        if ($EnableNetplay) {
            Write-Warning "-EnableNetplay ignored for 1-player title."
        }
        $useNetplay = $false
        Write-Host "  (1-player title -- netplay skipped)"
    } else {
        $useNetplay = Resolve-BoolOpt -Enable:$EnableNetplay -No:$NoNetplay -PromptDefault:$false `
            -Question "Enable netplay UI (needs nested recomp-net)?"
    }
}

$NetplayLobbyWs = ""
if ($useNetplay) {
    if (-not $LobbyUrl) {
        if ($interactive) {
            $LobbyUrl = Prompt-Line "Netplay lobby server URL (host or ws://...)" $DefaultLobbyHost
        } else {
            $LobbyUrl = $DefaultLobbyHost
        }
    }
    $NetplayLobbyWs = Normalize-LobbyUrl $LobbyUrl
}

$useCi = Resolve-BoolOpt -Enable:$EnableCi -No:$NoCi -PromptDefault:$true `
    -Question "Add GitHub Actions release workflow (Linux/Windows/macOS)?"
$doBoxart = Resolve-BoolOpt -Enable:$FetchBoxart -No:$NoFetchBoxart -PromptDefault:$true `
    -Question "Fetch libretro boxart now? (needs network)"
$doGenerate = Resolve-BoolOpt -Enable:$Generate -No:$NoGenerate -PromptDefault:$false `
    -Question "Run Generate now (emitters + OpenBIOS + game C)?"

$doBuild = $false
if ($doGenerate) {
    $doBuild = Resolve-BoolOpt -Enable:$EnableBuild -No:$NoBuild -PromptDefault:$true `
        -Question "Configure & build psx-runtime after Generate?"
} elseif ($EnableBuild) {
    Write-Warning "-EnableBuild requires Generate -- enabling Generate"
    $doGenerate = $true
    $doBuild = $true
}

$createGithub = Resolve-BoolOpt -Enable:$CreateGithub -No:$NoGithub -PromptDefault:$false `
    -Question "Create GitHub repo with gh (needs auth)?"

if ($useCi -and $useRecompUi -and -not $useWizard) {
    throw "Release CI requires the first-run setup wizard (omit -NoWizard, or pass -EnableWizard)."
}

if ($Bios -and -not (Test-Path -LiteralPath $Bios)) { throw "-Bios not found: $Bios" }
if ($Bios -and -not $doGenerate) { Write-Warning "-Bios ignored without Generate" }

$ProjectCmakeName = ($Name -replace '[^A-Za-z0-9_]', '_')
$EnvPrefix = $ProjectCmakeName.ToUpperInvariant()
$WindowTitle = $Name -replace 'Recomp$', ' Recompiled'
$WindowTitle = $WindowTitle -replace 'Recompiled Recompiled', 'Recompiled'
# Match runtime.cmake OUTPUT_NAME = MAKE_C_IDENTIFIER(WINDOW_TITLE).
$ExeBasename = ($WindowTitle -replace '[^A-Za-z0-9_]', '_')
$GameName = ($Name -replace 'Recomp$', '') -replace 'Recompiled$', ''
$GameName = $GameName.Trim()
if (-not $GameName) { $GameName = $Name }
$DiscHint = "your legally owned $GameName disc"
$EntryPc = "0x80010000"
$PublisherDisp = if ($Publisher) { $Publisher } else { "-" }
$YearDisp = if ($Year) { $Year } else { "-" }
$DescriptionMd = if ($Description) { $Description } else { "_Add a short pitch in catalog_identity.json / README._" }

# Folder = GitHub/catalog install_dir slug (hyphenated), not display $Name with spaces.
$InstallDirName = $GithubRepo
$Root = Join-Path (Resolve-Path $Dir) $InstallDirName
if (Test-Path $Root) { throw "Target already exists: $Root" }
if ($InstallDirName -ne $Name) {
    Write-Host "note: project folder is '$InstallDirName' (catalog/GitHub slug; display name stays '$Name')"
}

$NetplayBlockFile = Join-Path $env:TEMP "psxrecomp_netplay_block.cmake"
$WizardBlockFile = Join-Path $env:TEMP "psxrecomp_wizard_block.cmake"
$BoxartBlockFile = Join-Path $env:TEMP "psxrecomp_boxart_block.cmake"
$RecompUiBlockFile = Join-Path $env:TEMP "psxrecomp_recomp_ui_block.cmake"
$CodegenArgFile = Join-Path $env:TEMP "psxrecomp_codegen_arg.cmake"

if ($useRecompUi) {
    Set-Content -Encoding UTF8 -Path $RecompUiBlockFile -Value "# recomp-ui submodule present (PSX_RECOMP_UI defaults ON)."
    Set-Content -Encoding UTF8 -Path $CodegenArgFile -Value '    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"'
} else {
    Set-Content -Encoding UTF8 -Path $RecompUiBlockFile -Value @"
set(PSX_RECOMP_UI OFF CACHE BOOL
    "No recomp-ui submodule in this scaffold" FORCE)
"@
    Set-Content -Encoding UTF8 -Path $CodegenArgFile -Value "    # CODEGEN_SETUP_SOURCES -- add with recomp-ui / wizard later"
}

if ($useNetplay) {
    # PSX_NETPLAY defaults RNET_ENABLE_ICE=ON; recomp-net FetchContents
    # libjuice via pinned URL (not git) so RetComM AppImage builds configure.
    Set-Content -Encoding UTF8 -Path $NetplayBlockFile -Value @"
if(EXISTS "`${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
    set(PSX_NETPLAY ON CACHE BOOL
        "Link recomp-net delay-sync (opt-in; needs recomp-net)" FORCE)
endif()
"@
    $NetplayRuntimeArg = "    ENABLE_NETPLAY_IF_PRESENT"
    $NetplayLobbyUrlArg = "    NETPLAY_LOBBY_URL `"$NetplayLobbyWs`""
} else {
    Set-Content -Encoding UTF8 -Path $NetplayBlockFile -Value @"
# Full netplay UI -- enable after adding recomp-ui + testing:
# if(EXISTS "`${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "..." FORCE)
# endif()
"@
    $NetplayRuntimeArg = "    # ENABLE_NETPLAY_IF_PRESENT"
    $NetplayLobbyUrlArg = "    # NETPLAY_LOBBY_URL `"ws://${DefaultLobbyHost}:8765`""
}

if ($useWizard) {
    Set-Content -Encoding UTF8 -Path $WizardBlockFile -Value @"
set(PSX_SETUP_WIZARD ON CACHE BOOL
    "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" FORCE)
"@
    $WizardRuntimeArg = "    ENABLE_SETUP_WIZARD"
} else {
    Set-Content -Encoding UTF8 -Path $WizardBlockFile -Value @"
# First-run setup wizard -- enable after testing:
# set(PSX_SETUP_WIZARD ON CACHE BOOL "..." FORCE)
"@
    $WizardRuntimeArg = "    # ENABLE_SETUP_WIZARD"
}

Set-Content -Encoding UTF8 -Path $BoxartBlockFile -Value '    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"'
$HasBoxart = $false
$AppIconBlockFile = Join-Path $env:TEMP "psxrecomp_app_icon_block.cmake"
Set-Content -Encoding UTF8 -Path $AppIconBlockFile -Value '    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"'

function Fill-Template([string]$src, [string]$dst) {
    python $FillTokens $src $dst `
        --set "PROJECT_CMAKE_NAME=$ProjectCmakeName" `
        --set "WINDOW_TITLE=$WindowTitle" `
        --set "BOOT_EXE=$BootExe" `
        --set "GAME_NAME=$GameName" `
        --set "ENV_PREFIX=$EnvPrefix" `
        --set "EXE_BASENAME=$ExeBasename" `
        --set "ZIP_PREFIX=$ZipPrefix" `
        --set "GITHUB_OWNER=$GithubOwner" `
        --set "GITHUB_REPO=$GithubRepo" `
        --set "DISC_HINT=$DiscHint" `
        --set "GAME_TITLE=$WindowTitle" `
        --set "PLAYERS=$Players" `
        --set "DESCRIPTION=$DescriptionMd" `
        --set "PUBLISHER=$PublisherDisp" `
        --set "YEAR=$YearDisp" `
        --set "REGION=$Region" `
        --set "ENTRY_PC=$EntryPc" `
        --set "NETPLAY_RUNTIME_ARG=$NetplayRuntimeArg" `
        --set "NETPLAY_LOBBY_URL_ARG=$NetplayLobbyUrlArg" `
        --set "WIZARD_RUNTIME_ARG=$WizardRuntimeArg" `
        --set-file "BOXART_CMAKE_ARG=$BoxartBlockFile" `
        --set-file "APP_ICON_CMAKE_ARG=$AppIconBlockFile" `
        --set-file "NETPLAY_CMAKE_BLOCK=$NetplayBlockFile" `
        --set-file "WIZARD_CMAKE_BLOCK=$WizardBlockFile" `
        --set-file "RECOMP_UI_CMAKE_BLOCK=$RecompUiBlockFile" `
        --set-file "CODEGEN_SETUP_ARG=$CodegenArgFile"
}

Write-Host "== New Project Layout =="
Write-Host "  repo:       $Root"
Write-Host "  disc:       $Disc"
Write-Host "  zip prefix: $ZipPrefix"
Write-Host "  gh slug:    $GithubOwner/$GithubRepo"
Write-Host "  players:    $Players"
Write-Host "  recomp-ui:  $useRecompUi"
Write-Host "  wizard:     $useWizard"
Write-Host "  netplay:    $useNetplay"
if ($useNetplay) { Write-Host "  lobby URL:  $NetplayLobbyWs" }
Write-Host "  CI:         $useCi"
Write-Host "  boxart:     $doBoxart"
Write-Host "  generate:   $doGenerate"
Write-Host "  build:      $doBuild"
Write-Host "  github:     $createGithub"
Write-Host "  region:     $Region"

New-Item -ItemType Directory -Path $Root | Out-Null
Set-Location $Root
git init -q

Fill-Template (Join-Path $TemplateDir "CMakeLists.txt.in") (Join-Path $Root "CMakeLists.txt")
Fill-Template (Join-Path $TemplateDir "game.toml.in") (Join-Path $Root "game.toml")
Fill-Template (Join-Path $TemplateDir "game_options.toml.in") (Join-Path $Root "game_options.toml")
Fill-Template (Join-Path $TemplateDir "codegen_setup.c.in") (Join-Path $Root "codegen_setup.c")
Fill-Template (Join-Path $TemplateDir "codegen_setup.h.in") (Join-Path $Root "codegen_setup.h")
Fill-Template (Join-Path $TemplateDir "gitignore.in") (Join-Path $Root ".gitignore")
Fill-Template (Join-Path $TemplateDir "VERSION.in") (Join-Path $Root "VERSION")
Fill-Template (Join-Path $TemplateDir "README.md.in") (Join-Path $Root "README.md")
Fill-Template (Join-Path $TemplateDir "symbols.toml.in") (Join-Path $Root "symbols.toml")
New-Item -ItemType Directory -Force -Path (Join-Path $Root "seeds") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root "launcher_assets\img") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root "scripts") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root "tools") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root "mods\preloaded\packages") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root "assets") | Out-Null
# Empty mod catalog tree (runtime copies mods/preloaded -> beside the exe as mods/).
@'
# Preloaded mods

Ship reviewed, default-disabled packages here:

```text
packages/<package-id>/<version>/
  manifest.toml
  ...
```

Build wiring copies `mods/preloaded` next to the game executable as `mods/`.
Install player `.psxmod` archives through the launcher Mods manager instead of
committing them here. See `psxrecomp/docs/MOD_PACKAGES.md`.
'@ | Set-Content -Encoding utf8 (Join-Path $Root "mods\preloaded\README.md")
New-Item -ItemType File -Force -Path (Join-Path $Root "mods\preloaded\packages\.gitkeep") | Out-Null
Copy-Item (Join-Path $ScriptDir "sync_symbols.py") (Join-Path $Root "tools\sync_symbols.py")

Write-Host "== Adding submodules =="
git submodule add -b $PsxrecompRef $PsxrecompUrl psxrecomp
if ($useRecompUi) {
    git submodule add -b $RecompUiRef $RecompUiUrl recomp-ui
}
git submodule update --init --recursive

# RetComM-themed default app icon (Windows .ico + PNG for packaging).
$iconSrc = Join-Path $Root "psxrecomp\assets"
$iconDst = Join-Path $Root "assets"
if (Test-Path $iconSrc) {
    New-Item -ItemType Directory -Force -Path $iconDst | Out-Null
    foreach ($name in @("psxrecomp.svg", "psxrecomp.png", "psxrecomp.ico")) {
        $src = Join-Path $iconSrc $name
        if (Test-Path $src) {
            Copy-Item -Force $src (Join-Path $iconDst $name)
        }
    }
}

git -C psxrecomp checkout --detach -q HEAD
git add psxrecomp
if (Test-Path $iconDst) {
    git add assets 2>$null
}
if ($useRecompUi) {
    git -C recomp-ui checkout --detach -q HEAD
    git add recomp-ui
}
if (Test-Path "psxrecomp\lib\recomp-net") {
    git -C psxrecomp/lib/recomp-net checkout --detach -q HEAD 2>$null
}

Write-Host "== Framework pins =="
$PinsPath = Join-Path $Root "framework_pins.txt"
if (Test-Path "psxrecomp\tools\ci\record_pins.sh") {
    bash psxrecomp/tools/ci/record_pins.sh --root $Root | Tee-Object -FilePath $PinsPath
} else {
    $lines = @("psxrecomp=$(git -C psxrecomp rev-parse HEAD)")
    if ($useRecompUi) {
        $lines += "recomp-ui=$(git -C recomp-ui rev-parse HEAD)"
    } else {
        $lines += "recomp-ui=<not added>"
    }
    $lines | Tee-Object -FilePath $PinsPath
}

if ($useNetplay -and -not (Test-Path "psxrecomp\lib\recomp-net\CMakeLists.txt")) {
    Write-Warning "Netplay enabled but psxrecomp/lib/recomp-net missing after recursive update."
}

Write-Host "== Packager stub =="
Fill-Template (Join-Path $TemplateDir "package_setup_release.sh.in") (Join-Path $Root "scripts\package_setup_release.sh")

if ($useCi) {
    Write-Host "== CI release workflow =="
    $WfSrc = Join-Path $Root "psxrecomp\docs\ci\templates\setup-release.yml"
    $WfDst = Join-Path $Root ".github\workflows\release.yml"
    if (Test-Path -LiteralPath $WfSrc) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $WfDst) | Out-Null
        python $FillTokens $WfSrc $WfDst `
            --ci-placeholders `
            --set "ZIP_PREFIX=$ZipPrefix" `
            --set "GAME_TITLE=$WindowTitle" `
            --set "WINDOW_TITLE=$WindowTitle"
        Write-Host "  wrote .github/workflows/release.yml"
    } else {
        Write-Warning "Missing $WfSrc -- skipped CI workflow copy"
    }
} else {
    Write-Host "== CI workflow skipped =="
}

$DiscAbs = (Resolve-Path -LiteralPath $Disc).Path
$DiscBasename = Split-Path -Leaf $DiscAbs
if ($DiscBasename -notmatch '\.cue$') {
    Write-Warning "Prefer a Redump-style .cue (with sibling .bin tracks)."
}
$DiscOut = Join-Path $Root "disc"
New-Item -ItemType Directory -Force -Path $DiscOut | Out-Null

$ProbeCue = $DiscAbs
$DiscTomlPath = $DiscAbs
$GenDiscHint = $DiscAbs

if ($StageDisc) {
    Write-Host "== Staging full disc dump into disc/ (optional; large) =="
    $py = @'
import re, shutil, sys
from pathlib import Path
cue = Path(sys.argv[1])
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
shutil.copy2(cue, out / cue.name)
print(f"  copied {cue.name}")
text = cue.read_text(encoding="utf-8", errors="ignore")
for m in re.finditer(r'FILE\s+"([^"]+)"', text, re.I):
    src = cue.parent / m.group(1)
    if src.is_file():
        dst = out / src.name
        if not dst.exists():
            shutil.copy2(src, dst)
            print(f"  copied track: {src.name}")
    else:
        print(f"  warning: cue FILE missing: {src}", file=sys.stderr)
print("  disc staged under disc/ (gitignored -- never commit dumps)")
'@
    $pyFile = Join-Path $env:TEMP "psxrecomp_stage_disc.py"
    Set-Content -Encoding UTF8 -Path $pyFile -Value $py
    python $pyFile $DiscAbs $DiscOut
    $ProbeCue = Join-Path $DiscOut $DiscBasename
    $DiscTomlPath = "disc/$DiscBasename"
    } else {
    Write-Host "== External disc (no full copy) =="
    Write-Host "  cue remains at: $DiscAbs"
    Write-Host "  will extract boot EXE only into disc/"
}

$Probed = $false
$SeedCount = 0
if ($DiscBasename -match '\.cue$') {
    Write-Host "== Probing disc (identity + seeds + TOC fp) =="
    try {
        python $ProbeDisc $ProbeCue `
            --json-out (Join-Path $Root "disc_probe.json") `
            --write-game-toml (Join-Path $Root "game.toml") `
            --write-catalog (Join-Path $Root "catalog_identity.json") `
            --write-seeds (Join-Path $Root "seeds\ghidra_funcs.txt") `
            --write-boot-exe $DiscOut `
            --disc-rel $DiscTomlPath `
            --out-dir disc `
            --players $Players `
            --display-name $GameName `
            --description $Description `
            --publisher $Publisher `
            --year $Year `
            --region $Region
        $Probed = $true
        $SeedCount = (Select-String -Path (Join-Path $Root "seeds\ghidra_funcs.txt") -Pattern '^0x').Count
        $BootFromToml = python -c @"
import re,sys
t=open(sys.argv[1],encoding='utf-8').read()
m=re.search(r'boot_exe\s*=\s*\"([^\"]+)\"', t)
print(m.group(1) if m else '')
"@ (Join-Path $Root "game.toml")
        $BootFromToml = $BootFromToml.Trim()
        $EntryFromToml = python -c @"
import re,sys
t=open(sys.argv[1],encoding='utf-8').read()
m=re.search(r'entry_pc\s*=\s*\"([^\"]+)\"', t)
print(m.group(1) if m else '')
"@ (Join-Path $Root "game.toml")
        $EntryFromToml = $EntryFromToml.Trim()
        if ($BootFromToml -and $BootFromToml -ne $BootExe) {
            $BootExe = $BootFromToml
            Fill-Template (Join-Path $TemplateDir "CMakeLists.txt.in") (Join-Path $Root "CMakeLists.txt")
            Fill-Template (Join-Path $TemplateDir "codegen_setup.c.in") (Join-Path $Root "codegen_setup.c")
            Write-Host "  synced boot EXE -> $BootExe (CMake / codegen)"
        }
        if ($EntryFromToml) {
            $EntryPc = $EntryFromToml
            Fill-Template (Join-Path $TemplateDir "symbols.toml.in") (Join-Path $Root "symbols.toml")
            Write-Host "  symbols.toml BootEntry -> $EntryPc"
        }
        if (-not $StageDisc) {
            Write-Host "  game.toml disc -> $DiscTomlPath"
            Write-Host "  (local absolute path -- do not commit machine-specific dumps)"
        }
    } catch {
        Write-Warning "Disc probe failed -- left template game.toml; fill by hand."
    }
} else {
    Write-Warning "-Disc is not a .cue; skipped probe autofill."
}

if ($doBoxart) {
    Write-Host "== Fetching libretro boxart =="
    $cueHint = if ($DiscBasename) { $DiscBasename } else { $GameName }
    $prevPy = $env:PYTHONPATH
    try {
        python $FetchBoxartPy `
            --out (Join-Path $Root "launcher_assets\img\boxart.tga") `
            --cue-stem $cueHint `
            --display-name $GameName
        if ($LASTEXITCODE -eq 0) {
            $HasBoxart = $true
            Set-Content -Encoding UTF8 -Path $BoxartBlockFile -Value '    LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"'
            Fill-Template (Join-Path $TemplateDir "CMakeLists.txt.in") (Join-Path $Root "CMakeLists.txt")
            Write-Host "  wired LAUNCHER_BOXART in CMakeLists.txt"
            $env:PYTHONPATH = $ScriptDir
            python -c "from pathlib import Path; import sys; from project_studio.readme_metrics import inject_readme_boxart; inject_readme_boxart(Path(sys.argv[1]), sys.argv[2])" (Join-Path $Root "README.md") $GameName
            Write-Host "  injected boxart into README.md"
        } else {
            Write-Warning "boxart fetch failed — leave LAUNCHER_BOXART commented."
        }
    } catch {
        Write-Warning "boxart fetch failed — leave LAUNCHER_BOXART commented."
    } finally {
        if ($null -eq $prevPy) { Remove-Item Env:PYTHONPATH -ErrorAction SilentlyContinue } else { $env:PYTHONPATH = $prevPy }
    }
}

Write-Host "== Sync symbols header =="
Push-Location $Root
try { python tools/sync_symbols.py --game $GameName } catch { Write-Warning "sync_symbols failed" }
Pop-Location

git add CMakeLists.txt game.toml codegen_setup.c codegen_setup.h .gitignore VERSION README.md seeds scripts tools mods framework_pins.txt symbols.toml psx_symbols.h 2>$null
if (Test-Path (Join-Path $Root ".github")) {
    git add .github 2>$null
}
if (Test-Path (Join-Path $Root "catalog_identity.json")) {
    git add catalog_identity.json 2>$null
}
if (Test-Path (Join-Path $Root "disc_probe.json")) {
    git add disc_probe.json 2>$null
}
if ($HasBoxart) {
    git add launcher_assets/img/boxart.tga launcher_assets/img/boxart.png launcher_assets/img/BOXART_SOURCE.txt 2>$null
}
$committed = $false
try {
    git -c user.email=setup@localhost -c user.name=setup `
        commit -q -m "Initial New Project Layout scaffold"
    $committed = $true
} catch {
    Write-Host "  (skipped initial commit -- commit manually when ready)"
}

# Create the GitHub repo + origin now; push deferred until after generate/build.
$githubCreated = $false
if ($createGithub) {
    Write-Host "== GitHub repo (gh, create only -- push deferred) =="
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        Write-Warning "gh not installed -- skip create; push manually later."
    } elseif (-not $committed) {
        Write-Warning "no initial commit -- skip gh repo create."
    } else {
        $vis = switch ($GithubVisibility) {
            "public" { "--public" }
            "internal" { "--internal" }
            default { "--private" }
        }
        $ghSlug = "$GithubOwner/$GithubRepo"
        $ghDesc = (python -c "from fill_tokens import GITHUB_ABOUT_DESCRIPTION; print(GITHUB_ABOUT_DESCRIPTION)").Trim()
        $ghHome = (python -c "from fill_tokens import GITHUB_ABOUT_HOMEPAGE; print(GITHUB_ABOUT_HOMEPAGE)").Trim()
        try {
            gh repo create $ghSlug $vis --source=$Root --remote=origin --description "$ghDesc" --homepage "$ghHome"
            Write-Host "  created origin ($GithubVisibility); push deferred until end"
            $githubCreated = $true
        } catch {
            Write-Warning "gh repo create failed -- if the repo already exists, add origin and push at the end."
            if (-not (git remote get-url origin 2>$null)) {
                try {
                    $ghUrl = (gh repo view $ghSlug --json url -q .url 2>$null)
                    if ($ghUrl) {
                        git remote add origin $ghUrl
                        Write-Host "  attached existing origin -> $ghUrl"
                        $githubCreated = $true
                    }
                } catch {}
            } else {
                $githubCreated = $true
            }
        }
        if ($githubCreated) {
            try {
                gh repo edit $ghSlug --description "$ghDesc" --homepage "$ghHome"
                Write-Host "  GitHub About: R.A.I.D. description + Discord homepage"
            } catch {
                Write-Warning "gh repo edit (About) failed — set description/homepage by hand."
            }
        }
    }
}

$GeneratedOk = $false
$BuildOk = $false
if ($doGenerate) {
    Write-Host "== Generate (emitters + OpenBIOS + game C) =="
    Push-Location $Root
    try {
        bash psxrecomp/tools/ci/build_emitters.sh
        $genArgs = @(
            "psxrecomp\psxrecomp_cli.py", "generate",
            "--config", "game.toml",
            "--project-root", ".",
            "--disc", $GenDiscHint
        )
        if ($Bios) {
            $BiosAbs = (Resolve-Path -LiteralPath $Bios).Path
            $genArgs += @("--bios", $BiosAbs)
        }
        python @genArgs
        $GeneratedOk = $true
        Write-Host "  generate OK (generated/ is gitignored -- not committed)"
    } catch {
        Write-Warning "Generate failed -- fix seeds/disc and re-run generate by hand."
        $doBuild = $false
    } finally {
        Pop-Location
    }
}

if ($doBuild -and $GeneratedOk) {
    Write-Host "== Configure & build psx-runtime =="
    Push-Location $Root
    try {
        cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build build-release --target psx-runtime
        $BuildOk = $true
        Write-Host "  build OK -> build-release/ (gitignored)"
    } catch {
        Write-Warning "Build failed -- fix and re-run cmake by hand."
    } finally {
        Pop-Location
    }
}

function Get-ReleaseWorkflowName([string]$OwnerRepo) {
    try {
        return (gh api "repos/$OwnerRepo/actions/workflows" `
            --jq '.workflows[] | select(.path|endswith("release.yml")) | .name' 2>$null |
            Select-Object -First 1)
    } catch { return $null }
}

function Ensure-ActionsRegistersReleaseYml([string]$OwnerRepo) {
    $wfPath = Join-Path $Root ".github\workflows\release.yml"
    if (-not (Test-Path -LiteralPath $wfPath)) { return }
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { return }

    if ($GithubVisibility -eq "public") {
        try {
            gh repo edit $OwnerRepo --visibility public --accept-visibility-change-consequences 2>$null | Out-Null
        } catch {}
    }

    for ($i = 0; $i -lt 5; $i++) {
        $wf = Get-ReleaseWorkflowName $OwnerRepo
        if ($wf) {
            Write-Host "  Actions workflow registered: $wf"
            Write-Host "  (runs on workflow_dispatch or push of v* tags)"
            return
        }
        if ($i -lt 4) { Start-Sleep -Seconds 2 }
    }

    Write-Host "  Actions has not listed release.yml yet -- nudging with a follow-up commit..."
    $stamp = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mmZ")
    Add-Content -Encoding utf8 -Path $wfPath -Value "`n# Actions registration nudge ($stamp UTC)"
    git add $wfPath 2>$null
    try {
        git -c user.email=setup@localhost -c user.name=setup `
            commit -q -m "ci: nudge Actions to register release.yml"
        git push origin HEAD
        for ($i = 0; $i -lt 6; $i++) {
            $wf = Get-ReleaseWorkflowName $OwnerRepo
            if ($wf) {
                Write-Host "  Actions workflow registered after nudge: $wf"
                Write-Host "  (runs on workflow_dispatch or push of v* tags)"
                return
            }
            if ($i -lt 5) { Start-Sleep -Seconds 2 }
        }
        Write-Warning "release.yml nudged but Actions still has not listed it; open the Actions tab once."
    } catch {
        Write-Warning "Actions nudge commit/push failed."
    }
}

$githubPushed = $false
if ($createGithub -and (git remote get-url origin 2>$null)) {
    Write-Host "== GitHub push (final) =="
    if (-not $committed) {
        Write-Warning "no local commit to push."
    } else {
        try {
            git push -u origin HEAD
            $githubPushed = $true
            Write-Host "  pushed $(git rev-parse --abbrev-ref HEAD) -> origin"
            if ($useCi -and (Test-Path (Join-Path $Root ".github\workflows\release.yml"))) {
                try {
                    $ownerRepo = (gh repo view --json nameWithOwner -q .nameWithOwner 2>$null)
                    if ($ownerRepo) { Ensure-ActionsRegistersReleaseYml $ownerRepo }
                } catch {}
            }
        } catch {
            Write-Warning "git push failed -- fetch/rebase or force-push manually if origin already has a different initial commit."
        }
    }
}

$Step1 = if ($Probed) {
    "Review game.toml + catalog_identity.json + seeds/ghidra_funcs.txt ($SeedCount JAL seeds)."
} else {
    "Edit game.toml / re-run probe_disc.py --write-seeds."
}
$Step2 = if ($BuildOk) {
    "Runtime already built under build-release/ -- soak offline boot."
} elseif ($GeneratedOk) {
@"
Build playable runtime:

       cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
       cmake --build build-release --target psx-runtime
"@
} else {
@"
Build emitters, Generate, then playable runtime:

       bash psxrecomp/tools/ci/build_emitters.sh
       python psxrecomp\psxrecomp_cli.py generate --config game.toml --project-root . --disc $GenDiscHint
       cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
       cmake --build build-release --target psx-runtime
"@
}
$CiNote = if ($useCi) {
    $n = "CI: .github/workflows/release.yml ready (zip prefix=$ZipPrefix; submodule gitlinks pin the build)."
    if ($githubPushed) { $n += " Pushed -- open Actions -> Release builds (workflow_dispatch)." }
    $n
} else {
    "CI workflow not installed (declined)."
}
$GhNote = if ($createGithub) {
    if ($githubPushed) { "GitHub: created/attached origin and pushed ($GithubVisibility)." }
    elseif ($githubCreated) { "GitHub: origin ready but push failed -- see warnings above." }
    else { "GitHub: create/push did not complete -- check gh auth / remote." }
} else {
    "GitHub remote not created (declined)."
}

Write-Host @"

== Done ==

Next steps:
  1. $Step1
  2. $Step2
  3. Label symbols in symbols.toml -> python tools/sync_symbols.py
  4. Soak offline boot -> netplay QA (if enabled) -> tag vX.Y.Z.
  5. $CiNote
     Pins: submodule gitlinks (framework_pins.txt is an optional snapshot)
  6. $GhNote

Docs: psxrecomp\docs\GAME_PROJECT_SETUP.md ; psxrecomp\docs\SYMBOLS.md
"@
