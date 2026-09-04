# release_overlay_stage.ps1 — THE shared overlay-shard release staging surface
# for every psxrecomp title on Windows. Dot-source it from a title's
# tools/package_release.ps1:
#
#   . "$FrameworkRoot\tools\release_overlay_stage.ps1"
#   $CgTag = Get-OverlayCgTag -RecompTools ... -RecompInc ... -GameExe ... -GameToml ...
#   Add-OverlayCache     -GameId "SCUS-94423" -CacheSrcRoot ... -Stage ... -CgTag $CgTag
#   Add-OverlayToolchain -Stage ... -RecompDir ... -RecompTools ... -RecompInc ... -MingwBin ... -DlCache ...
#   Add-ModCatalog       -BuildPath ... -Stage ... -GameModSource ... -FrameworkModSource ...
#
# WHY THIS FILE EXISTS
# --------------------
# Every title used to carry its own hand-copied version of this logic, and the
# copies drifted three ways (measured 2026-09-01):
#
#   * MegaManX6 invented the tcc toolchain tier (a277e56, 2026-06-25, 345 lines).
#   * Ape Escape's packager was created 2026-07-05 by copying that and trimming
#     it to 146 lines. The overlay cache + toolchain staging were among the lines
#     cut. Ape has NEVER contained `overlay_toolchain` or `AllowNoCache` in any
#     commit, so every Ape release ever shipped ran 100% of its overlay
#     dispatches on the dirty-RAM interpreter (measured: disp_native=0,
#     disp_interp=4,480,307 in a single session).
#   * Tomba 2 later added SHA256 pinning (0a9c3a3); MegaManX6 never got it back
#     and kept trusting whatever the mirror served, plus reusing a cached archive
#     forever on a bare Test-Path.
#
# The copies drifted because improvements never flow back between forks AND
# because nothing fails when a title lacks the feature: the runtime silently
# falls back to interpretation, so a stripped packager looks perfectly healthy.
# That is why Add-OverlayCache FAILS by default instead of warning — a missing
# cache has to stop a release, not scroll past in a log.
#
# WHY IT IS NOW A THIN WRAPPER (bead beads-eio.3.102)
# ---------------------------------------------------
# Consolidating the five Windows copies into this one module did not stop the
# defect class, it only halved it. Linux was never consolidated at all: three
# title repos carried a forked tools/package_appimage.sh (403/436/408 lines),
# each hand-building the SAME tag string this module used to build here. When
# the framework added the `_f<flavor>` field, one of those three forks was
# hand-patched and two were not, so two titles' packagers staged zero shards
# from a valid cache and could not produce a Linux release at all.
#
# Two implementations, one per platform, have to be kept in step by review —
# which is exactly the argument that made cache_tag() the fix on the Windows
# side in the first place, and this bug is the proof that review does not hold.
# So the staging logic now lives in ONE place, tools/release_stage.py, which
# both platforms call:
#
#   tools/release_stage.py            the implementation
#   tools/release_overlay_stage.ps1   this file  (Windows packagers)
#   tools/release_overlay_stage.sh    the sh counterpart (AppImage packagers)
#
# The parameter surface of every function below is UNCHANGED, so no title's
# packager needs editing for this. What changed is that none of these functions
# contains a tag format string, a shard filter, an extension list, an arch-abi
# string, or a catalog rule any more.
#
# RULES FOR EDITING THIS FILE
#   * No tag format string. `cache_tag()` in tools/compile_overlays.py is the
#     only place that knows the tag's shape.
#   * If a function here grows real logic, the logic belongs in
#     release_stage.py, where both platforms get it at once.
#
# Keep this file title-agnostic. Anything game-specific belongs in the caller.

# NOTE: deliberately no `Set-StrictMode` here. This file is DOT-SOURCED, so any
# strictness set at its top level applies to the CALLER's scope for the rest of
# that script -- a framework helper must not silently change how a title's
# packager evaluates. (Measured: it broke an unrelated caller epilogue.)

# Path to the shared implementation, resolved relative to THIS file so a title
# never has to know it exists.
$script:PsxReleaseStageTool = Join-Path $PSScriptRoot "release_stage.py"

function New-PsxDir {
    param([Parameter(Mandatory)][string]$Path)
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    return $Path
}

<#
.SYNOPSIS
Run the shared staging tool, failing the caller on a non-zero exit.

.DESCRIPTION
`py -3`, never a bare `python`: on this machine a bare `python` can resolve to a
Cygwin build that SIGSEGVs when spawned from a job, and the failure reads as a
silent fallback rather than an error. Output is passed through so the tool's own
messages (which carry the rebuild recipes) reach the release log verbatim.
#>
function Invoke-PsxReleaseStage {
    # ONE array parameter, always passed by name. PowerShell 5.1 tries to bind
    # any token starting with '-' as a parameter name, so a call written as
    #   Invoke-PsxReleaseStage stage-cache --game-id $Id ...
    # dies with "A parameter cannot be found that matches parameter name
    # 'game-id'". Every caller below therefore builds an array and passes
    # -StageArgs, and the array is splatted into the native `py` call where
    # PowerShell passes each element through verbatim.
    param([Parameter(Mandatory)][string[]]$StageArgs)
    if (-not (Test-Path -LiteralPath $script:PsxReleaseStageTool)) {
        throw ("Shared release staging tool is missing: $($script:PsxReleaseStageTool). " +
               "The pinned framework predates it (bead beads-eio.3.102); bump the " +
               "psxrecomp submodule.")
    }
    $out = & py -3 $script:PsxReleaseStageTool @StageArgs 2>&1
    $code = $LASTEXITCODE
    foreach ($line in $out) { Write-Host $line }
    if ($code -ne 0) {
        throw ("release staging failed (release_stage.py " + $StageArgs[0] +
               " exited $code). See the message above.")
    }
    return $out
}

# Read an integer a subcommand wrote to a temp file. The tool's stdout carries
# human-readable progress; counts come back out-of-band so a wrapper never has
# to parse prose.
function Get-PsxStageCount {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    $text = (Get-Content -LiteralPath $Path -Raw).Trim()
    if ($text -match '^\d+$') { return [int]$text }
    return 0
}

function New-PsxStageTempFile {
    return (Join-Path ([IO.Path]::GetTempPath()) ("psx_stage_" + [guid]::NewGuid().ToString("N")))
}

<#
.SYNOPSIS
Fetch an archive and verify its SHA256 on EVERY use, including cache hits.

.DESCRIPTION
A bare `if (-not (Test-Path $zip)) { download }` trusts whatever the mirror
served the day the cache was first filled, forever. This verifies the cached
copy too, refetching when it does not match. python.org and savannah both 502
periodically, so transient failures retry with backoff rather than losing a
whole release build.

The fetch itself now lives in release_stage.py (`fetch-pinned`), so the Windows
toolchain and the Linux toolchain cannot end up with different retry, cache or
verification behaviour.
#>
function Get-PinnedArchive {
    param(
        [Parameter(Mandatory)][string]$Uri,
        [Parameter(Mandatory)][string]$Sha256,
        [Parameter(Mandatory)][string]$Destination,
        [int]$Retries = 4
    )
    Invoke-PsxReleaseStage -StageArgs @(
        'fetch-pinned', '--url', $Uri, '--sha256', $Sha256,
        '--destination', $Destination, '--retries', "$Retries") | Out-Null
    return $Destination
}

<#
.SYNOPSIS
Derive the codegen cache tag for a release.

.DESCRIPTION
The tag namespaces the shard cache. It is derived from the PACKAGED game.toml,
not the dev one: a cache built against the dev config lands under a different
tag and the shipped runtime silently ignores it. Callers must pass the staged
game.toml for exactly this reason.

-Flavor is the codegen flavor baked into the runtime BINARY being packaged
(overlay_api.h: 0 base, 2 pgxp; runtime/runtime.cmake sets 2 for a PGXP
target). It is NOT platform-dependent -- a Windows and a Linux build of the same
target have the same flavor -- so do not add a platform branch for it. Prefer
-BuildPath/-RuntimeTarget, which reads the value the build itself published and
therefore cannot disagree with the binary being shipped; the -Flavor default of
0 exists only so the already-shipped title packagers keep working unchanged.
#>
function Get-OverlayCgTag {
    param(
        [Parameter(Mandatory)][string]$RecompTools,   # framework tools/ (has compile_overlays.py)
        [Parameter(Mandatory)][string]$RecompInc,     # framework runtime/include
        [Parameter(Mandatory)][string]$GameExe,       # psxrecomp-game.exe
        [Parameter(Mandatory)][string]$GameToml,      # the STAGED game.toml
        [int]$Flavor = 0,
        [string]$BuildPath = "",                      # derive the flavor instead
        [string]$RuntimeTarget = "psx-runtime"
    )
    # $RecompTools is accepted for source compatibility with every existing
    # caller. The tool is found relative to THIS file, which is in that same
    # directory, so the parameter is no longer load-bearing.
    $stageArgs = @('cg-tag', '--runtime-include', $RecompInc,
                   '--recompiler', $GameExe, '--game-toml', $GameToml)
    if ($BuildPath) {
        $stageArgs += @('--flavor-from-build', $BuildPath,
                        '--runtime-target', $RuntimeTarget)
    } else {
        $stageArgs += @('--flavor', "$Flavor")
    }
    if (-not (Test-Path -LiteralPath $script:PsxReleaseStageTool)) {
        throw ("Shared release staging tool is missing: $($script:PsxReleaseStageTool). " +
               "The pinned framework predates it (bead beads-eio.3.102).")
    }
    # stdout is the tag and nothing else, so it is read directly rather than
    # through Invoke-PsxReleaseStage (which echoes for the log).
    $tag = (& py -3 $script:PsxReleaseStageTool @stageArgs) | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0 -or -not $tag) {
        throw "Could not derive the overlay codegen tag (release_stage.py cg-tag failed)"
    }
    return $tag.Trim()
}

<#
.SYNOPSIS
Stage the prebuilt overlay shard cache for one codegen tag, or refuse to ship.

.DESCRIPTION
Only shards under $CgTag are copied: the runtime ignores every other namespace,
so shipping them would just inflate the download. Shipping with NO cache is a
real downgrade -- every player's first visit to every area runs interpreted --
so it throws unless -AllowNoCache makes that a deliberate, recorded choice.

-AllowNoCache is DEPRECATED and is retained only because unmigrated title
packagers still pass it. It maps to release_stage.py's
--ship-without-overlay-cache-because, which is loud, names what it does, and
prints the reason. Do not add it to a release recipe.
#>
function Add-OverlayCache {
    param(
        [Parameter(Mandatory)][string]$GameId,        # e.g. SCUS-94423
        [Parameter(Mandatory)][string]$CacheSrcRoot,  # <root>/<buildDir>/cache
        [Parameter(Mandatory)][string]$Stage,         # staging dir
        [Parameter(Mandatory)][string]$CgTag,
        [switch]$AllowNoCache
    )
    $countFile = New-PsxStageTempFile
    try {
        $stageArgs = @('stage-cache', '--game-id', $GameId,
                       '--cache-src-root', $CacheSrcRoot, '--stage', $Stage,
                       '--cg-tag', $CgTag, '--count-file', $countFile)
        if ($AllowNoCache) {
            $stageArgs += @('--ship-without-overlay-cache-because',
                            'the caller passed the deprecated -AllowNoCache switch')
        }
        Invoke-PsxReleaseStage -StageArgs $stageArgs | Out-Null
        return (Get-PsxStageCount $countFile)
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $countFile
    }
}

<#
.SYNOPSIS
Stage the self-contained overlay toolchain (pinned python + tcc + recompiler).

.DESCRIPTION
This is the fallback that lets a player with NO compiler installed still turn
captured overlays into native code. Without it the runtime's autocompile gate
(tk_present) is false and overlays stay interpreted forever, which is exactly
how Ape Escape shipped for its entire life -- and how EVERY Linux release of
every title has shipped, since no AppImage packager staged one at all.
#>
function Add-OverlayToolchain {
    param(
        [Parameter(Mandatory)][string]$Stage,
        [Parameter(Mandatory)][string]$RecompDir,     # dir holding psxrecomp-game.exe
        [Parameter(Mandatory)][string]$RecompTools,   # framework tools/
        [Parameter(Mandatory)][string]$RecompInc,     # framework runtime/include
        [Parameter(Mandatory)][string]$MingwBin,
        [Parameter(Mandatory)][string]$DlCache
    )
    Invoke-PsxReleaseStage -StageArgs @(
        'stage-toolchain', '--stage', $Stage, '--recomp-dir', $RecompDir,
        '--recomp-tools', $RecompTools, '--recomp-include', $RecompInc,
        '--dl-cache', $DlCache, '--platform', 'win',
        '--mingw-bin', $MingwBin) | Out-Null
    return (Join-Path $Stage "overlay_toolchain")
}

<#
.SYNOPSIS
Stage the mod catalog and verify it DERIVES from the real sources.

.DESCRIPTION
Every title used to assert a hard-coded package count -- Tomba 2 `-ne 5`,
MegaManX6 `-lt 16`, Ape Escape `-ne 4`. All three describe shared framework
content, so all three go stale the moment the framework gains or loses a mod:
measured 2026-09-01, Tomba 2 demanded 5 while the true catalog was 7 (the
framework had added psx.enhancement.pgxp and the game had added a fourth mod),
which made the title unreleasable for a reason that had nothing to do with it.

So assert the INVARIANT instead of a number: everything the sources define must
survive into the package. That cannot go stale when a mod is added, and it still
catches the failure that actually matters -- a mod silently not shipping.

Staging copies from the BUILD dir, never the source tree: the framework stages
its own builtin packages there at build time, and copying the source tree
silently drops them (pattern and rationale from MegaManX6).

release_stage.py additionally prefers the catalog manifest the BUILD published
(psx_mod_catalog_<target>.txt, written by runtime.cmake's
_psxrt_stage_mod_catalog) when -RuntimeTarget is given, which is a strictly
better authority than re-globbing the source trees.
#>
function Add-ModCatalog {
    param(
        [Parameter(Mandatory)][string]$BuildPath,          # build dir (has mods/bundled)
        [Parameter(Mandatory)][string]$Stage,
        [string]$GameModSource      = "",                  # <repo>/mods/preloaded
        [string]$FrameworkModSource = "",                  # <framework>/mods/builtin
        [string]$RuntimeTarget      = ""
    )
    $countFile = New-PsxStageTempFile
    try {
        $stageArgs = @('stage-mods', '--build-path', $BuildPath,
                       '--stage', $Stage, '--count-file', $countFile)
        if ($GameModSource)      { $stageArgs += @('--mod-source', $GameModSource) }
        if ($FrameworkModSource) { $stageArgs += @('--mod-source', $FrameworkModSource) }
        if ($RuntimeTarget)      { $stageArgs += @('--runtime-target', $RuntimeTarget) }
        Invoke-PsxReleaseStage -StageArgs $stageArgs | Out-Null
        return (Get-PsxStageCount $countFile)
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $countFile
    }
}
