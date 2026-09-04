# third_party/ — vendored dependency archives

Source archives the build would otherwise download, staged here so a checkout
can configure with no network at all.

`deps.manifest` is the single pin record — name, archive file, SHA256, upstream
URL. Both `cmake/psx_dependency_archive.cmake` and `tools/ci/vendor_deps.sh`
read it, so a vendored archive can never drift from the pin the build would
have fetched. A vendored archive is verified against the same `URL_HASH` as a
downloaded one; nothing here is trusted because it is local.

## What is committed, and why only that

Only **libchdr** (`libchdr-<sha>.tar.gz`, ~520 KB, BSD-3-Clause — see
`../THIRD_PARTY_ATTRIBUTION.md`).

`runtime/runtime.cmake` includes `chd_dependency.cmake` unconditionally, and
libchdr has no `find_package` path and ships in no toolchain pack — so it is
both the *first* network access a build makes and the only one with no local
fallback. A player building a released game behind a firewall used to fail
there, inside a FetchContent subbuild, before reaching any other dependency.

SDL3 (~15 MB) and zlib resolve from a system package or a toolchain pack first
and are **not** committed; `.gitignore` keeps them out. Stage them on demand:

```sh
tools/ci/vendor_deps.sh              # every dependency in the manifest
tools/ci/vendor_deps.sh SDL3         # just one
tools/ci/vendor_deps.sh --check      # verify what is staged; never downloads
```

Then configure with `-DPSX_DEPS_OFFLINE=ON` to turn any remaining download
attempt into a hard error that names the missing archive, instead of a fetch
that stalls behind a proxy and fails three CMake frames deep.

## Adding a dependency

Add the row to `deps.manifest`, run `tools/ci/vendor_deps.sh <name>` to stage
and verify the archive, and resolve it in CMake through
`psxrecomp_dependency_archive()` / `psxrecomp_dependency_source_dir()` rather
than a literal `URL` — a literal URL is a build that cannot go offline. Commit
the archive only if the dependency has no local fallback and is small.
