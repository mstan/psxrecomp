# Profile-Guided Optimization (PGO) for PSXRecomp games

PGO lets the compiler optimize the runtime using **profiles from a real play
session**, instead of guessing which paths are hot. For MotK (and other
MDEC/VLC-heavy titles), that can move intro-FMV host pace from roughly
**~39 FPS (LTO-only)** toward **~50 FPS med** offline under faithful
load-delay — without changing guest timing correctness.

PGO is a **host performance** tool. It does not make emulation more accurate.
Wrong timing “wins” (`PSX_LOAD_DELAY=0`, fake vsync accel) are still forbidden.

---

## Concepts

| Mode | CMake | What happens |
|------|--------|----------------|
| *(off)* | omit `PSX_PGO` or leave empty | Normal Release build |
| **generate** | `-DPSX_PGO=generate` | Instrument the binary; running it writes `.gcda` profiles |
| **use** | `-DPSX_PGO=use` | Rebuild using those profiles for layout/inlining |

Profiles are stored under the build tree, default:

```text
<build-dir>/pgo/
```

Override with `-DPSX_PGO_DIR=...` if needed.

Typical flags (GCC/Clang):

- generate: `-fprofile-generate=<dir>`
- use: `-fprofile-use=<dir> -fprofile-correction`

If the code changed a lot since the last train, you may see
**coverage-mismatch** warnings. Retrain (do not keep stale profiles forever).

---

## Where it is wired today

The MotK game repo enables PGO in its top-level `CMakeLists.txt` via the
`PSX_PGO` / `PSX_PGO_DIR` cache variables. Other game repos can copy the same
block.

Framework `runtime.cmake` does **not** turn PGO on by itself — each game
opts in.

---

## MotK: one-shot train script (recommended)

From **MastersOfTerasKasiRecomp** (repo root), with BIOS + disc available and
a working display:

```bash
# Optional knobs (defaults: 3 runs × 90s each):
#   PGO_TRAIN_RUNS=3 PGO_TRAIN_SECS=90
DISPLAY=:0 ./scripts/pgo_motk_intro.sh
```

What the script does:

1. Configures `build-release` with `-DPSX_PGO=generate` and builds.
2. Clears `<build>/pgo/`, then launches the game **without the launcher**
   (`--no-launcher`) for several timed runs so logo + crawl FMV stay hot.
3. Merges `.gcda` across runs (profiles accumulate in `pgo/`).
4. Reconfigures with `-DPSX_PGO=use` and rebuilds the optimized binary.

Requirements:

- Disc cue at `motk/Star Wars - Masters of Teras Kasi (USA).cue` (or edit
  the script).
- `DISPLAY` / `XAUTHORITY` set if you run from SSH or a non-default session
  (the script defaults `DISPLAY=:0`).
- Ninja + Release toolchain (same as a normal MotK Release build).

When it finishes:

```text
PGO use build ready: .../build-release/Masters_of_Teras_Kasi_Recompiled
```

---

## MotK: manual generate → train → use

Useful if you want a custom train path (netplay, specific scene, longer run).

### 1. Instrumented build

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOTK_NATIVE=ON \
  -DPSX_PGO=generate
cmake --build build-release --target psx-runtime -j"$(nproc)"
```

`MOTK_NATIVE=ON` (`-march=native`) is recommended for **local** play machines
only. Leave it **OFF** for portable CI/release packages.

### 2. Train (write profiles)

Wipe stale profiles when starting a fresh train:

```bash
rm -rf build-release/pgo && mkdir -p build-release/pgo
```

Run the heavy path you care about (intro FMV for MotK):

```bash
./build-release/Masters_of_Teras_Kasi_Recompiled \
  --no-launcher --game game.toml \
  --disc "motk/Star Wars - Masters of Teras Kasi (USA).cue"
```

Let logo + crawl play for ~60–90 seconds (or longer). Quit. Confirm profiles:

```bash
find build-release/pgo -name '*.gcda' | wc -l
# expect dozens/hundreds of files; 0 = train failed
```

You can run multiple sessions **without** deleting `pgo/` so counts merge.

### 3. Optimized rebuild

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOTK_NATIVE=ON \
  -DPSX_PGO=use
cmake --build build-release --target psx-runtime -j"$(nproc)"
```

Verify flags stuck:

```bash
grep '^MOTK_NATIVE' build-release/CMakeCache.txt   # ON for local
grep 'fprofile-use' build-release/build.ninja | head -1
```

---

## When to retrain

Retrain after changes that move hot code, especially:

- MDEC / DMA / CD / interrupt / cycle-charge edits
- Large `generated/` regenerations
- Interpreter / overlay hot-path changes
- Switching major compiler versions

Symptoms of stale profiles:

- `coverage-mismatch` / profile-use warnings at compile time
- Intro FMV FPS slides back toward the LTO-only band (~39) despite `-DPSX_PGO=use`

MotK issue log (`ISSUES.md`) treats intro PGO as part of the normal
`build-release` play configuration.

---

## What PGO is not

| Do | Don’t |
|----|--------|
| Train on the real host-bound scene (MotK intro FMV) | Expect 60 FPS offline if guest load-delay still dominates |
| Keep Release + LTO; add PGO on top | Use PGO as a substitute for faithful timing fixes |
| Retrain after big hot-path edits | Ship CI artifacts that depend on your machine’s `.gcda` without documenting it |
| Use `MOTK_NATIVE=ON` locally with PGO | Turn on `-march=native` for multi-arch release zips |

`PSX_LOAD_DELAY=0` still jumps to hundreds of FPS — that only proves host
cost, and is **not** a valid shipping mode.

---

## Adding PGO to another game repo

Copy MotK’s CMake fragment (before `include(.../runtime.cmake)`):

```cmake
set(PSX_PGO "" CACHE STRING "PGO mode: empty | generate | use")
set(PSX_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "PGO profile directory")
if(PSX_PGO STREQUAL "generate")
    file(MAKE_DIRECTORY "${PSX_PGO_DIR}")
    add_compile_options(-fprofile-generate=${PSX_PGO_DIR})
    add_link_options(-fprofile-generate=${PSX_PGO_DIR})
elseif(PSX_PGO STREQUAL "use")
    add_compile_options(-fprofile-use=${PSX_PGO_DIR} -fprofile-correction)
    add_link_options(-fprofile-use=${PSX_PGO_DIR})
endif()
```

Then write a small train script that:

1. Builds with `generate`
2. Runs your game’s expensive scene long enough to write `.gcda`
3. Rebuilds with `use`

GCC/Clang on Linux and macOS are the supported path. MSVC PGO uses a
different toolchain model and is not wired by this fragment.

---

## Related docs

- MotK: `ISSUES.md` (intro FMV host pace, PGO expectations)
- MotK: `scripts/pgo_motk_intro.sh` (automated train)
- Framework: `docs/BUILDING.md` (general build options)
