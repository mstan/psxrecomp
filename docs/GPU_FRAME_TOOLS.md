# GP0 frame tools — who drew this pixel?

A set of tools that answer one question about a rendering bug: **which guest
function issued this primitive, and with which blend mode?**

```
tools/psx_gpu_frame.py       transport + GP0 decode + attribution (the library)
tools/gpu_frame_capture.py   capture a frame from a running game
tools/gpu_frame_layers.py    render one image per guest function
tools/gpu_frame_diff.py      diff a good frame against a bad one
tools/gpu_parity.py          frame-locked image parity vs DuckStation
tools/tests/test_gpu_frame.py
```

RetComM Studio's **Frames** tab is a viewer and launcher over exactly these
artifacts. It never decodes a GP0 packet itself, so a headless capture and the
GUI can never disagree about what a frame contained.

---

## Why this exists

`gpu_frame_dump` on the debug server has always stamped every GP0 packet with
the guest code that issued it — `func`, `pc`, `ra`, plus the linked-list OT rank
(`handle_gpu_frame_dump` in `runtime/src/debug_server.c`, `GpuGp0RingEntry` in
`runtime/include/gpu.h`). Nothing consumed that attribution, so "the glow is
opaque and the vignette is missing" stayed a description of a screenshot rather
than a pointer at a function.

---

## The provenance rule

Everything these tools report is **observed** from one execution of one frame,
and is labelled as such. It is never merged into a static claim.

A `func` in a frame dump proves that code ran and issued a primitive. It says
nothing about the call graph — the analyser's static coverage gap
(`FUNCTION_DISCOVERY.md`) stays exactly as wide as it was. Studio shows observed
counts and static names in different columns for the same reason.

---

## What is decoded faithfully, and what is not

**Faithful:**

* Vertices, including the 11-bit signed coordinate and the running GP0(E5) draw
  offset — the same offset `gp0_exec_*` applies in `gpu.c`.
* Packet word layouts for every polygon, line, rectangle, fill and copy form.
* The texpage latch. A textured polygon's own tpage word overwrites draw-mode
  state (`set_tpage_from_poly` in `gpu.c`), and later untextured polygons and
  rectangles — which carry no tpage word — inherit it. Getting this wrong
  mislabels precisely the semi-transparency mode you are usually chasing.
* All four semi-transparency blends, when layers are rendered:
  `0.5B+0.5F`, `B+F`, `B-F`, `B+0.25F` (GPUSTAT bits 5-6).

**Not:**

* Textures are never sampled. Texture pages, CLUTs and UVs are decoded and
  reported, so a wrong-CLUT hypothesis is testable, but a textured primitive
  renders as its command colour. Re-implementing texture sampling would buy
  fidelity the question does not need and add a whole new way to be wrong.
* The ring truncates each packet to `GPU_GP0_RING_MAX_WORDS` (12). Longer
  packets are decoded as far as recorded and flagged `truncated` — never
  guessed at.

---

## The build must actually have a debug server

**Release + `-DPSX_DEBUG_TOOLS=ON` is the combination you want.** Not a Debug
build: a Debug build of a recomp is far too slow to reach the frame you are
chasing.

`runtime/src/debug_server.c` is always compiled, but `debug_server_init()` is
only *called* when `PSX_NO_DEBUG_TOOLS` is undefined (`runtime/src/main.cpp`).
`PSX_DEBUG_TOOLS` defaults **ON** for `Debug` / `RelWithDebInfo` and **OFF** for
`Release` / `MinSizeRel` (`runtime/runtime.cmake`), so a plain
`cmake -DCMAKE_BUILD_TYPE=Release` produces a lean binary that opens no port at
all. Every "the tool won't connect" starts here.

```bash
cmake -S . -B build-release -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=ON
cmake --build build-release --target psx-runtime
```

Check an existing build dir without reconfiguring it:

```bash
grep -E '^(PSX_DEBUG_TOOLS|CMAKE_BUILD_TYPE):' build-release/CMakeCache.txt
```

`PSX_DEBUG_TOOLS` is an `option()`, so it lives in the cache: changing
`CMAKE_BUILD_TYPE` on an **existing** build dir does *not* flip it. Pass the
`-D` explicitly, or start a fresh build dir.

In RetComM Studio this is the **Build tab → Debug tools** row: a selector that
injects the flag, a **Configure for debugging** button that sets Release + ON in
one click, and a status line that reads the configured build dir and says
whether the server will be there. The Frames tab shows the same line when it
cannot connect, so "not connected" never has to be guessed at.

### The port is a runtime setting, not a build flag

The debug port comes from `game.toml`:

```toml
[runtime]
debug_port = 4370
```

If that key is absent the runtime uses the compiled `DEFAULT_DEBUG_PORT` (4370
for `psx-runtime`, 4380 for `psx-beetle`), and `--debug-port <n>` on the command
line overrides both. Studio's Frames tab defaults its port field to whatever the
selected project's `game.toml` says.

---

## You cannot pause the game, and you do not need to

`pause`, `continue`, `step` and `run_to_frame` were **removed** from
psx-runtime. `runtime/src/debug_server.c` still registers them, but only as
handlers that return an error explaining the migration — pause-step-read
synthesizes a snapshot ("what is state right NOW") instead of reading the
history the runtime already records continuously, and in this codebase it forced
the runtime into a wait loop where a dropped client looked like a freeze.

The replacement is better for chasing a render bug. The GP0 ring holds
`1 << 20` packets (`gpu.c`, `GP0_RING_CAP`) — several hundred frames — so the
workflow runs the other way round:

> Play until the bug is on screen. **Then** capture that frame, and the frames
> leading up to it.

Reaching backwards beats freezing forwards, and it is the only thing that works
at all for a glitch you cannot reliably stop on.

```bash
python3 $P/gpu_frame_capture.py --ring        # what can I still ask for?
# GP0 ring: 812344 packet(s) seen, capacity 1048576, 12 words/packet
#   capturable frames: 40918..41230
```

`gpu_frame_dump` on a frame that has been evicted returns an **empty** dump, not
an error — which reads exactly like "that frame drew nothing", a conclusion you
might act on. So `capture()` checks the span first and refuses, and Studio's
Frames tab shows the span next to the frame selector and marks an out-of-ring
frame in red.

What replaces each removed command:

| Gone | Use instead |
|---|---|
| `run_to_frame N` | `--frame N` against the ring (any frame it still holds) |
| `step` | `--frames N`, walking backwards from `--frame` |
| `pause` + read state | `frame_range` / `get_frame` (per-frame records), `fn_entry_dump`, `wtrace_dump` |
| `pause` + inspect drawing | `gpu_frame_dump frame=N` — what this whole toolchain does |

Savestates are unaffected: `savestate` is a real handler, and a savestate plus a
fixed frame number still makes a run reproducible.

---

## Transport

**One request per connection.** `io_thread_main()` in `debug_server.c` accepts,
reads a single line, replies, and closes. A client that holds the socket open
and sends a second command gets silence and then EOF, which is indistinguishable
from a hung emulator. `psx_gpu_frame.DebugConn` opens a socket per command;
`tools/debug_client.py`'s `query()` documents the same contract.

---

## Typical session

```bash
cd <your recomp project>
P=psxrecomp/tools

# 1. Play until the bug is on screen. Capture the newest frame as the suspect,
#    then reach BACK into the ring for one that still looked right.
python3 $P/gpu_frame_capture.py --tag bad --out analysis/frames --summary
python3 $P/gpu_frame_capture.py --frame 41100 --tag good --out analysis/frames --summary

# 2. What changed?
python3 $P/gpu_frame_diff.py analysis/frames/good.json analysis/frames/bad.json

# 3. Look at the bad frame one function at a time.
python3 $P/gpu_frame_layers.py analysis/frames/bad.json --out analysis/frames/bad-layers

# 4. Did the guest even run the same? (needs a patched DuckStation on 4371)
python3 $P/gpu_parity.py --frame 41230 --out analysis/frames/parity
```

`--frames N` also grabs the N-1 frames *before* `--frame`, walking backwards
through the ring — the frames leading up to the one you noticed are usually the
ones that explain it. A savestate plus a fixed frame number still makes the whole
thing reproducible.

A screenshot is only written for the live frame: the framebuffer holds the
present, not the historical frame you just dumped, and saving it beside an older
dump would label it as something it is not.

### Artifacts

| File | Contents |
|---|---|
| `<tag>.json` | the full dump: every decoded primitive, with `func`/`pc`/`ra`/`ot` |
| `<tag>.summary.json` | totals, opcode + blend-mode histograms, per-function attribution |
| `<tag>.opcodes.json` | the server's own `gpu_opcodes` histogram |
| `<tag>.png` | the presented frame |
| `<tag>-layers/composite.png` | every primitive, in issue order |
| `<tag>-layers/layer-<func>.png` | one function's contribution, RGBA, alpha = coverage |
| `<tag>-layers/sheet.png` | labelled contact sheet of all layers |
| `<tag>-layers/layers.json` | the layer index: per-function stats and file names |
| `diff.json` | machine-readable diff, `--json` |

A busy frame's dump runs to tens of megabytes. Anything that only wants to know
*who drew what* should read `<tag>.summary.json` instead — which is what Studio
does.

### Layers render over grey, on purpose

An isolated layer is drawn over neutral grey (`--layer-backdrop`, default 128),
not black. A `B-F` subtractive vignette against black is black, and an additive
glow against black is just the glow: both blends go invisible in exactly the
view meant to show them. The composite still starts from black, as the GPU does.

---

## Reading a diff

The report leads with the two findings that name a bug on their own:

* **a blend mode that disappeared** — if `B-F` drew ten primitives in the good
  frame and none in the bad one, whatever layer used that blend (a vignette, a
  shadow) is no longer being drawn at all;
* **a function that stopped drawing** — it stopped running, or stopped reaching
  its emit path.

Then, per function: `stopped drawing`, `lost semi-transparency`,
`started drawing`, `gained semi-transparency`, `count changed`.

The diff is over **counts and flags per (function, opcode)**, never over exact
geometry. An animating effect legitimately moves its vertices every frame;
geometry appears only as a sample, for context.

---

## Turning an address into a name

Every function the tools report is an address you can name:

```bash
python3 -m project_studio analyze set-symbol --root . --pc 0x8004ABCD \
    --name LandEffect_DrawRays --status guessed \
    --note "observed issuing GP0 primitives (frame capture)"
python3 tools/sync_symbols.py     # regenerates psx_symbols.h / PSX_FN_*
```

Studio's Frames tab has a **Save name** box that calls exactly this, so
`symbols.toml` keeps a single writer.

---

## Parity scope

`gpu_parity.py` compares **images**, not GP0 streams. Only DuckStation is
driven: it implements `run_to_frame`, psx-runtime does not, so the native side is
sampled where it already is and DuckStation is advanced to meet it. A residual
frame mismatch is reported rather than papered over. The DuckStation oracle
patch (`tools/duckstation/psxrecomp_oracle.patch`) implements `screenshot`,
`read_vram`, `gpu_state`, `run_to_frame`, `step` and `pause` — but not
`gpu_frame_dump`, so there is no packet stream to compare on that side. Adding
one to the patch is the obvious next step if image parity keeps saying "the
streams must differ" without saying where; until then, `tools/cosim.py` brackets
the divergence.

Getting an oracle on Linux: `python3 tools/duckstation_oracle.py all` builds and
installs a patched DuckStation into `~/.local/share/retcomm/oracle/duckstation/`
— outside any game repo, shared by every title — and `start --disc <cue>` runs it
headless on 4371. On Arch-family and NixOS hosts the build runs inside
`ubuntu:22.04` via podman, because upstream's CMake refuses those hosts outright
and its build scripts may not be patched. See `tools/duckstation/README.md`.

Both instances must be driven identically — same disc, same starting state.
Parity between two runs that were not driven the same way means nothing.

---

## Tests

```bash
python3 -m unittest discover -s tools/tests -p 'test_gpu_frame.py'
```

Covers the packet word layouts against `gpu.c`'s command lengths, the signed
vertex, the E5 offset, the textured-polygon tpage latch, the four blends,
truncation flagging, the attribution rollup, the diff verdicts, and a stub
server that reproduces the one-request-per-connection contract.
