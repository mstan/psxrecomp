param(
    [ValidateSet('full', 'hybrid')]
    [string]$Mode = 'hybrid',
    [switch]$Launch,
    [int]$DebugPort = 4615,
    [string]$GameRoot = '',
    # Which isolated build variant to run. 'build-tomba2-interp-iso-rel' is the
    # production-config measurement (PSX_DEBUG_TOOLS=OFF: no TCP server, FPS
    # only via the stderr log); 'build-tomba2-interp-iso-dbg' adds the TCP
    # debug surface (screenshots/telemetry/input) at some hot-path overhead.
    # Each variant keeps its OWN cache dir, so cold-cache runs stay repeatable.
    [string]$BuildDir = 'build-tomba2-interp-iso-rel'
)

$ErrorActionPreference = 'Stop'

if ($DebugPort -lt 1 -or $DebugPort -gt 65535) {
    throw "DebugPort must be in the range 1..65535 (received $DebugPort)."
}

function Quote-ProcessArgument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

# Default GameRoot is the ISOLATED game worktree (_wt-tomba2-interp-perf,
# branch interp/game-isolated, forked from Tomba2Recomp's validated master),
# NOT the shared F:\Projects\psxrecomp\Tomba2Recomp checkout — that checkout's
# HEAD belongs to the AOT overlay-spike investigation (branch
# feat/aot-overlay-spike, uncommitted AOT files) and its game.toml has already
# drifted from master (adds overlay_capture_history). Reading game.toml from
# there would silently reintroduce the sharing this worktree split was meant
# to end. -GameRoot still accepts an explicit override for one-off comparisons.
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workspaceRoot = Split-Path $repoRoot -Parent
if (-not $GameRoot) {
    $GameRoot = Join-Path $workspaceRoot '_wt-tomba2-interp-perf'
}
$GameRoot = (Resolve-Path $GameRoot).Path

# build-tomba2-interp-iso-rel is configured from the ISOLATED game worktree
# (_wt-tomba2-interp-perf, branch interp/game-isolated) with
# -DPSXRECOMP_GAME_EXE_NAME_OVERRIDE=Tomba2InterpPerf, so the build emits
# Tomba2InterpPerf.exe natively — there is no more Tomba2Recomp.exe in this
# build dir to alias. The old build-tomba2-perf-rel dir (shared checkout,
# CMAKE_HOME_DIRECTORY=Tomba2Recomp) built plain Tomba2Recomp.exe and this
# script used to Copy-Item a uniquely-named alias next to it so process-name
# tooling couldn't confuse it with another investigation's run; that copy
# step is gone because the isolated build dir + isolated exe name make the
# alias unnecessary; build-tomba2-perf-rel itself is left untouched as
# evidence of the original shared-checkout measurement.
$buildDir = Join-Path $repoRoot (Join-Path 'runtime' $BuildDir)
$isolatedExe = Join-Path $buildDir 'Tomba2InterpPerf.exe'
$gameConfig = Join-Path $GameRoot 'game.toml'
$bios = Join-Path $workspaceRoot 'psxrecomp\bios\SCPH1001.BIN'
$recompiler = Join-Path $repoRoot 'recompiler\build-interpreter-perf-native\psxrecomp-game.exe'
$compileTool = Join-Path $repoRoot 'tools\compile_overlays.py'
$runtimeInclude = Join-Path $repoRoot 'runtime\include'

foreach ($required in @($isolatedExe, $gameConfig, $bios, $recompiler, $compileTool)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required launch input is missing: $required"
    }
}

# Refuse to launch if EITHER this isolated build OR the shared-checkout
# Tomba2Recomp build is already running. The two now build from separate
# worktrees/exe names, so they can no longer collide on-disk, but they can
# still collide at runtime (same debug port range, same memcard/BIOS files
# read concurrently, same taskkill-by-name cleanup a human runs by hand) —
# so the guard stays broad on purpose rather than narrowing to just our own
# process name.
foreach ($conflictingName in @('Tomba2InterpPerf', 'Tomba2Recomp')) {
    if ($Launch -and (Get-Process -Name $conflictingName -ErrorAction SilentlyContinue)) {
        throw "$conflictingName is already running; refusing to start another instance (they share BIOS/debug-port/taskkill surface)."
    }
}

$sessionRoot = Join-Path $repoRoot '.local\tomba2-interp-perf'
$stateDir = Join-Path $sessionRoot 'state'
$capturePath = Join-Path $sessionRoot 'captures\SCUS-94454\overlay_captures.json'
$runDir = Join-Path $sessionRoot (Join-Path 'run' $Mode)
$logDir = Join-Path $sessionRoot 'logs'
$cacheDir = Join-Path $buildDir 'cache'
foreach ($dir in @($stateDir, (Split-Path $capturePath -Parent), $runDir, $logDir, $cacheDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

# Windows Python EXPLICITLY (never unqualified 'python'): the game process
# inherits whatever PATH the launching shell had, and an MSYS python first on
# PATH cannot open the Windows-style script path — every autocompile run then
# fails with "can't open file" and the reshard silently never happens.
$windowsPython = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
if (-not (Test-Path -LiteralPath $windowsPython -PathType Leaf)) {
    throw "Windows Python not found at $windowsPython (required for autocompile)."
}
$autoCompile = @(
    (Quote-ProcessArgument $windowsPython), (Quote-ProcessArgument $compileTool),
    '--captures', (Quote-ProcessArgument $capturePath),
    '--game-toml', (Quote-ProcessArgument $gameConfig),
    '--recompiler', (Quote-ProcessArgument $recompiler),
    '--runtime-include', (Quote-ProcessArgument $runtimeInclude),
    '--out-dir', (Quote-ProcessArgument $cacheDir)
) -join ' '

$childEnvironment = @{
    PSX_NO_LAUNCHER = '1'
    PSX_OVERLAY_CAPTURES = $capturePath
    PSX_OVERLAY_AUTOCOMPILE_CWD = $GameRoot
    PSX_FRAME_INTERPOLATION = '0'
}
if ($Mode -eq 'full') {
    $childEnvironment.PSX_FORCE_INTERP = '1'
    $childEnvironment.PSX_OVERLAY_NATIVE_OFF = '1'
    # Keep the full-interpreter measurement free of background compilation
    # load. Capture remains additive for later hybrid regeneration.
    $childEnvironment.PSX_OVERLAY_AUTOCOMPILE_OFF = '1'
} else {
    $childEnvironment.PSX_FORCE_INTERP = '0'
    $childEnvironment.PSX_OVERLAY_NATIVE_OFF = '0'
    $childEnvironment.PSX_OVERLAY_AUTOCOMPILE_OFF = '0'
    $childEnvironment.PSX_OVERLAY_BACKEND = 'gcc'
    $childEnvironment.PSX_OVERLAY_AUTOCOMPILE_CMD = $autoCompile
}

$arguments = @(
    '--bios', $bios,
    '--game', $gameConfig,
    '--debug-port', $DebugPort.ToString(),
    '--window-title', "Tomba 2 - $Mode interpreter perf",
    '--memcard-dir', $stateDir,
    '--no-launcher'
)
$argumentLine = ($arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' '

Write-Output "Prepared isolated executable: $isolatedExe"
Write-Output "Mode: $Mode; debug port: $DebugPort; writable state: $stateDir"
Write-Output "Overlay cache: $cacheDir; additive captures: $capturePath"

if (-not $Launch) {
    Write-Output 'Preparation only; no process was launched. Pass -Launch explicitly to start it.'
    exit 0
}

if (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue) {
    if (Get-NetTCPConnection -State Listen -LocalPort $DebugPort -ErrorAction SilentlyContinue) {
        throw "Debug port $DebugPort is already listening; choose another -DebugPort."
    }
}

$savedEnvironment = @{}
try {
    foreach ($name in $childEnvironment.Keys) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $childEnvironment[$name], 'Process')
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $stdout = Join-Path $logDir "$Mode-$stamp.stdout.log"
    $stderr = Join-Path $logDir "$Mode-$stamp.stderr.log"
    $process = Start-Process -FilePath $isolatedExe -ArgumentList $argumentLine `
        -WorkingDirectory $runDir -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -WindowStyle Normal -PassThru
    Write-Output "Launched PID $($process.Id); stdout=$stdout; stderr=$stderr"
} finally {
    foreach ($name in $childEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
