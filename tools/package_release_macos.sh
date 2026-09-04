#!/usr/bin/env bash
# package_release_macos.sh — shared macOS release packager for psxrecomp games.
#
# macOS counterpart to tools/package_release.ps1. That script packages THIS
# framework's BIOS-only runtime and each game repo carries its own Windows
# packager; this one is generic, so every game repo can call it instead of
# duplicating ~200 lines of bundle/codesign/gate logic per title.
#
# It takes an already-built runtime plus the per-game identity and produces a
# signed, relocatable .app inside a staged release folder, then archives it as
# <package>-<version>-macos-<arch>.zip with a .sha256 sidecar (the naming the
# game repos' Windows releases already use).
#
# It does NOT build. The game's build script builds, then calls this.
#
# Usage (from a game repo):
#   bash psxrecomp/tools/package_release_macos.sh \
#       --repo . --build build-macos-prod \
#       --bin Crash_Bandicoot_Recompiled \
#       --app-name "Crash Bandicoot Recompiled" \
#       --package CrashBandicootRecomp \
#       --bundle-id com.mstan.crashbandicootrecomp \
#       --version v0.0.1 \
#       --game-config game.toml \
#       --require-mod crash-bandicoot.enhancement.adaptive-widescreen/1.0.0/manifest.toml
#
# Required:
#   --repo PATH        game repository root
#   --build PATH       build dir holding the built binary and CMake-staged data
#   --bin NAME         built binary name (the target's OUTPUT_NAME)
#
# Optional:
#   --app-name NAME    .app display name          (default: --bin, _ -> space)
#   --package NAME     archive name stem          (default: basename of --repo)
#   --bundle-id ID     CFBundleIdentifier         (default: com.mstan.<package>)
#   --version VER      release version, e.g. v0.0.1        (default: dev)
#   --out DIR          output directory           (default: <repo>/release-macos)
#   --game-config P    game.toml to bundle, relative to repo
#   --exclude-dev-mods drop channel = "developer" packages (also EXCLUDE_DEV_MODS=1)
#   --mods-src DIR     override catalog source   (default: the build tree's
#                      mods/, which carries framework builtins AND the title's
#                      own packages; per-machine state.toml is stripped)
#   --require-mod PKG  path under mods/bundled that must exist (repeatable)
#   --data FILE        file the runtime reads BESIDE the executable, relative to
#                      repo, e.g. keybinds.ini (repeatable)
#   --doc FILE         extra doc to ship, relative to repo (repeatable)
#   --dmg / --no-dmg   also build a .dmg          (default: no)
#   --no-zip           stage and sign only, skip the archive
#
# Release gates (any failure aborts, non-zero):
#   * the binary is self-contained (links only OS libraries / bundled frameworks)
#   * no absolute retail-BIOS path baked into the binary
#   * the 512 KiB OpenBIOS image and its MIT notice are present
#   * every --require-mod package is present
#   * no disc / memory-card / generated / local-state files in the stage
#   * saves/ ships empty
#   * the .app carries a valid signature
set -euo pipefail

die() { echo "package_release_macos: ERROR: $*" >&2; exit 1; }
note() { echo "      $*"; }

[ "$(uname -s)" = "Darwin" ] || die "run this on macOS."

REPO=""; BUILD=""; BIN_NAME=""; APP_NAME=""; PACKAGE=""; BUNDLE_ID=""
VERSION="dev"; OUT=""; GAME_CONFIG=""; MODS_SRC=""; MODS_SRC_EXPLICIT=""
DO_DMG=0; DO_ZIP=1
REQUIRE_MODS=(); DOCS=(); DATA_FILES=()

while [ $# -gt 0 ]; do
  case "$1" in
    --repo) REPO="$2"; shift 2;;
    --build) BUILD="$2"; shift 2;;
    --bin) BIN_NAME="$2"; shift 2;;
    --app-name) APP_NAME="$2"; shift 2;;
    --package) PACKAGE="$2"; shift 2;;
    --bundle-id) BUNDLE_ID="$2"; shift 2;;
    --version) VERSION="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --game-config) GAME_CONFIG="$2"; shift 2;;
    --exclude-dev-mods) EXCLUDE_DEV_MODS=1; shift;;
    --mods-src) MODS_SRC="$2"; MODS_SRC_EXPLICIT=1; shift 2;;
    --require-mod) REQUIRE_MODS+=("$2"); shift 2;;
    --data) DATA_FILES+=("$2"); shift 2;;
    --doc) DOCS+=("$2"); shift 2;;
    --dmg) DO_DMG=1; shift;;
    --no-dmg) DO_DMG=0; shift;;
    --no-zip) DO_ZIP=0; shift;;
    -h|--help) sed -n '2,57p' "$0"; exit 0;;
    *) die "unknown arg: $1";;
  esac
done

[ -n "$REPO" ] || die "--repo is required"
[ -n "$BUILD" ] || die "--build is required"
[ -n "$BIN_NAME" ] || die "--bin is required"
REPO="$(cd "$REPO" && pwd)"
[ -d "$BUILD" ] || BUILD="$REPO/$BUILD"
[ -d "$BUILD" ] || die "build dir not found: $BUILD"
BUILD="$(cd "$BUILD" && pwd)"

BIN="$BUILD/$BIN_NAME"
[ -f "$BIN" ] || die "built binary not found: $BIN (build it before packaging)"

[ -n "$APP_NAME" ] || APP_NAME="${BIN_NAME//_/ }"
[ -n "$PACKAGE" ] || PACKAGE="$(basename "$REPO")"
[ -n "$BUNDLE_ID" ] || BUNDLE_ID="com.mstan.$(echo "$PACKAGE" | tr '[:upper:]' '[:lower:]')"
[ -n "$OUT" ] || OUT="$REPO/release-macos"
[ -n "$MODS_SRC" ] || MODS_SRC="$REPO/mods/preloaded"

# Label the archive with what the binary actually contains, not what was asked
# for: a --arch universal build that silently produced a thin slice must not
# ship claiming to be universal.
ARCHS="$(lipo -archs "$BIN" 2>/dev/null || echo unknown)"
case "$ARCHS" in
  *arm64*x86_64*|*x86_64*arm64*) ARCH_LABEL="universal";;
  *arm64*) ARCH_LABEL="arm64";;
  *x86_64*) ARCH_LABEL="x86_64";;
  *) ARCH_LABEL="$ARCHS";;
esac

STAGE_ROOT="$OUT/stage"
STAGE="$STAGE_ROOT/$PACKAGE-macos-$ARCH_LABEL"
APPDIR="$STAGE/$APP_NAME.app"

echo "==== packaging $APP_NAME $VERSION (macOS $ARCH_LABEL) ===="
note "binary: $BIN ($ARCHS)"

# The stage is a fixed child of the output dir. Verify that before rm -rf.
case "$STAGE_ROOT" in
  "$OUT"/*) ;;
  *) die "refusing to clean a stage outside the output dir: $STAGE_ROOT";;
esac
rm -rf "$STAGE_ROOT"
mkdir -p "$APPDIR/Contents/MacOS" "$APPDIR/Contents/Resources" \
         "$APPDIR/Contents/Frameworks" "$STAGE/saves"

cp "$BIN" "$APPDIR/Contents/MacOS/$BIN_NAME"

# ---- staged data ------------------------------------------------------------
# The runtime resolves bios/, assets/ and mods/ beside the REAL EXECUTABLE, so
# they must be reachable from Contents/MacOS. They cannot literally live there:
# codesign treats every non-executable under Contents/MacOS as a nested-code
# candidate and rejects the whole bundle ("bundle format unrecognized, invalid,
# or unsuitable") — mods/bundled/<pkg>/<version>/ parses as a versioned bundle,
# assets/img/*.tga as stray nested code. So data lives in Contents/Resources and
# Contents/MacOS carries symlinks: codesign seals a symlink as a symlink instead
# of descending into it, and runtime path resolution is unchanged.
stage_data() {  # stage_data <name-beside-exe> <source>
    local name="$1" src="$2"
    [ -e "$src" ] || die "expected staged data missing: $src"
    rm -rf "$APPDIR/Contents/Resources/$name" "$APPDIR/Contents/MacOS/$name"
    cp -a "$src" "$APPDIR/Contents/Resources/$name"
    ln -s "../Resources/$name" "$APPDIR/Contents/MacOS/$name"
}

stage_data bios "$BUILD/bios"
stage_data assets "$BUILD/assets"

# Mod catalog. The hazard this guards against is real but narrow: a developer
# build dir accumulates mods/state.toml with enhancements switched on, and
# shipping that silently flips a title's default presentation for every player.
#
# Taking the SOURCE catalog instead used to be the remedy, but it drops every
# framework builtin (PGXP, fast loading, CD speed, bezel): those live in
# psxrecomp/mods/builtin and only ever appear NEXT TO THE EXE, staged there by
# runtime.cmake. A title with its own catalog therefore shipped one package
# where the player should have seen five.
#
# So take the build tree -- which is exactly what the runtime resolves at
# runtime -- and delete the one file that is actually unsafe. That matches
# package_setup_host.sh and package_release.ps1, so all three packagers now
# ship the same catalog. An explicit --mods-src still wins for anyone who
# wants the source-only set.
if [ -n "$MODS_SRC_EXPLICIT" ]; then
    [ -d "$MODS_SRC" ] || die "--mods-src is not a directory: $MODS_SRC"
    stage_data mods "$MODS_SRC"
elif [ -d "$BUILD/mods" ]; then
    stage_data mods "$BUILD/mods"
    find "$APPDIR/Contents/Resources/mods" \
        \( -name state.toml -o -name state.toml.tmp \) -delete
elif [ -d "$MODS_SRC" ]; then
    stage_data mods "$MODS_SRC"
else
    die "no mod catalog found: neither $BUILD/mods nor $MODS_SRC exists (rebuild the runtime target)"
fi
_mod_manifests=$(find "$APPDIR/Contents/Resources/mods" -name manifest.toml 2>/dev/null | wc -l)
[ "$_mod_manifests" -ge 1 ] \
    || die "staged mods/ contains no manifest.toml; the catalog would ship empty"
[ -z "$(find "$APPDIR/Contents/Resources/mods" -name 'state.toml*' 2>/dev/null)" ] \
    || die "per-machine mods/state.toml survived staging"
# Developer-channel work is unfinished: it ships with local builds and must
# never be published. Channels are per FEATURE, so this cannot be a grep -- a
# line-anchored match cannot tell a package-level key from one inside a
# [[feature]] block, and would drop a whole catalog over one instrument.
# mod_channel_filter.py parses instead, dropping the version directory when
# nothing in it ships and otherwise emitting a manifest without the developer
# features. The staged catalog is build output, so emitting it filtered is
# generation; the author's manifest in the repo is never touched.
if [ "${EXCLUDE_DEV_MODS:-0}" = "1" ]; then
    note "excluding developer-channel mods"
    command -v python3 >/dev/null 2>&1 \
        || die "no python3 on PATH; cannot filter developer-channel mods"
    python3 "$(dirname "$0")/mod_channel_filter.py" \
        "$APPDIR/Contents/Resources/mods/bundled" \
        "$APPDIR/Contents/Resources/mods/packages" \
        || die "developer-channel filtering failed"
    _dev_left=$( { grep -rlE '^[[:space:]]*channel[[:space:]]*=[[:space:]]*"developer"[[:space:]]*$' \
        "$APPDIR" --include=manifest.toml 2>/dev/null || true; } | wc -l)
    [ "$_dev_left" -eq 0 ] || die "developer manifest(s) survived pruning: $_dev_left"
    _mod_manifests=$(find "$APPDIR/Contents/Resources/mods" -name manifest.toml 2>/dev/null | wc -l)
fi
note "mod catalog: ${_mod_manifests} manifest(s), no per-machine state"

if [ -n "$GAME_CONFIG" ]; then
    stage_data "$(basename "$GAME_CONFIG")" "$REPO/$GAME_CONFIG"
fi

# Files the runtime reads beside the executable (keybinds.ini and friends) get
# the same Resources+symlink treatment as bios/ and assets/.
for f in "${DATA_FILES[@]}"; do
    stage_data "$(basename "$f")" "$REPO/$f"
done

for doc in "${DOCS[@]}"; do
    if [ -f "$REPO/$doc" ]; then
        cp "$REPO/$doc" "$APPDIR/Contents/Resources/$(basename "$doc")"
    fi
done
# Surface the player-facing docs beside the .app too, so they are readable
# without opening the bundle. Mirrors the flat Windows package layout.
for doc in START_HERE.txt README.md LICENSE RELEASE_NOTES.md; do
    if [ -f "$REPO/$doc" ]; then
        cp "$REPO/$doc" "$STAGE/$doc"
    fi
done

# ---- launcher shim ----------------------------------------------------------
# The real binary sits behind a shim so memory cards, settings and overlay
# caches land in the folder holding the .app (user-visible, writable, and the
# same place saves/ ships) rather than inside the bundle, which is signed and
# may be mounted read-only. --game takes an absolute in-bundle path so it
# resolves regardless of that working directory.
GAME_ARG=""
if [ -n "$GAME_CONFIG" ]; then
    GAME_ARG="--game \"\$DIR/$(basename "$GAME_CONFIG")\""
fi
cat > "$APPDIR/Contents/MacOS/$APP_NAME" <<EOF
#!/bin/sh
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
USERDIR="\$(cd "\$DIR/../../.." && pwd)"
export SDL_JOYSTICK_HIDAPI_STEAM=1
cd "\$USERDIR" 2>/dev/null || true
if [ "\$#" -eq 0 ]; then
    exec "\$DIR/$BIN_NAME" $GAME_ARG
fi
exec "\$DIR/$BIN_NAME" "\$@"
EOF
chmod +x "$APPDIR/Contents/MacOS/$APP_NAME"

SHORT_VERSION="${VERSION#v}"
cat > "$APPDIR/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleExecutable</key><string>$APP_NAME</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>$SHORT_VERSION</string>
  <key>CFBundleShortVersionString</key><string>$SHORT_VERSION</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF

# SDL3 is fetched and linked statically by runtime.cmake, so a default build has
# no non-system dylib to relocate. Builds pointed at a system SDL
# (-DPSX_SDL3_FETCH=OFF, -DPSX_SDL_BACKEND=SDL2) do; dylibbundler handles those
# and is a no-op otherwise. The self-containment gate below is what actually
# decides whether the result is shippable.
if command -v dylibbundler >/dev/null 2>&1; then
    dylibbundler -od -b -x "$APPDIR/Contents/MacOS/$BIN_NAME" \
        -d "$APPDIR/Contents/Frameworks" -p @executable_path/../Frameworks >/dev/null 2>&1 || true
fi

# ---- signing ----------------------------------------------------------------
# Ad-hoc, so it runs locally without a Developer ID. NOT --deep: it descends
# into the staged data and fails on the mod catalog's versioned directories.
# Sign the only Mach-O explicitly, then the bundle.
codesign --force --sign - "$APPDIR/Contents/MacOS/$BIN_NAME" 2>/dev/null
codesign --force --sign - "$APPDIR" 2>/dev/null
codesign --verify --deep "$APPDIR" 2>/dev/null \
    || die "ad-hoc codesign failed; the .app will not launch cleanly"
note "signed and verified (ad-hoc)"

# ---- release gates ----------------------------------------------------------
echo "---- gates ----"

# 1. Self-containment. Anything outside the OS or the bundle's own Frameworks
#    means the .app only runs on a machine that happens to have that library.
BAD_LINKS="$(otool -L "$APPDIR/Contents/MacOS/$BIN_NAME" | tail -n +2 | awk '{print $1}' \
    | grep -vE '^(/usr/lib/|/System/Library/|@executable_path/../Frameworks/|@rpath/)' || true)"
[ -z "$BAD_LINKS" ] || die "binary is not self-contained; links: $(echo "$BAD_LINKS" | tr '\n' ' ')"
note "self-contained (links only OS libraries)"

# 2. No baked absolute retail-BIOS path. One baked into the binary makes it
#    silently load the BUILDER'S BIOS wherever that path exists, so the clean
#    OpenBIOS-by-default path never gets exercised where releases are validated.
#
#    Match only genuinely rooted paths. The Windows gate keys on a drive letter
#    ("C:\..."); the macOS tell is a path rooted in a real user or volume
#    location. Two things must NOT trip this: the correct relative default
#    "bios/SCPH1001.BIN", and the usage-message placeholder
#    "/path/to/SCPH1001.BIN" — both are expected in every release binary.
ROOTED='/(Users|home|private|var|tmp|opt|Volumes)/'
BAKED="$(strings -a "$APPDIR/Contents/MacOS/$BIN_NAME" \
    | grep -oE "${ROOTED}[^ \"']*SCPH[0-9]*\.BIN" | sort -u || true)"
[ -z "$BAKED" ] || die "binary contains baked absolute BIOS path(s): $(echo "$BAKED" | tr '\n' ' ')"

#    Same failure class, wider net: any path into the build machine's home
#    directory that survived into the binary is a leak (and often a privacy
#    problem in a public release).
HOME_LEAK="$(strings -a "$APPDIR/Contents/MacOS/$BIN_NAME" \
    | grep -oE "${HOME}/[^ \"']*" | sort -u | head -5 || true)"
[ -z "$HOME_LEAK" ] || die "binary contains baked build-machine path(s): $(echo "$HOME_LEAK" | tr '\n' ' ')"
note "no baked absolute BIOS or build-machine path"

# 3. Bundled OpenBIOS: redistributable and required; retail images are not.
OPENBIOS="$APPDIR/Contents/Resources/bios/openbios.bin"
[ -f "$OPENBIOS" ] || die "bundled OpenBIOS image missing"
[ "$(stat -f%z "$OPENBIOS")" = "524288" ] || die "OpenBIOS image is not 512 KiB"
[ -f "$APPDIR/Contents/Resources/bios/OpenBIOS.LICENSE" ] || die "OpenBIOS MIT notice missing"
note "bundled OpenBIOS (512 KiB) + MIT notice"

# 4. Required mod packages. A silently mod-less launcher looks fine but has no
#    Mods page at all, which is indistinguishable from the feature being cut.
for m in "${REQUIRE_MODS[@]}"; do
    [ -f "$APPDIR/Contents/Resources/mods/bundled/$m" ] \
        || die "required mod package missing: mods/bundled/$m"
done
[ ${#REQUIRE_MODS[@]} -eq 0 ] || note "mod catalog: ${#REQUIRE_MODS[@]} required package(s) present"

# 5. Nothing from the developer's machine or the copyrighted source material.
STRAY="$(find "$STAGE" \( \
        -iname '*.cue' -o -iname '*.iso' -o -iname '*.chd' -o -iname '*.mcd' \
     -o -iname '*.mcr' -o -iname 'SCPH*.BIN' -o -iname 'settings.toml' \
     -o -iname 'state.toml' -o -iname 'overlay_captures.json' \
     -o -iname 'bios.cfg' -o -iname 'disc.cfg' \) -print 2>/dev/null || true)"
[ -z "$STRAY" ] || die "files that must never ship entered the stage:
$STRAY"
# Every .bin except the bundled OpenBIOS is suspect (disc images, BIOS dumps).
STRAY_BIN="$(find "$STAGE" -iname '*.bin' ! -path "$OPENBIOS" -print 2>/dev/null || true)"
[ -z "$STRAY_BIN" ] || die "unexpected .bin in stage (only openbios.bin may ship):
$STRAY_BIN"
STRAY_GEN="$(find "$STAGE" -path '*/generated/*' -print 2>/dev/null | head -1 || true)"
[ -z "$STRAY_GEN" ] || die "generated recompiler output entered the stage: $STRAY_GEN"
note "no disc / retail BIOS / generated / local-state files"

# 6. saves/ ships empty so a player's first run starts clean.
SAVED="$(find "$STAGE/saves" -type f -print 2>/dev/null || true)"
[ -z "$SAVED" ] || die "saves/ must ship empty, contains:
$SAVED"
note "saves/ empty"

echo "---- gates passed ----"
note "BUILT: $APPDIR"

# ---- archive ----------------------------------------------------------------
if [ "$DO_ZIP" = "1" ]; then
    ZIP="$OUT/$PACKAGE-$VERSION-macos-$ARCH_LABEL.zip"
    rm -f "$ZIP" "$ZIP.sha256"
    # ditto, not zip: it preserves the symlinks and the code signature so the
    # unpacked .app still verifies. A plain `zip` can flatten both.
    ( cd "$STAGE_ROOT" && ditto -c -k --sequesterRsrc --keepParent \
        "$(basename "$STAGE")" "$ZIP" )
    ( cd "$OUT" && shasum -a 256 "$(basename "$ZIP")" > "$(basename "$ZIP").sha256" )
    note "BUILT: $ZIP"
    note "SHA-256: $(awk '{print $1}' "$ZIP.sha256")"
fi

if [ "$DO_DMG" = "1" ]; then
    DMG="$OUT/$PACKAGE-$VERSION-macos-$ARCH_LABEL.dmg"
    rm -f "$DMG"
    if command -v create-dmg >/dev/null 2>&1; then
        create-dmg --volname "$APP_NAME" --app-drop-link 420 180 "$DMG" "$APPDIR" \
          || hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
    else
        hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
    fi
    note "BUILT: $DMG"
fi
