#!/usr/bin/env sh
# New Project Layout scaffolding (Linux / macOS).
#
# Path flags (pass on the CLI — use your shell's path completion):
#   --disc <cue>          REQUIRED legal Redump .cue (kept on your dump drive)
#   --dir <parent>        Parent directory for the new repo (default: .)
#   --bios <SCPH1001.BIN> Optional retail BIOS for --generate / generate prompt
#   --boot-exe <name>     Optional override until disc probe runs
#   --stage-disc          Copy full cue+bins into repo disc/ (large; optional)
#   --no-stage-disc       Default: probe in place, extract boot EXE only
#   --psxrecomp-ref / --recomp-ui-ref / --recomp-net-ref / URLs
#   --github-owner <org>  README download-badge owner (default TechnicallyComputers)
#   --github-repo <name>  README download-badge repo (default project name)
#
# Everything else is prompted on a TTY (or passed via flags / --yes defaults):
#   name, players (1–8, default 2), zip-prefix, github owner/repo (README badges),
#   marketing blurb, recomp-ui, wizard, netplay, lobby URL (if netplay), CI
#   workflow, fetch-boxart, generate, build (if generate), GitHub repo (gh)
#
# Non-interactive: --yes (or PSXRECOMP_SETUP_YES=1) requires --name and --disc;
#   boolean opts stay off unless explicitly flagged; lobby URL defaults to
#   netplay.retcomm.net when netplay is enabled.
#
# Disc layout (default): game.toml disc= absolute path to your Redump cue;
#   only the small boot EXE is written under disc/. Use --stage-disc to copy
#   the full multi-track dump into the repo (gitignored) instead.
#
# GitHub publish order (avoids competing "initial" commits / missing CI):
#   scaffold + CI workflow → commit → gh repo create (no push) → generate/build
#   → single git push -u origin HEAD → verify Actions workflows if CI enabled.
#
# Usage:
#   sh tools/new_project_layout/setup_project.sh --disc /path/to/game.cue
#   sh tools/new_project_layout/setup_project.sh --disc game.cue --name Foo --yes
#   sh tools/new_project_layout/setup_project.sh --disc game.cue --stage-disc

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEMPLATE_DIR="$SCRIPT_DIR/templates"
PROBE_DISC="$SCRIPT_DIR/probe_disc.py"
FILL_TOKENS="$SCRIPT_DIR/fill_tokens.py"
FETCH_BOXART="$SCRIPT_DIR/fetch_boxart.py"

NAME=""
DISC=""
PARENT="."
BOOT_EXE="SLUS_01234"
PLAYERS=""
ZIP_PREFIX=""
ENABLE_RECOMP_UI=""
ENABLE_NETPLAY=""
ENABLE_WIZARD=""
ENABLE_CI=""
FETCH_BOXART_FLAG=""
DO_GENERATE=""
DO_BUILD=""
CREATE_GITHUB=""
GITHUB_VISIBILITY="private"
GITHUB_OWNER=""
GITHUB_REPO=""
BIOS_PATH=""
LOBBY_URL=""
DESCRIPTION=""
PUBLISHER=""
YEAR=""
REGION=""
YES_MODE=0
DEFAULT_LOBBY_HOST="netplay.retcomm.net"
# mstan/psxrecomp and recomp-ui both use master (not main).
# Nested recomp-net defaults to whatever SHA psxrecomp pins; override with
# --recomp-net-ref (branch/tag/SHA) or RECOMP_NET_REF=… so netplay can track
# main (or feat/rollback) instead of a stale nested pin.
PSXRECOMP_REF="master"
RECOMP_UI_REF="master"
RECOMP_NET_REF="${RECOMP_NET_REF:-}"
PSXRECOMP_URL="${PSXRECOMP_URL:-https://github.com/mstan/psxrecomp.git}"
RECOMP_UI_URL="${RECOMP_UI_URL:-https://github.com/mstan/recomp-ui.git}"

# Track whether bools were set on the CLI (so prompts can skip).
SET_RECOMP_UI=0
SET_NETPLAY=0
SET_WIZARD=0
SET_CI=0
SET_BOXART=0
SET_GENERATE=0
SET_BUILD=0
SET_GITHUB=0
SET_LOBBY_URL=0
SET_DESCRIPTION=0
SET_PUBLISHER=0
SET_YEAR=0
SET_REGION=0
STAGE_DISC=0

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

is_tty() {
    [ -t 0 ] && [ -t 1 ]
}

prompt_line() {
    # prompt_line "Question" default_var_name [default_value]
    _q=$1
    _var=$2
    _def=${3:-}
    if [ -n "$_def" ]; then
        printf '%s [%s]: ' "$_q" "$_def" >/dev/tty
    else
        printf '%s: ' "$_q" >/dev/tty
    fi
    # shellcheck disable=SC2162
    read _ans </dev/tty || _ans=
    if [ -z "$_ans" ]; then
        _ans=$_def
    fi
    eval "$_var=\$_ans"
}

prompt_yn() {
    # prompt_yn "Question" var_name default_y_or_n
    _q=$1
    _var=$2
    _def=$3
    _hint=y/N
    [ "$_def" = "1" ] || [ "$_def" = "y" ] || [ "$_def" = "Y" ] && _hint=Y/n
    while :; do
        printf '%s [%s]: ' "$_q" "$_hint" >/dev/tty
        # shellcheck disable=SC2162
        read _ans </dev/tty || _ans=
        if [ -z "$_ans" ]; then
            if [ "$_def" = "1" ] || [ "$_def" = "y" ] || [ "$_def" = "Y" ]; then
                eval "$_var=1"
            else
                eval "$_var=0"
            fi
            return 0
        fi
        case "$_ans" in
            y|Y|yes|YES) eval "$_var=1"; return 0 ;;
            n|N|no|NO) eval "$_var=0"; return 0 ;;
            *) printf '  please answer y or n\n' >/dev/tty ;;
        esac
    done
}

# Bare host → ws://host:8765; host:port → ws://host:port; ws(s):// kept as-is.
normalize_lobby_url() {
    _in=$1
    if [ -z "$_in" ]; then
        _in="$DEFAULT_LOBBY_HOST"
    fi
    case "$_in" in
        ws://*|wss://*) printf '%s' "$_in" ;;
        *://*) printf '%s' "$_in" ;;
        *:*) printf 'ws://%s' "$_in" ;;
        *) printf 'ws://%s:8765' "$_in" ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME=$2; shift 2 ;;
        --disc) DISC=$2; shift 2 ;;
        --dir) PARENT=$2; shift 2 ;;
        --boot-exe) BOOT_EXE=$2; shift 2 ;;
        --players) PLAYERS=$2; shift 2 ;;
        --zip-prefix) ZIP_PREFIX=$2; shift 2 ;;
        --enable-recomp-ui) ENABLE_RECOMP_UI=1; SET_RECOMP_UI=1; shift ;;
        --no-recomp-ui) ENABLE_RECOMP_UI=0; SET_RECOMP_UI=1; shift ;;
        --enable-netplay) ENABLE_NETPLAY=1; SET_NETPLAY=1; shift ;;
        --no-netplay) ENABLE_NETPLAY=0; SET_NETPLAY=1; shift ;;
        --lobby-url) LOBBY_URL=$2; SET_LOBBY_URL=1; shift 2 ;;
        --enable-wizard) ENABLE_WIZARD=1; SET_WIZARD=1; shift ;;
        --no-wizard) ENABLE_WIZARD=0; SET_WIZARD=1; shift ;;
        --enable-ci) ENABLE_CI=1; SET_CI=1; shift ;;
        --no-ci) ENABLE_CI=0; SET_CI=1; shift ;;
        --fetch-boxart) FETCH_BOXART_FLAG=1; SET_BOXART=1; shift ;;
        --no-fetch-boxart) FETCH_BOXART_FLAG=0; SET_BOXART=1; shift ;;
        --generate) DO_GENERATE=1; SET_GENERATE=1; shift ;;
        --no-generate) DO_GENERATE=0; SET_GENERATE=1; shift ;;
        --enable-build) DO_BUILD=1; SET_BUILD=1; shift ;;
        --no-build) DO_BUILD=0; SET_BUILD=1; shift ;;
        --create-github) CREATE_GITHUB=1; SET_GITHUB=1; shift ;;
        --no-github) CREATE_GITHUB=0; SET_GITHUB=1; shift ;;
        --github-visibility) GITHUB_VISIBILITY=$2; shift 2 ;;
        --github-owner) GITHUB_OWNER=$2; shift 2 ;;
        --github-repo) GITHUB_REPO=$2; shift 2 ;;
        --description) DESCRIPTION=$2; SET_DESCRIPTION=1; shift 2 ;;
        --publisher) PUBLISHER=$2; SET_PUBLISHER=1; shift 2 ;;
        --year) YEAR=$2; SET_YEAR=1; shift 2 ;;
        --region) REGION=$2; SET_REGION=1; shift 2 ;;
        --bios) BIOS_PATH=$2; shift 2 ;;
        --yes|-y) YES_MODE=1; shift ;;
        --stage-disc) STAGE_DISC=1; shift ;;
        --no-stage-disc) STAGE_DISC=0; shift ;;
        --psxrecomp-ref) PSXRECOMP_REF=$2; shift 2 ;;
        --recomp-ui-ref) RECOMP_UI_REF=$2; shift 2 ;;
        --recomp-net-ref) RECOMP_NET_REF=$2; shift 2 ;;
        -h|--help) usage 0 ;;
        *) echo "Unknown arg: $1" >&2; usage 1 ;;
    esac
done

if [ "${PSXRECOMP_SETUP_YES:-0}" = "1" ]; then
    YES_MODE=1
fi

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: missing required command: $1" >&2
        exit 1
    fi
}
need_cmd git
need_cmd python3

# --- Required path: disc ---------------------------------------------------
if [ -z "$DISC" ]; then
    echo "error: --disc <path-to-game.cue> is required (use your shell to tab-complete the path)." >&2
    echo "       Example: sh $0 --disc ~/dumps/MyGame\\ \\(USA\\).cue" >&2
    exit 1
fi
if [ ! -f "$DISC" ]; then
    echo "error: --disc not found: $DISC" >&2
    exit 1
fi

# --- Interactive / flag config ---------------------------------------------
if [ -z "$NAME" ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        echo "error: --name is required in non-interactive mode (--yes / non-TTY)" >&2
        exit 1
    fi
    prompt_line "Project name (e.g. MyGameRecomp)" NAME ""
    if [ -z "$NAME" ]; then
        echo "error: project name is required" >&2
        exit 1
    fi
fi

if [ -z "$PLAYERS" ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        PLAYERS=2
    else
        prompt_line "Max players (1-8)" PLAYERS "2"
    fi
fi
case "$PLAYERS" in
    ''|*[!0-9]*)
        echo "error: players must be an integer 1–8 (got: $PLAYERS)" >&2
        exit 1
        ;;
esac
if [ "$PLAYERS" -lt 1 ] || [ "$PLAYERS" -gt 8 ]; then
    echo "error: players must be 1–8 (got: $PLAYERS)" >&2
    exit 1
fi

if [ -z "$ZIP_PREFIX" ]; then
    _derived=$(PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -c 'from fill_tokens import derive_zip_prefix; import sys; print(derive_zip_prefix(sys.argv[1]))' "$NAME")
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        ZIP_PREFIX=$_derived
    else
        prompt_line "Zip / CI artifact prefix" ZIP_PREFIX "$_derived"
    fi
fi

if [ -z "$GITHUB_OWNER" ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        GITHUB_OWNER=TechnicallyComputers
    else
        prompt_line "GitHub owner / org (README download badges)" GITHUB_OWNER \
            "TechnicallyComputers"
    fi
fi
if [ -z "$GITHUB_REPO" ]; then
    _derived_repo=$(PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -c 'from fill_tokens import sanitize_github_name; import sys; print(sanitize_github_name(sys.argv[1]))' "$NAME")
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        GITHUB_REPO=$_derived_repo
    else
        prompt_line "GitHub repo name (README download badges)" GITHUB_REPO "$_derived_repo"
    fi
fi
GITHUB_OWNER=$(PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
    python3 -c 'from fill_tokens import sanitize_github_name; import sys; print(sanitize_github_name(sys.argv[1]))' "$GITHUB_OWNER")
GITHUB_REPO=$(PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
    python3 -c 'from fill_tokens import sanitize_github_name; import sys; print(sanitize_github_name(sys.argv[1]))' "$GITHUB_REPO")

# Optional marketing (catalog_identity.json + README)
if [ "$SET_DESCRIPTION" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        DESCRIPTION=""
    else
        prompt_line "Short game description (optional)" DESCRIPTION ""
    fi
fi
if [ "$SET_PUBLISHER" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        PUBLISHER=""
    else
        prompt_line "Publisher (optional)" PUBLISHER ""
    fi
fi
if [ "$SET_YEAR" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        YEAR=""
    else
        prompt_line "Release year (optional)" YEAR ""
    fi
fi
if [ "$SET_REGION" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        REGION="USA"
    else
        prompt_line "Region" REGION "USA"
    fi
fi

if [ "$SET_RECOMP_UI" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        ENABLE_RECOMP_UI=0
    else
        prompt_yn "Include recomp-ui launcher submodule?" ENABLE_RECOMP_UI 1
    fi
fi

if [ "$ENABLE_RECOMP_UI" -eq 0 ]; then
    if [ "${ENABLE_WIZARD:-0}" = "1" ] || [ "${ENABLE_NETPLAY:-0}" = "1" ]; then
        echo "warning: wizard/netplay require recomp-ui — forcing both off." >&2
    fi
    ENABLE_WIZARD=0
    ENABLE_NETPLAY=0
    SET_WIZARD=1
    SET_NETPLAY=1
    echo "  (recomp-ui declined — skipping wizard/netplay; PSX_RECOMP_UI=OFF)"
else
    if [ "$SET_WIZARD" -eq 0 ]; then
        # Default ON: setup-host CI zips need the wizard (FORCE_SETUP_HOST
        # without PSX_SETUP_WIZARD opens no first-run UI).
        if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
            ENABLE_WIZARD=1
        else
            prompt_yn "Enable first-run setup wizard + Generate & rebuild?" ENABLE_WIZARD 1
        fi
    fi
    # 1-player titles cannot use multiplayer netplay — skip prompts entirely.
    if [ "$PLAYERS" -eq 1 ]; then
        if [ "${ENABLE_NETPLAY:-0}" = "1" ]; then
            echo "warning: --enable-netplay ignored for 1-player title." >&2
        fi
        ENABLE_NETPLAY=0
        SET_NETPLAY=1
        echo "  (1-player title — netplay skipped)"
    elif [ "$SET_NETPLAY" -eq 0 ]; then
        if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
            ENABLE_NETPLAY=0
        else
            prompt_yn "Enable netplay UI (needs nested recomp-net)?" ENABLE_NETPLAY 0
        fi
    fi
fi

NETPLAY_LOBBY_WS=""
if [ "${ENABLE_NETPLAY:-0}" -eq 1 ]; then
    if [ "$SET_LOBBY_URL" -eq 0 ]; then
        if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
            LOBBY_URL="$DEFAULT_LOBBY_HOST"
        else
            prompt_line "Netplay lobby server URL (host or ws://…)" LOBBY_URL \
                "$DEFAULT_LOBBY_HOST"
        fi
    fi
    NETPLAY_LOBBY_WS=$(normalize_lobby_url "$LOBBY_URL")
fi

if [ "$SET_CI" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        ENABLE_CI=0
    else
        prompt_yn "Add GitHub Actions release workflow (Linux/Windows/macOS)?" ENABLE_CI 1
    fi
fi

# Setup-host release CI needs PSX_SETUP_WIZARD (FORCE_SETUP_HOST alone is not enough).
if [ "${ENABLE_CI:-0}" -eq 1 ] && [ "${ENABLE_RECOMP_UI:-0}" -eq 1 ] &&
   [ "${ENABLE_WIZARD:-0}" -eq 0 ]; then
    echo "error: release CI requires the first-run setup wizard." >&2
    echo "  Pass --enable-wizard (or omit --no-wizard)." >&2
    exit 1
fi

if [ "$SET_BOXART" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        FETCH_BOXART_FLAG=0
    else
        prompt_yn "Fetch libretro boxart now? (needs network)" FETCH_BOXART_FLAG 1
    fi
fi

if [ "$SET_GENERATE" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        DO_GENERATE=0
    else
        prompt_yn "Run Generate now (emitters + OpenBIOS + game C)?" DO_GENERATE 0
    fi
fi

if [ "$SET_BUILD" -eq 0 ]; then
    if [ "${DO_GENERATE:-0}" -eq 1 ]; then
        if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
            DO_BUILD=0
        else
            prompt_yn "Configure & build psx-runtime after Generate?" DO_BUILD 1
        fi
    else
        DO_BUILD=0
        SET_BUILD=1
    fi
elif [ "${DO_BUILD:-0}" -eq 1 ] && [ "${DO_GENERATE:-0}" -eq 0 ]; then
    echo "warning: --enable-build requires Generate — enabling Generate" >&2
    DO_GENERATE=1
    SET_GENERATE=1
fi

if [ "$SET_GITHUB" -eq 0 ]; then
    if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
        CREATE_GITHUB=0
    else
        prompt_yn "Create GitHub repo with gh (needs auth)?" CREATE_GITHUB 0
    fi
fi
if [ "${CREATE_GITHUB:-0}" -eq 1 ]; then
    case "$GITHUB_VISIBILITY" in
        public|private|internal) ;;
        *)
            if [ "$YES_MODE" -eq 1 ] || ! is_tty; then
                GITHUB_VISIBILITY=private
            else
                prompt_line "GitHub visibility (public/private)" GITHUB_VISIBILITY "private"
            fi
            ;;
    esac
fi

if [ -n "$BIOS_PATH" ] && [ ! -f "$BIOS_PATH" ]; then
    echo "error: --bios not found: $BIOS_PATH" >&2
    exit 1
fi
if [ -n "$BIOS_PATH" ] && [ "$DO_GENERATE" -eq 0 ]; then
    echo "warning: --bios ignored without generate" >&2
fi

PROJECT_CMAKE_NAME=$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9_' '_')
ENV_PREFIX=$(printf '%s' "$PROJECT_CMAKE_NAME" | tr 'a-z' 'A-Z')
WINDOW_TITLE=$(printf '%s' "$NAME" | sed 's/Recomp$/ Recompiled/;s/Recompiled Recompiled/Recompiled/')
# Must match runtime.cmake OUTPUT_NAME: MAKE_C_IDENTIFIER(WINDOW_TITLE).
# Using $NAME alone (TwistedMetal4Recomp) breaks CI packaging — binary is
# TwistedMetal4_Recompiled.
EXE_BASENAME=$(printf '%s' "$WINDOW_TITLE" | tr -c 'A-Za-z0-9_' '_')
GAME_NAME=$(printf '%s' "$NAME" | sed 's/Recomp$//;s/Recompiled$//' | sed 's/[[:space:]]*$//')
[ -n "$GAME_NAME" ] || GAME_NAME="$NAME"
DISC_HINT="your legally owned ${GAME_NAME} disc"
ENTRY_PC="0x80010000"
[ -n "$REGION" ] || REGION="USA"
# Catalog keeps blanks; README table uses em-dash placeholders.
PUBLISHER_DISP=${PUBLISHER:-—}
YEAR_DISP=${YEAR:-—}
DESCRIPTION_MD="$DESCRIPTION"
if [ -z "$DESCRIPTION_MD" ]; then
    DESCRIPTION_MD="_Add a short pitch in catalog_identity.json / README._"
fi

# Folder = GitHub/catalog install_dir slug (hyphenated), not display $NAME with spaces.
INSTALL_DIR_NAME="$GITHUB_REPO"
ROOT=$(CDPATH= cd -- "$PARENT" && pwd)/"$INSTALL_DIR_NAME"
if [ -e "$ROOT" ]; then
    echo "error: target already exists: $ROOT" >&2
    exit 1
fi
if [ "$INSTALL_DIR_NAME" != "$NAME" ]; then
    echo "note: project folder is '$INSTALL_DIR_NAME' (catalog/GitHub slug; display name stays '$NAME')" >&2
fi

# --- CMake block files -----------------------------------------------------
NETPLAY_BLOCK_FILE=$(mktemp)
WIZARD_BLOCK_FILE=$(mktemp)
BOXART_BLOCK_FILE=$(mktemp)
APP_ICON_BLOCK_FILE=$(mktemp)
RECOMP_UI_BLOCK_FILE=$(mktemp)
CODEGEN_ARG_FILE=$(mktemp)
trap 'rm -f "$NETPLAY_BLOCK_FILE" "$WIZARD_BLOCK_FILE" "$BOXART_BLOCK_FILE" "$APP_ICON_BLOCK_FILE" "$RECOMP_UI_BLOCK_FILE" "$CODEGEN_ARG_FILE"' EXIT

if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    printf '%s\n' '# recomp-ui submodule present (PSX_RECOMP_UI defaults ON).' \
        >"$RECOMP_UI_BLOCK_FILE"
    printf '%s\n' '    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"' \
        >"$CODEGEN_ARG_FILE"
else
    printf '%s\n' \
'set(PSX_RECOMP_UI OFF CACHE BOOL
    "No recomp-ui submodule in this scaffold" FORCE)' \
        >"$RECOMP_UI_BLOCK_FILE"
    printf '%s\n' '    # CODEGEN_SETUP_SOURCES — add with recomp-ui / wizard later' \
        >"$CODEGEN_ARG_FILE"
fi

if [ "$ENABLE_NETPLAY" -eq 1 ]; then
    # PSX_NETPLAY defaults RNET_ENABLE_ICE=ON; recomp-net FetchContents
    # libjuice via pinned URL (not git) so RetComM AppImage builds configure.
    printf '%s\n' \
'if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
    set(PSX_NETPLAY ON CACHE BOOL
        "Link recomp-net delay-sync (opt-in; needs recomp-net)" FORCE)
endif()' >"$NETPLAY_BLOCK_FILE"
    NETPLAY_RUNTIME_ARG="    ENABLE_NETPLAY_IF_PRESENT"
    NETPLAY_LOBBY_URL_ARG="    NETPLAY_LOBBY_URL \"$NETPLAY_LOBBY_WS\""
else
    printf '%s\n' \
'# Full netplay UI — enable after adding recomp-ui + testing:
# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)
# endif()' >"$NETPLAY_BLOCK_FILE"
    NETPLAY_RUNTIME_ARG="    # ENABLE_NETPLAY_IF_PRESENT"
    NETPLAY_LOBBY_URL_ARG="    # NETPLAY_LOBBY_URL \"ws://${DEFAULT_LOBBY_HOST}:8765\""
fi

if [ "$ENABLE_WIZARD" -eq 1 ]; then
    printf '%s\n' \
'set(PSX_SETUP_WIZARD ON CACHE BOOL
    "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" FORCE)' \
        >"$WIZARD_BLOCK_FILE"
    WIZARD_RUNTIME_ARG="    ENABLE_SETUP_WIZARD"
else
    printf '%s\n' \
'# First-run setup wizard — enable after testing:
# set(PSX_SETUP_WIZARD ON CACHE BOOL "…" FORCE)' >"$WIZARD_BLOCK_FILE"
    WIZARD_RUNTIME_ARG="    # ENABLE_SETUP_WIZARD"
fi

printf '%s\n' '    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"' \
    >"$BOXART_BLOCK_FILE"
HAS_BOXART=0
printf '%s\n' '    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"' \
    >"$APP_ICON_BLOCK_FILE"

fill_template() {
    src=$1
    dst=$2
    python3 "$FILL_TOKENS" "$src" "$dst" \
        --set "PROJECT_CMAKE_NAME=$PROJECT_CMAKE_NAME" \
        --set "WINDOW_TITLE=$WINDOW_TITLE" \
        --set "BOOT_EXE=$BOOT_EXE" \
        --set "GAME_NAME=$GAME_NAME" \
        --set "ENV_PREFIX=$ENV_PREFIX" \
        --set "EXE_BASENAME=$EXE_BASENAME" \
        --set "ZIP_PREFIX=$ZIP_PREFIX" \
        --set "GITHUB_OWNER=$GITHUB_OWNER" \
        --set "GITHUB_REPO=$GITHUB_REPO" \
        --set "DISC_HINT=$DISC_HINT" \
        --set "GAME_TITLE=$WINDOW_TITLE" \
        --set "PLAYERS=$PLAYERS" \
        --set "DESCRIPTION=$DESCRIPTION_MD" \
        --set "PUBLISHER=$PUBLISHER_DISP" \
        --set "YEAR=$YEAR_DISP" \
        --set "REGION=$REGION" \
        --set "ENTRY_PC=$ENTRY_PC" \
        --set "NETPLAY_RUNTIME_ARG=$NETPLAY_RUNTIME_ARG" \
        --set "NETPLAY_LOBBY_URL_ARG=$NETPLAY_LOBBY_URL_ARG" \
        --set "WIZARD_RUNTIME_ARG=$WIZARD_RUNTIME_ARG" \
        --set-file "BOXART_CMAKE_ARG=$BOXART_BLOCK_FILE" \
        --set-file "APP_ICON_CMAKE_ARG=$APP_ICON_BLOCK_FILE" \
        --set-file "NETPLAY_CMAKE_BLOCK=$NETPLAY_BLOCK_FILE" \
        --set-file "WIZARD_CMAKE_BLOCK=$WIZARD_BLOCK_FILE" \
        --set-file "RECOMP_UI_CMAKE_BLOCK=$RECOMP_UI_BLOCK_FILE" \
        --set-file "CODEGEN_SETUP_ARG=$CODEGEN_ARG_FILE"
}

echo "== New Project Layout =="
echo "  repo:       $ROOT"
echo "  disc:       $DISC"
echo "  zip prefix: $ZIP_PREFIX"
echo "  gh slug:    $GITHUB_OWNER/$GITHUB_REPO"
echo "  players:    $PLAYERS"
echo "  recomp-ui:  $ENABLE_RECOMP_UI"
echo "  wizard:     $ENABLE_WIZARD"
echo "  netplay:    $ENABLE_NETPLAY"
if [ "$ENABLE_NETPLAY" -eq 1 ]; then
    echo "  lobby URL:  $NETPLAY_LOBBY_WS"
fi
echo "  CI:         $ENABLE_CI"
echo "  boxart:     $FETCH_BOXART_FLAG"
echo "  generate:   $DO_GENERATE"
echo "  build:      $DO_BUILD"
echo "  github:     $CREATE_GITHUB"
echo "  region:     $REGION"

mkdir -p "$ROOT"
cd "$ROOT"
git init -q

fill_template "$TEMPLATE_DIR/CMakeLists.txt.in" "$ROOT/CMakeLists.txt"
fill_template "$TEMPLATE_DIR/game.toml.in" "$ROOT/game.toml"
fill_template "$TEMPLATE_DIR/game_options.toml.in" "$ROOT/game_options.toml"
fill_template "$TEMPLATE_DIR/codegen_setup.c.in" "$ROOT/codegen_setup.c"
fill_template "$TEMPLATE_DIR/codegen_setup.h.in" "$ROOT/codegen_setup.h"
fill_template "$TEMPLATE_DIR/gitignore.in" "$ROOT/.gitignore"
fill_template "$TEMPLATE_DIR/VERSION.in" "$ROOT/VERSION"
fill_template "$TEMPLATE_DIR/README.md.in" "$ROOT/README.md"
fill_template "$TEMPLATE_DIR/symbols.toml.in" "$ROOT/symbols.toml"
mkdir -p "$ROOT/seeds" "$ROOT/launcher_assets/img" "$ROOT/scripts" "$ROOT/tools" \
    "$ROOT/mods/preloaded/packages" "$ROOT/assets" "$ROOT/.github"
cp "$SCRIPT_DIR/sync_symbols.py" "$ROOT/tools/sync_symbols.py"
chmod +x "$ROOT/tools/sync_symbols.py"
if [ -f "$TEMPLATE_DIR/raid-discord.png" ]; then
    cp "$TEMPLATE_DIR/raid-discord.png" "$ROOT/.github/raid-discord.png"
fi
# Empty mod catalog tree (runtime copies mods/preloaded → beside the exe as mods/).
cat > "$ROOT/mods/preloaded/README.md" <<'EOF'
# Preloaded mods

Ship reviewed, default-disabled packages here:

```text
packages/<package-id>/<version>/
  manifest.toml
  …
```

Build wiring copies `mods/preloaded` next to the game executable as `mods/`.
Install player `.psxmod` archives through the launcher Mods manager instead of
committing them here. See `psxrecomp/docs/MOD_PACKAGES.md`.
EOF
: > "$ROOT/mods/preloaded/packages/.gitkeep"

echo "== Adding submodules =="
git submodule add -b "$PSXRECOMP_REF" "$PSXRECOMP_URL" psxrecomp
if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    git submodule add -b "$RECOMP_UI_REF" "$RECOMP_UI_URL" recomp-ui
fi
git submodule update --init --recursive

# RetComM-themed default app icon (Windows .ico + PNG for packaging).
if [ -d psxrecomp/assets ]; then
    mkdir -p "$ROOT/assets"
    for _icon in psxrecomp.svg psxrecomp.png psxrecomp.ico; do
        if [ -f "psxrecomp/assets/$_icon" ]; then
            cp -f "psxrecomp/assets/$_icon" "$ROOT/assets/$_icon"
        fi
    done
fi

git -C psxrecomp checkout --detach -q HEAD
git add psxrecomp
if [ -d "$ROOT/assets" ]; then
    git add assets 2>/dev/null || true
fi
if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    git -C recomp-ui checkout --detach -q HEAD
    git add recomp-ui
fi
if [ -d psxrecomp/lib/recomp-net ]; then
    # Nested submodule pins a SHA inside psxrecomp — not live remote main.
    # Optional override so netplay can follow a branch that has rollback.h etc.
    if [ -n "$RECOMP_NET_REF" ]; then
        echo "== Override recomp-net → $RECOMP_NET_REF =="
        git -C psxrecomp/lib/recomp-net fetch -q origin "$RECOMP_NET_REF"
        git -C psxrecomp/lib/recomp-net checkout --detach -q FETCH_HEAD
        git -C psxrecomp add lib/recomp-net
    else
        git -C psxrecomp/lib/recomp-net checkout --detach -q HEAD 2>/dev/null || true
    fi
fi

echo "== Framework pins =="
if [ -x psxrecomp/tools/ci/record_pins.sh ]; then
    bash psxrecomp/tools/ci/record_pins.sh --root "$ROOT" | tee "$ROOT/framework_pins.txt"
else
    {
        echo "psxrecomp=$(git -C psxrecomp rev-parse HEAD)"
        if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
            echo "recomp-ui=$(git -C recomp-ui rev-parse HEAD)"
        else
            echo "recomp-ui=<not added>"
        fi
        if [ -d psxrecomp/lib/recomp-net ]; then
            echo "recomp-net=$(git -C psxrecomp/lib/recomp-net rev-parse HEAD 2>/dev/null || echo missing)"
        fi
    } | tee "$ROOT/framework_pins.txt"
fi

if [ "$ENABLE_NETPLAY" -eq 1 ] && [ ! -f psxrecomp/lib/recomp-net/CMakeLists.txt ]; then
    echo "warning: netplay enabled but psxrecomp/lib/recomp-net missing after recursive update." >&2
fi

echo "== Packager stub =="
fill_template "$TEMPLATE_DIR/package_setup_release.sh.in" \
    "$ROOT/scripts/package_setup_release.sh"
chmod +x "$ROOT/scripts/package_setup_release.sh"

CI_WORKFLOW_OK=0
if [ "$ENABLE_CI" -eq 1 ]; then
    echo "== CI release workflow =="
    WF_SRC="$ROOT/psxrecomp/docs/ci/templates/setup-release.yml"
    if [ -f "$WF_SRC" ]; then
        mkdir -p "$ROOT/.github/workflows"
        python3 "$FILL_TOKENS" "$WF_SRC" "$ROOT/.github/workflows/release.yml" \
            --ci-placeholders \
            --set "ZIP_PREFIX=$ZIP_PREFIX" \
            --set "GAME_TITLE=$WINDOW_TITLE" \
            --set "WINDOW_TITLE=$WINDOW_TITLE"
        echo "  wrote .github/workflows/release.yml"
        CI_WORKFLOW_OK=1
    else
        echo "error: CI enabled but missing $WF_SRC" >&2
        echo "       Bump --psxrecomp-ref to a pin that has docs/ci/templates/." >&2
        exit 1
    fi
else
    echo "== CI workflow skipped =="
fi

DISC_BASENAME=""
PROBED=0
SEED_COUNT=0
mkdir -p "$ROOT/disc"
DISC_ABS=$(CDPATH= cd -- "$(dirname -- "$DISC")" && pwd)/$(basename -- "$DISC")
DISC_BASENAME=$(basename -- "$DISC_ABS")
case "$DISC" in
    *.cue|*.CUE) ;;
    *)
        echo "warning: prefer a Redump-style .cue (with sibling .bin tracks)." >&2
        ;;
esac

PROBE_CUE="$DISC_ABS"
DISC_TOML_PATH="$DISC_ABS"
GEN_DISC_HINT="$DISC_ABS"

if [ "$STAGE_DISC" -eq 1 ]; then
    echo "== Staging full disc dump into disc/ (optional; large) =="
    python3 - "$DISC_ABS" "$ROOT/disc" <<'PY'
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
print("  disc staged under disc/ (gitignored — never commit dumps)")
PY
    PROBE_CUE="$ROOT/disc/$DISC_BASENAME"
    DISC_TOML_PATH="disc/$DISC_BASENAME"
    GEN_DISC_HINT="disc/$DISC_BASENAME"
else
    echo "== External disc (no full copy) =="
    echo "  cue remains at: $DISC_ABS"
    echo "  will extract boot EXE only into disc/"
fi

case "$DISC_BASENAME" in
    *.cue|*.CUE)
        echo "== Probing disc (identity + seeds + TOC fp) =="
        if python3 "$PROBE_DISC" "$PROBE_CUE" \
            --json-out "$ROOT/disc_probe.json" \
            --write-game-toml "$ROOT/game.toml" \
            --write-catalog "$ROOT/catalog_identity.json" \
            --write-seeds "$ROOT/seeds/ghidra_funcs.txt" \
            --write-boot-exe "$ROOT/disc" \
            --disc-rel "$DISC_TOML_PATH" \
            --out-dir disc \
            --players "$PLAYERS" \
            --display-name "$GAME_NAME" \
            --description "$DESCRIPTION" \
            --publisher "$PUBLISHER" \
            --year "$YEAR" \
            --region "$REGION"; then
            PROBED=1
            SEED_COUNT=$(grep -c '^0x' "$ROOT/seeds/ghidra_funcs.txt" 2>/dev/null || echo 0)
            BOOT_FROM_TOML=$(python3 -c '
import re,sys
t=open(sys.argv[1],encoding="utf-8").read()
m=re.search(r"boot_exe\s*=\s*\"([^\"]+)\"", t)
print(m.group(1) if m else "")
' "$ROOT/game.toml")
            ENTRY_FROM_TOML=$(python3 -c '
import re,sys
t=open(sys.argv[1],encoding="utf-8").read()
m=re.search(r"entry_pc\s*=\s*\"([^\"]+)\"", t)
print(m.group(1) if m else "")
' "$ROOT/game.toml")
            if [ -n "$BOOT_FROM_TOML" ] && [ "$BOOT_FROM_TOML" != "$BOOT_EXE" ]; then
                BOOT_EXE="$BOOT_FROM_TOML"
                fill_template "$TEMPLATE_DIR/CMakeLists.txt.in" "$ROOT/CMakeLists.txt"
                fill_template "$TEMPLATE_DIR/codegen_setup.c.in" "$ROOT/codegen_setup.c"
                echo "  synced boot EXE → $BOOT_EXE (CMake / codegen)"
            fi
            if [ -n "$ENTRY_FROM_TOML" ]; then
                ENTRY_PC="$ENTRY_FROM_TOML"
                fill_template "$TEMPLATE_DIR/symbols.toml.in" "$ROOT/symbols.toml"
                echo "  symbols.toml BootEntry → $ENTRY_PC"
            fi
            if [ "$STAGE_DISC" -eq 0 ]; then
                echo "  game.toml disc → $DISC_TOML_PATH"
                echo "  (local absolute path — do not commit machine-specific dumps)"
            fi
        else
            echo "warning: disc probe failed — left template game.toml; fill by hand." >&2
        fi
        ;;
    *)
        echo "warning: --disc is not a .cue; skipped probe autofill." >&2
        ;;
esac

if [ "$FETCH_BOXART_FLAG" -eq 1 ]; then
    echo "== Fetching libretro boxart =="
    CUE_HINT="${DISC_BASENAME:-$GAME_NAME}"
    if python3 "$FETCH_BOXART" \
        --out "$ROOT/launcher_assets/img/boxart.tga" \
        --cue-stem "$CUE_HINT" \
        --display-name "$GAME_NAME"; then
        HAS_BOXART=1
        printf '%s\n' '    LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"' \
            >"$BOXART_BLOCK_FILE"
        fill_template "$TEMPLATE_DIR/CMakeLists.txt.in" "$ROOT/CMakeLists.txt"
        echo "  wired LAUNCHER_BOXART in CMakeLists.txt"
        PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" python3 -c \
            'from pathlib import Path; import sys; from project_studio.readme_metrics import inject_readme_boxart; inject_readme_boxart(Path(sys.argv[1]), sys.argv[2])' \
            "$ROOT/README.md" "$GAME_NAME" \
            && echo "  injected boxart into README.md" \
            || echo "warning: README boxart inject failed" >&2
    else
        echo "warning: boxart fetch failed — leave LAUNCHER_BOXART commented." >&2
    fi
fi

echo "== Sync symbols header =="
(
    cd "$ROOT"
    python3 tools/sync_symbols.py --game "$GAME_NAME" || true
)

git add CMakeLists.txt game.toml codegen_setup.c codegen_setup.h .gitignore VERSION README.md seeds scripts tools mods framework_pins.txt symbols.toml psx_symbols.h || true
if [ -d "$ROOT/.github" ]; then
    git add .github || true
fi
if [ -f "$ROOT/catalog_identity.json" ]; then
    git add catalog_identity.json || true
fi
if [ -f "$ROOT/disc_probe.json" ]; then
    git add disc_probe.json || true
fi
if [ "$HAS_BOXART" -eq 1 ]; then
    git add launcher_assets/img/boxart.tga launcher_assets/img/boxart.png launcher_assets/img/BOXART_SOURCE.txt || true
fi
COMMITTED=0
if git -c user.email=setup@localhost -c user.name=setup \
    commit -q -m "Initial New Project Layout scaffold" 2>/dev/null; then
    COMMITTED=1
else
    echo "  (skipped initial commit — commit manually when ready)"
fi

# Create the GitHub repo + origin remote now, but do NOT push yet.
# Push once after generate/build so CI workflow and final pins land together
# (early --push caused a second local "initial" commit to conflict on re-run).
GITHUB_CREATED=0
if [ "${CREATE_GITHUB:-0}" -eq 1 ]; then
    echo "== GitHub repo (gh, create only — push deferred) =="
    if ! command -v gh >/dev/null 2>&1; then
        echo "warning: gh not installed — skip create; push manually later." >&2
    elif [ "$COMMITTED" -eq 0 ]; then
        echo "warning: no initial commit — skip gh repo create." >&2
    else
        VIS_FLAG="--private"
        [ "$GITHUB_VISIBILITY" = "public" ] && VIS_FLAG="--public"
        [ "$GITHUB_VISIBILITY" = "internal" ] && VIS_FLAG="--internal"
        GH_SLUG="$GITHUB_OWNER/$GITHUB_REPO"
        GH_DESC=$(PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
            python3 -c 'from fill_tokens import GITHUB_ABOUT_DESCRIPTION; print(GITHUB_ABOUT_DESCRIPTION)')
        GH_HOME=$(PYTHONPATH="$SCRIPT_DIR${PYTHONPATH:+:$PYTHONPATH}" \
            python3 -c 'from fill_tokens import GITHUB_ABOUT_HOMEPAGE; print(GITHUB_ABOUT_HOMEPAGE)')
        if gh repo create "$GH_SLUG" $VIS_FLAG --source="$ROOT" --remote=origin \
            --description "$GH_DESC" --homepage "$GH_HOME"; then
            echo "  created origin ($GITHUB_VISIBILITY); push deferred until end"
            GITHUB_CREATED=1
        else
            echo "warning: gh repo create failed — if the repo already exists," >&2
            echo "         add origin and push manually at the end." >&2
            if ! git remote get-url origin >/dev/null 2>&1; then
                _gh_url=$(gh repo view "$GH_SLUG" --json url -q .url 2>/dev/null || true)
                if [ -n "$_gh_url" ]; then
                    git remote add origin "$_gh_url"
                    echo "  attached existing origin → $_gh_url"
                    GITHUB_CREATED=1
                fi
            else
                GITHUB_CREATED=1
            fi
        fi
        if [ "$GITHUB_CREATED" -eq 1 ]; then
            if gh repo edit "$GH_SLUG" --description "$GH_DESC" --homepage "$GH_HOME"; then
                echo "  GitHub About: R.A.I.D. description + Discord homepage"
            else
                echo "warning: gh repo edit (About) failed — set description/homepage by hand." >&2
            fi
        fi
    fi
fi

GEN_DISC_HINT="${GEN_DISC_HINT:-disc/$DISC_BASENAME}"
GENERATED_OK=0
BUILD_OK=0
if [ "$DO_GENERATE" -eq 1 ]; then
    echo "== Generate (emitters + OpenBIOS + game C) =="
    if [ ! -x "$ROOT/psxrecomp/tools/ci/build_emitters.sh" ]; then
        echo "error: missing psxrecomp/tools/ci/build_emitters.sh" >&2
        exit 1
    fi
    (
        cd "$ROOT"
        bash psxrecomp/tools/ci/build_emitters.sh
        # Quote disc/bios paths (Redump cues often contain spaces).
        set -- --config game.toml --project-root . --disc "$GEN_DISC_HINT"
        if [ -n "$BIOS_PATH" ]; then
            BIOS_ABS=$(CDPATH= cd -- "$(dirname -- "$BIOS_PATH")" && pwd)/$(basename -- "$BIOS_PATH")
            set -- "$@" --bios "$BIOS_ABS"
        fi
        python3 psxrecomp/psxrecomp_cli.py generate "$@"
    ) && GENERATED_OK=1
    if [ "$GENERATED_OK" -eq 1 ]; then
        echo "  generate OK (generated/ is gitignored — not committed)"
    else
        echo "warning: generate failed — fix seeds/disc and re-run generate by hand." >&2
        DO_BUILD=0
    fi
fi

if [ "${DO_BUILD:-0}" -eq 1 ] && [ "$GENERATED_OK" -eq 1 ]; then
    echo "== Configure & build psx-runtime =="
    if (
        cd "$ROOT"
        cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build build-release --target psx-runtime
    ); then
        BUILD_OK=1
        echo "  build OK → build-release/ (gitignored)"
    else
        echo "warning: build failed — fix and re-run cmake by hand." >&2
    fi
fi

# Return registered release.yml workflow name, or empty.
github_release_workflow_name() {
    _owner=$1
    gh api "repos/${_owner}/actions/workflows" \
        --jq '.workflows[] | select(.path|endswith("release.yml")) | .name' \
        2>/dev/null | head -n1 || true
}

# After the first push, Actions sometimes does not index release.yml (especially
# if the repo was private at first push). Poll, then nudge with a tiny commit.
ensure_actions_registers_release_yml() {
    _owner=$1
    _wf_path=".github/workflows/release.yml"
    [ -f "$_wf_path" ] || return 0
    command -v gh >/dev/null 2>&1 || return 0

    # If the user asked for public, force visibility before re-checking (idempotent).
    if [ "${GITHUB_VISIBILITY}" = "public" ]; then
        gh repo edit "$_owner" --visibility public --accept-visibility-change-consequences \
            >/dev/null 2>&1 || true
    fi

    _i=0
    while [ "$_i" -lt 5 ]; do
        _wf=$(github_release_workflow_name "$_owner")
        if [ -n "$_wf" ]; then
            echo "  Actions workflow registered: $_wf"
            echo "  (runs on workflow_dispatch or push of v* tags)"
            return 0
        fi
        _i=$((_i + 1))
        [ "$_i" -lt 5 ] && sleep 2
    done

    echo "  Actions has not listed release.yml yet — nudging with a follow-up commit…"
    {
        printf '\n'
        printf '# Actions registration nudge (%s UTC)\n' "$(date -u +%Y-%m-%dT%H:%MZ)"
    } >>"$_wf_path"
    git add "$_wf_path"
    if git -c user.email=setup@localhost -c user.name=setup \
        commit -q -m "ci: nudge Actions to register release.yml"; then
        if git push origin HEAD; then
            _i=0
            while [ "$_i" -lt 6 ]; do
                _wf=$(github_release_workflow_name "$_owner")
                if [ -n "$_wf" ]; then
                    echo "  Actions workflow registered after nudge: $_wf"
                    echo "  (runs on workflow_dispatch or push of v* tags)"
                    return 0
                fi
                _i=$((_i + 1))
                [ "$_i" -lt 6 ] && sleep 2
            done
            echo "warning: release.yml nudged but Actions still has not listed it;" >&2
            echo "         open the Actions tab once, or re-push after making the repo public." >&2
        else
            echo "warning: nudge commit created but push failed." >&2
        fi
    else
        echo "warning: could not create Actions nudge commit." >&2
    fi
}

# Single publish after scaffold (+ optional generate/build).
GITHUB_PUSHED=0
if [ "${CREATE_GITHUB:-0}" -eq 1 ] && git remote get-url origin >/dev/null 2>&1; then
    echo "== GitHub push (final) =="
    if [ "$COMMITTED" -eq 0 ]; then
        echo "warning: no local commit to push." >&2
    elif git push -u origin HEAD; then
        GITHUB_PUSHED=1
        echo "  pushed $(git rev-parse --abbrev-ref HEAD) → origin"
        if [ "$CI_WORKFLOW_OK" -eq 1 ] && command -v gh >/dev/null 2>&1; then
            _owner_repo=$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || true)
            if [ -n "$_owner_repo" ]; then
                ensure_actions_registers_release_yml "$_owner_repo"
            fi
        fi
    else
        echo "warning: git push failed (non-fast-forward if origin already has" >&2
        echo "         a different initial commit). Resolve with:" >&2
        echo "           git fetch origin && git push -u origin HEAD" >&2
        echo "         or, if this local tree should win: git push -u origin HEAD --force" >&2
    fi
fi

if [ "$PROBED" -eq 1 ]; then
    STEP1="Review game.toml + catalog_identity.json + seeds/ghidra_funcs.txt ($SEED_COUNT JAL seeds)."
else
    STEP1="Edit game.toml / re-run probe_disc.py --write-seeds."
fi

BUILD_HINT="cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release --target psx-runtime"
if [ "$BUILD_OK" -eq 1 ]; then
    STEP2="Runtime already built under build-release/ — soak offline boot."
elif [ "$GENERATED_OK" -eq 1 ]; then
    STEP2="Build playable runtime:

       $BUILD_HINT"
else
    STEP2="Build emitters, Generate, then playable runtime:

       ./psxrecomp/tools/ci/build_emitters.sh
       python3 psxrecomp/psxrecomp_cli.py generate \\
         --config game.toml --project-root . --disc \"$GEN_DISC_HINT\"
       $BUILD_HINT"
fi

CI_NOTE="CI workflow not installed (declined)."
if [ "$ENABLE_CI" -eq 1 ] && [ "$CI_WORKFLOW_OK" -eq 1 ]; then
    CI_NOTE="CI: .github/workflows/release.yml ready (zip prefix=$ZIP_PREFIX; submodule gitlinks pin the build)."
    if [ "$GITHUB_PUSHED" -eq 1 ]; then
        CI_NOTE="$CI_NOTE Pushed — open Actions → Release builds (workflow_dispatch)."
    fi
fi

GH_NOTE="GitHub remote not created (declined)."
if [ "${CREATE_GITHUB:-0}" -eq 1 ]; then
    if [ "$GITHUB_PUSHED" -eq 1 ]; then
        GH_NOTE="GitHub: created/attached origin and pushed ($GITHUB_VISIBILITY)."
    elif [ "$GITHUB_CREATED" -eq 1 ]; then
        GH_NOTE="GitHub: origin ready but push failed — see warnings above."
    else
        GH_NOTE="GitHub: create/push did not complete — check gh auth / remote."
    fi
fi

cat <<EOF

== Done ==

Next steps:
  1. $STEP1
  2. $STEP2
  3. Label symbols in symbols.toml → python3 tools/sync_symbols.py
  4. Soak offline boot → netplay QA (if enabled) → tag vX.Y.Z.
  5. $CI_NOTE
     Pins: submodule gitlinks (framework_pins.txt is an optional snapshot)
  6. $GH_NOTE

Docs: psxrecomp/docs/GAME_PROJECT_SETUP.md · psxrecomp/docs/SYMBOLS.md
EOF
