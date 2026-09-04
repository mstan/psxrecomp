#!/usr/bin/env bash
# Stage the pinned dependency archives into third_party/ so a build needs no
# network. Reads third_party/deps.manifest — the same pin record
# cmake/psx_dependency_archive.cmake reads — so what lands here is byte-identical
# to what the build would otherwise have downloaded.
#
# Usage:
#   tools/ci/vendor_deps.sh                 # stage every dependency
#   tools/ci/vendor_deps.sh SDL3 psx_zlib   # stage only these
#   tools/ci/vendor_deps.sh --check         # verify what is staged; never download
#   tools/ci/vendor_deps.sh --force         # re-download even if present
#
# After a full run, configure with -DPSX_DEPS_OFFLINE=ON to make any remaining
# download attempt a hard error rather than a silent trip to github.com.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
THIRD_PARTY="${ROOT}/third_party"
MANIFEST="${THIRD_PARTY}/deps.manifest"

CHECK_ONLY=0
FORCE=0
WANTED=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --check) CHECK_ONLY=1; shift ;;
    --force) FORCE=1; shift ;;
    -h|--help)
      sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      exit 2
      ;;
    *) WANTED+=("$1"); shift ;;
  esac
done

if [[ ! -f "${MANIFEST}" ]]; then
  echo "error: missing manifest: ${MANIFEST}" >&2
  exit 1
fi

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "error: need sha256sum or shasum to verify archives" >&2
    exit 1
  fi
}

wanted() {
  [[ ${#WANTED[@]} -eq 0 ]] && return 0
  local n
  for n in "${WANTED[@]}"; do
    [[ "$n" == "$1" ]] && return 0
  done
  return 1
}

download() {
  # Prefer HTTP/1.1: GitHub release assets intermittently REFUSED_STREAM under
  # HTTP/2, which is the same failure prefetch_sdl3.sh works around.
  local url="$1" dest="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --http1.1 --retry 5 --retry-all-errors --retry-delay 2 \
      -o "${dest}" "${url}"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "${dest}" "${url}"
  else
    echo "error: need curl or wget to download ${url}" >&2
    exit 1
  fi
}

mkdir -p "${THIRD_PARTY}"

seen=0
fail=0
matched=()

while read -r name file sha url _rest; do
  case "${name}" in ""|\#*) continue ;; esac
  if [[ -z "${file}" || -z "${sha}" || -z "${url}" ]]; then
    echo "error: malformed manifest row: ${name} ${file} ${sha} ${url}" >&2
    fail=1
    continue
  fi
  wanted "${name}" || continue
  matched+=("${name}")
  seen=$((seen + 1))

  target="${THIRD_PARTY}/${file}"

  if [[ -f "${target}" && "${FORCE}" -eq 0 ]]; then
    have="$(sha256_of "${target}")"
    if [[ "${have}" == "${sha}" ]]; then
      echo "ok: ${name} — third_party/${file}"
      continue
    fi
    echo "error: ${name} — third_party/${file} digest mismatch" >&2
    echo "         manifest: ${sha}" >&2
    echo "         on disk:  ${have}" >&2
    if [[ "${CHECK_ONLY}" -eq 1 ]]; then
      fail=1
      continue
    fi
    echo "       re-downloading" >&2
  elif [[ "${CHECK_ONLY}" -eq 1 ]]; then
    echo "missing: ${name} — third_party/${file}" >&2
    fail=1
    continue
  fi

  echo "fetching: ${name} <- ${url}"
  tmp="$(mktemp "${THIRD_PARTY}/.${file}.XXXXXX")"
  # shellcheck disable=SC2064
  trap "rm -f '${tmp}'" EXIT
  download "${url}" "${tmp}"
  have="$(sha256_of "${tmp}")"
  if [[ "${have}" != "${sha}" ]]; then
    echo "error: ${name} digest mismatch after download" >&2
    echo "         want: ${sha}" >&2
    echo "         got:  ${have}" >&2
    rm -f "${tmp}"
    trap - EXIT
    fail=1
    continue
  fi
  mv -f "${tmp}" "${target}"
  chmod 0644 "${target}"
  trap - EXIT
  echo "ok: ${name} — third_party/${file}"
done <"${MANIFEST}"

if [[ ${#WANTED[@]} -gt 0 ]]; then
  for n in "${WANTED[@]}"; do
    found=0
    for m in "${matched[@]:-}"; do
      [[ "$m" == "$n" ]] && found=1
    done
    if [[ "${found}" -eq 0 ]]; then
      echo "error: no '${n}' row in ${MANIFEST}" >&2
      fail=1
    fi
  done
fi

if [[ "${seen}" -eq 0 ]]; then
  echo "error: nothing to do — no manifest rows selected" >&2
  exit 1
fi

if [[ "${fail}" -ne 0 ]]; then
  exit 1
fi

if [[ "${CHECK_ONLY}" -eq 1 ]]; then
  echo "all requested dependencies are vendored and verified."
else
  echo "vendored ${seen} dependenc$([[ ${seen} -eq 1 ]] && echo y || echo ies) into third_party/."
  echo "configure with -DPSX_DEPS_OFFLINE=ON to forbid any further download."
fi
