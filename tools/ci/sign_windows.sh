#!/usr/bin/env bash
# Authenticode-sign Windows binaries (.exe / .dll) for a release, or do nothing.
#
# Usage:
#   sign_windows.sh <file-or-dir>...      # dirs are searched for *.exe / *.dll
#
# Certificate, from the environment (a CI secret; never a file in the repo):
#   WINDOWS_SIGN_PFX_BASE64   the PKCS#12 certificate, base64  -- OR --
#   WINDOWS_SIGN_PFX          path to the .pfx (local use)
#   WINDOWS_SIGN_PFX_PASSWORD its password (may be empty)
#   WINDOWS_SIGN_TIMESTAMP_URL RFC 3161 server (default: DigiCert); the
#                             timestamp is what keeps a signature valid after
#                             the certificate itself expires
#   WINDOWS_SIGN_DESCRIPTION  optional /d text shown in the UAC / properties UI
#   SIGNTOOL                  optional path to signtool.exe
#
# NO CERTIFICATE = NO-OP, exit 0, with a notice. That is deliberate: every
# packager calls this unconditionally, so a fork or a local build without
# the secret still packages -- it just ships unsigned, exactly as before.
# A certificate that is present but fails to sign IS fatal: a release that
# was configured to be signed must not quietly ship unsigned.
#
# Why this exists: Windows 11 Smart App Control allows an executable only if
# it carries a valid Authenticode signature or the exact file hash already
# has reputation. A fresh unsigned build has neither and is blocked with no
# "run anyway". DLLs are signed too: the policy covers them as well.
#
# Signs with signtool (Windows SDK, on the windows-* runners) or, on a
# non-Windows host, osslsigncode if installed.
set -euo pipefail

if [[ $# -eq 0 ]]; then
  echo "usage: $0 <file-or-dir>..." >&2
  exit 2
fi

notice() {
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then echo "::notice::$*"; else echo "note: $*"; fi
}

PFX_B64="${WINDOWS_SIGN_PFX_BASE64:-}"
PFX_PATH="${WINDOWS_SIGN_PFX:-}"
PFX_PASS="${WINDOWS_SIGN_PFX_PASSWORD:-}"
TS_URL="${WINDOWS_SIGN_TIMESTAMP_URL:-http://timestamp.digicert.com}"
DESC="${WINDOWS_SIGN_DESCRIPTION:-}"

if [[ -z "${PFX_B64}" && -z "${PFX_PATH}" ]]; then
  notice "Windows code signing skipped: no WINDOWS_SIGN_PFX_BASE64 / WINDOWS_SIGN_PFX (binaries ship unsigned)"
  exit 0
fi

# ---- collect targets -------------------------------------------------------
FILES=()
for t in "$@"; do
  if [[ -d "${t}" ]]; then
    # An embedded toolchain pack is hundreds of third-party binaries the
    # player only compiles with; the shipped program is what gets signed.
    while IFS= read -r -d '' f; do FILES+=("${f}"); done < <(
      find "${t}" -type f \( -iname '*.exe' -o -iname '*.dll' \) \
           -not -path '*/toolchain/*' -print0 | sort -z)
  elif [[ -f "${t}" ]]; then
    FILES+=("${t}")
  else
    echo "error: no such file or directory: ${t}" >&2
    exit 1
  fi
done
if [[ ${#FILES[@]} -eq 0 ]]; then
  notice "Windows code signing: nothing to sign under: $*"
  exit 0
fi

# ---- materialize the certificate ------------------------------------------
TMP_PFX=""
cleanup() { [[ -n "${TMP_PFX}" ]] && rm -f "${TMP_PFX}"; }
trap cleanup EXIT
if [[ -z "${PFX_PATH}" ]]; then
  TMP_PFX="$(mktemp "${TMPDIR:-/tmp}/sign-XXXXXX.pfx")"
  if ! printf '%s' "${PFX_B64}" | base64 -d >"${TMP_PFX}" 2>/dev/null; then
    echo "error: WINDOWS_SIGN_PFX_BASE64 is not valid base64" >&2
    exit 1
  fi
  PFX_PATH="${TMP_PFX}"
fi
if [[ ! -s "${PFX_PATH}" ]]; then
  echo "error: certificate file is empty or missing: ${PFX_PATH}" >&2
  exit 1
fi

# ---- pick a signer ---------------------------------------------------------
host="$(uname -s)"
SIGNER=""
if [[ "${host}" == MINGW* || "${host}" == MSYS* || "${host}" == CYGWIN* ]]; then
  if [[ -n "${SIGNTOOL:-}" && -x "${SIGNTOOL}" ]]; then
    SIGNER="${SIGNTOOL}"
  elif command -v signtool >/dev/null 2>&1; then
    SIGNER="$(command -v signtool)"
  else
    # Windows SDK: newest kit, x64 tools.
    for kits in "/c/Program Files (x86)/Windows Kits/10/bin" "/c/Program Files/Windows Kits/10/bin"; do
      [[ -d "${kits}" ]] || continue
      cand="$(ls -d "${kits}"/10.*/x64/signtool.exe 2>/dev/null | sort -V | tail -1 || true)"
      if [[ -n "${cand}" ]]; then SIGNER="${cand}"; break; fi
    done
  fi
  if [[ -z "${SIGNER}" ]]; then
    echo "error: signtool.exe not found (install the Windows SDK or set SIGNTOOL)" >&2
    exit 1
  fi
  MODE=signtool
else
  if command -v osslsigncode >/dev/null 2>&1; then
    SIGNER="$(command -v osslsigncode)"
    MODE=osslsigncode
  else
    echo "error: a certificate is configured but this host has no signtool (Windows) or osslsigncode" >&2
    exit 1
  fi
fi

winpath() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }

# Timestamp servers are the flaky part of signing; give each file a few tries.
sign_one() {
  local f="$1" attempt rc
  for attempt in 1 2 3 4; do
    rc=0
    if [[ "${MODE}" == signtool ]]; then
      local args=(sign /fd SHA256 /td SHA256 /tr "${TS_URL}" /f "$(winpath "${PFX_PATH}")")
      [[ -n "${PFX_PASS}" ]] && args+=(/p "${PFX_PASS}")
      [[ -n "${DESC}" ]] && args+=(/d "${DESC}")
      "${SIGNER}" "${args[@]}" "$(winpath "${f}")" >/dev/null 2>&1 || rc=$?
    else
      local out="${f}.signed.$$"
      local args=(sign -pkcs12 "${PFX_PATH}" -h sha256 -ts "${TS_URL}" -in "${f}" -out "${out}")
      [[ -n "${PFX_PASS}" ]] && args+=(-pass "${PFX_PASS}")
      [[ -n "${DESC}" ]] && args+=(-n "${DESC}")
      if "${SIGNER}" "${args[@]}" >/dev/null 2>&1; then mv -f "${out}" "${f}"; else rc=$?; rm -f "${out}"; fi
    fi
    [[ ${rc} -eq 0 ]] && return 0
    sleep $((attempt * 5))
  done
  return 1
}

verify_one() {
  local f="$1"
  if [[ "${MODE}" == signtool ]]; then
    "${SIGNER}" verify /pa /q "$(winpath "${f}")" >/dev/null 2>&1
  else
    "${SIGNER}" verify -in "${f}" >/dev/null 2>&1
  fi
}

echo "signing ${#FILES[@]} Windows binaries with ${MODE} (timestamp: ${TS_URL})"
failed=0
for f in "${FILES[@]}"; do
  if sign_one "${f}" && verify_one "${f}"; then
    echo "  signed  ${f}"
  else
    echo "  FAILED  ${f}" >&2
    failed=1
  fi
done
if [[ ${failed} -ne 0 ]]; then
  echo "error: code signing failed for at least one file (see above)" >&2
  exit 1
fi
