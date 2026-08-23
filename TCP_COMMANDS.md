# TCP Debug Server Commands

Protocol: **JSON over newline**, one object per line, responses on same connection.

- Request shape: `{"id": N, "cmd": "<command>", ...params}`
- Success: `{"id": N, "ok": true, ...data}`
- Failure: `{"id": N, "ok": false, "error": "<msg>"}`

There are **two live** servers, both implementing this protocol with overlapping command sets:

| Server | Port | Source |
|---|---|---|
| **Native** (our recompiled runtime) | `4370` | `runtime/src/debug_server.c` |
| **Beetle PSX** (oracle) | `4380` | `runtime/src/beetle_debug_server.c` |

> **DuckStation (port 4371) was retired as the oracle on 2026-05-05** and is no
> longer built from this repository — there is no `duckstation` entry in
> `.gitmodules`. The **D** column in the curated inventory below is historical
> and reflects DuckStation, not Beetle. For authoritative per-command
> native/Beetle coverage, use the generated
> [Complete command index](#complete-command-index-generated) at the bottom of
> this file, which is derived from the two servers' command tables.

The `debug_client.py` CLI can target either, or `compare` two at once to diff state live — that's how divergence hunts work.

```bash
python tools/debug_client.py <cmd> [args]           # native (port 4370)
python tools/debug_client.py --port 4380 <cmd>      # psx-beetle
python tools/debug_client.py --ds <cmd> [args]      # duckstation (port 4371)
python tools/debug_client.py compare <cmd>          # run on both, diff results
```

Commands without a bespoke CLI mapping pass through generically: extra
args of the form `key=value` become JSON fields (ints when numeric, else
strings), so every server command is reachable, e.g.
`debug_client.py --port 4370 gpu_frame_dump frame=14528 count=65536`.

---

## Command inventory

Columns: **N** = native, **D** = DuckStation oracle.

| Command | N | D | Params | Description |
|---|---|---|---|---|
| `ping` / `frame` | ✓ | ✓ | — | Heartbeat + current frame number |
| `get_registers` (`regs`) | ✓ | ✓ | — | All 32 GPRs + PC + HI + LO (native also: COP0 SR/Cause/EPC, I_STAT, I_MASK) |
| `read_ram` | ✓ | ✓ | `addr`, `len` | Read bytes from PS1 address space as hex string — up to the full 2 MB in ONE response line. `dump_ram` is an alias (the old chunked multi-line variant is gone: it broke the one-request/one-response protocol and wedged the server) |
| `write_ram` | ✓ | ✓ | `addr`, `val` | Write **one byte** to PS1 address space. Note the parameter is `val` (not `hex`), and the write is a single byte per call — this row previously documented both incorrectly |
| `read_scratch` |   | ✓ | `addr`, `len` | Read PS1 scratchpad (0x1F800000 region) |
| `read_vram` / `vram_peek` | ✓¹ | ✓ | `x`, `y`, `w`, `h` | Read 16-bit VRAM pixels (max 128×128) |
| `gpu_state` | ✓ | ✓ | — | Display area, display depth, draw offset, GPUSTAT, clip rect, xfer state |
| `screenshot_hires` | ✓ | ✓ | `path` | PNG of the **supersampled** surface (the present path the window uses), at `display × gr_scale()`. ⚠ `screenshot`/`screenshot_file` capture native 15-bit VRAM and are **blind to anything that only exists in the hi-res mirror** — geometry correction, SSAA edges, perspective UVs — so they show a clean frame while the player sees a broken one. Use this one to verify those. Falls back to the native resolve (and reports `scale: 1`) when no hi-res surface exists |
| `present_shot` | ✓ |   | `path` | PNG of the **composed present surface** — the frame after the backend fits the display buffer to the window, so it carries the presented aspect. ⚠ every other capture resolves the display buffer *before* that fit: on a 508×256 display in a 4:3 window they answer 508×256 while the player sees 640×480. Use this one for anything aspect-shaped (widescreen, letterbox), where a pre-fit buffer would hide the very stage the change touches. Staged and fulfilled on the next present, so the ack means *queued* — poll `present_shot_seq`. Unavailable headless and on the Vulkan backend (its swapchain has no readback hook) |
| `present_shot_seq` | ✓ |   | — | Completion counter for `present_shot`, plus `wrote` (1 = that completion produced a PNG). Sample before staging, poll until `seq` moves. Advances on success *and* failure, so the poll always terminates |
| `geom_correction` |   | ✓ | — | `[video] geometry_correction` / `perspective_texturing` engagement: enable flag plus free-running `geometry_vertex_hits` and `perspective_triangles` totals. Both enhancements silently fall back to the faithful path on anything they cannot prove is projected geometry, so a zero counter with the flag on means the title never qualifies — sample twice and diff for a rate |
| `sio_state` | ✓ | ✓ | — | SIO registers + (native only) pad/memcard protocol + TX/RX history |
| `irq_state` | ✓ | ✓ | — | `I_STAT`, `I_MASK` (both), plus chain state on native |
| `dma_state` | ✓ | ✓ | — | DPCR, DICR, all 7 channel states (madr/bcr/chcr) |
| `event_state` |   | ✓ | — | EvCB table summary (stub on DS — events are BIOS-level) |
| `overlay_state` |   | ✓ | — | Current overlay info |
| `cdrom_sector_dump` | ✓ |   | `offset`, `len` | Dump bytes from the last CD-ROM sector observed by the controller, including LBA/mode metadata |
| `cdrom_sector_history` | ✓ |   | `count`, optional `lba` | Dump newest CD-ROM sector history entries, including raw XA subheader fields, CPU/audio delivery flags, and the first 128 bytes |
| `cdrom_sector_history_clear` | ✓ |   | — | Reset the CD-ROM sector history ring |
| `watch` | ✓ | ✓ | `addr` | Set byte-level memory watchpoint (fires per-frame on change) |
| `unwatch` | ✓ | ✓ | `addr` | Remove memory watchpoint |
| `set_input` | ✓ | ✓ | `buttons`, optional `frames`, optional `lx`, `ly`, `rx`, `ry` | Override pad1 buttons and optional analog axes (PS1 inverted bitmask, 0 = pressed; axes 0-255). Holds until `clear_input` on both backends; pass `frames=N` (beetle) to auto-release after N frames |
| `clear_input` | ✓ | ✓ | — | Remove input and analog axis overrides |
| `turbo` | ✓ |   | `enabled` | Enable/disable TCP-controlled frontend turbo for fast-forward validation |
| `turbo_state` | ✓ |   | — | Query TCP-controlled turbo state |
| `pause` | ✓ |   | — | **REMOVED** — still registered, but always returns an error. Query a ring buffer (`fn_entry_tail`, `wtrace_dump`, `gpu_frame_dump`) instead of synthesizing a snapshot |
| `continue` (`c`) | ✓ |   | — | **REMOVED** — nothing to resume, since `pause` is gone |
| `step` | ✓ |   | — | **REMOVED** — query a ring buffer over the window of interest instead of advancing N frames synchronously |
| `run_to_frame` | ✓ |   | — | **REMOVED** — use `frame_range` / `read_frame_ram` against the live frame ring instead |
| `history` | ✓ | ✓ | — | Ring buffer stats (frames available) |
| `get_frame` | ✓ | ✓ | `frame` | Full frame record from ring buffer |
| `frame_range` | ✓ | ✓ | `start`, `end` | Range query, max 200 frames |
| `frame_timeseries` | ✓ | ✓ | `start`, `end` | Compact timeseries, max 200 frames |
| `set_snapshot` | ✓ | ✓ | `slot`, `addr`, `size` | Configure per-frame RAM snapshot region (slots 0-3) |
| `get_snapshots` | ✓ | ✓ | — | Show snapshot config |
| `screenshot` | ✓ | ✓ | `path` (optional) | Write a **PNG** of the current display to `path` (default `psx_screenshot.png` in the runtime cwd); single metadata response `{path,width,height}`. `screenshot_file` is an alias; the old inline-hex-row `screenshot` is gone (it streamed h+1 response lines per request and poisoned the connection) |
| `first_failure` | ✓ |   | — | Find first divergence point between runs (native-side tracking) |
| `read_frame_ram` | ✓ |   | `addr`, `len`, `frame` | Read RAM **as of a specific frame** (from ring buffer) |
| `wtrace_range` | ✓ |   | `lo`, `hi` | Set RAM-write trace range (ring of 262 144 writes with RA — `WRITE_TRACE_CAP`, `1 << 18`) |
| `wtrace_dump` | ✓ | beetle | optional `addr_lo`, `addr_hi`, `count`, `newest` | Dump RAM-write trace entries as JSON. The address filter is applied server-side over the FULL ring before the emit cap — always pass it when hunting a specific buffer, otherwise you only see the oldest `count` entries of the whole ring |
| `wtrace_clear` | ✓ |   | — | Reset the trace ring |
| `mmio_dump` | ✓ |   | optional `addr`, `count`, `newest` | Dump the always-on MMIO write ring (256K entries, ALL 0x1F801xxx writes — SPU/DMA traffic rolls it in well under a minute of gameplay; for display history use `gp1_dump`) |
| `mmio_clear` | ✓ |   | — | Reset the MMIO write ring |
| `gp1_dump` | ✓ |   | optional `frame_lo`, `frame_hi`, `count`, `newest` | Dump the dedicated ALWAYS-ON GP1 (0x1F801814 display control) ring — 512K entries ≈ 15 min of gameplay (Tomba writes ~10 GP1/frame), survives the general MMIO ring's eviction. Frame filter is server-side over the full ring. Each entry: val + func/pc/cpu_pc/ra/sp/a0/a1/sr/epc/frame |
| `pc_break` |   | ✓² | `addr` | DS execute breakpoint, state captured on hit (via `pc_hit_last`) |
| `pc_unbreak` |   | ✓² | `addr` | Remove an execute breakpoint |
| `pc_break_list` |   | ✓² | — | List active execute breakpoints |
| `pc_hit_last` |   | ✓² | — | Captured state (PC, $ra, all GPRs, COP0) from most recent PC break hit |
| `pc_hit_clear` |   | ✓² | — | Clear the last-hit record |
| `quit` | ✓ |   | — | Shutdown native runtime |

¹ Native `vram_peek` is the legacy name; DS calls it `read_vram`. Same semantics.
² The `pc_*` family is specific to the DS oracle: DuckStation's CPU core honours `CPU::AddBreakpointWithCallback`, while our native runtime dispatches whole recompiled functions (no mid-function PC breaks).

### Boot-time write ranges

Set `PSX_WTRACE_BOOT=lo,hi[;lo,hi...]` before launching a debug-tools build to
retain the first writes to one or more half-open RAM ranges from guest
instruction zero. Addresses may be hexadecimal or decimal; KSEG addresses are
normalized to physical addresses. For example, the Crash Bash investigation
that motivated this option can be reproduced without title-specific code:

```powershell
$env:PSX_WTRACE_BOOT='0x000B3A80,0x000B3B00'
.\CrashBashRecomp.exe
```

Connect at any later point and query `wtrace_boot_stats`,
`wtrace_boot_summary`, or `wtrace_boot_dump`. Each retained entry includes the
write address/value/width, guest PC and return address, register context, frame,
and DMA channel. The option is ignored in builds made with debug tools disabled.

---

## Divergence-hunt workflow

When a recompiled-BIOS bug is suspected, the two servers let you find the **first** divergence instead of chasing symptoms. Standard procedure (inherited from v3's `DEBUG.md`):

1. **Sync state via PC + registers, not frame number.** Frames drift after even a single timing glitch. Pause both servers; compare `get_registers` until they match.
2. **Dump both sides fully.** Compare `get_frame`, `gpu_state`, `irq_state`, `dma_state` (DS), `dump_ram` over the same regions.
3. **Byte-level comparison.** Tiny mismatches usually point at one subsystem. Use `debug_client.py compare <cmd>` for automatic diff.
4. **Find the earliest mismatch**, not a later symptom. Ring-buffer queries (`frame_range`, `read_frame_ram`) help locate which frame went wrong.
5. **Trace the write.** Use `watch` to catch the divergent store, or DS's `pc_break` on the suspect function entry. Look at `$ra` in `pc_hit_last` to identify the caller chain.
6. **Classify.** codegen (recompiler generates wrong instruction), runtime (MMIO or kernel simulation wrong), timing (IRQ cadence), or BIOS (real-hardware quirk we didn't model).
7. **Minimal fix** in the correct subsystem. Never hand-deliver state to hide the symptom (see CLAUDE.md §0).

---

## Server send budget + serve-stall telemetry

The server is pumped on the **main thread**: every millisecond it spends
sending a response is a millisecond the emulator does not run. Inline
responses are bounded — 2 s per zero-progress chunk and **15 s total per
response**; a client that exceeds the budget is disconnected (the runtime
never stalls indefinitely). Responses bigger than the budget allows must
use the `*_dump_file` variants, which write to disk instead of the socket.

Cumulative serve-stall is exported as `tcp_send_stall_ms` /
`tcp_clients_dropped` in `psx_freeze_heartbeat.json` (plus per-tick
`tcp_ms` in its ring) and in every wedge/fatal dump header. **Check these
first when diagnosing slow or stalled frames** — on 2026-06-10 two
"attract-idle degradations" turned out to be a TCP client trickle-draining
mega-dumps, throttling the main loop to 6 fps (all 8 watchdog stack
samples inside `WS2_32!send`). A slow-frames wedge with a large
`tcp_send_stall_ms` delta over the same window is observer interference,
not a guest bug.

## Call-contract (bail) telemetry

The dispatch call contract (Bug D family fix, 2026-06-10) guards every
generated continuation: it may only run if the guest actually returned to
the call site ($ra == site return address, $sp == caller's sp at the
call). Violations begin a "bail" unwind that abandons stale C frames and
re-dispatches the guest's true target. Counters in
`psx_freeze_heartbeat.json` and dump headers:

- `bail_first` — contract violations detected (wild returns). Nonzero
  during gameplay means the game executed a wild control transfer (e.g.
  Tomba's dead jumptable case `jal 0x80120B3C`, the chest freeze). A
  small count with the game continuing normally is the fix working.
- `bail_resolved` — unwinds that resolved at an enclosing call site whose
  contract matched (multi-level return).
- `bail_flattened` — unwinds that reached the outermost dispatch loop and
  re-dispatched the wild target on a clean host stack.
- `bail_anomaly` — bail flag observed at exception entry (must stay 0;
  anything else is a runtime bug).

---

## `bios_info` — linked recompiled-BIOS identity (native only)

Reports which BIOS image this build's recompiled C was generated from
(`psx_bios_image`, emitted into the generated dispatch from the BIOS profile)
and whether the loaded ROM matches it: `image_id`, `sha256`, `crc32`, `size`,
`bundled` (redistributable image shipped with the game), the kernel-bless
window, the HLE anchors (`shell_entry_phys` / `deliver_event_ret`; 0 =
structurally unavailable on this BIOS), and `image_wordsum` vs
`loaded_wordsum` with `match`. With the launch identity gate a running
process always reports `match:1`.

- `{"cmd":"bios_info"}`

## `s3_smear_watch` — callee-saved-register smear tripwire (native only)

Latches the first interpreted instruction in a PC window whose execution
changes `$s3` (`runtime/src/dirty_ram_interp.c`). A `jalr`'s exec_one spans
the entire nested native callee, so the latch names the callee that returned
with a clobbered callee-saved register; the insn ring is frozen at the latch.

- `{"cmd":"s3_smear_watch","lo":"<hex>","hi":"<hex>"}` — arm (each arming
  fully re-specifies the watch). Optional `"excl":"<hex insn>"`: exact
  encoding to ignore, so a watched loop's own `$s3` advance (e.g. an
  `addi s3,s3,8` list walk) doesn't trip the latch.
- `{"cmd":"s3_smear_watch"}` — report the latch: `valid`, `pc`, `insn`,
  `s3_old`/`s3_new`, `call_target` (rs at the call site for jr/jalr),
  `frame`.
- `{"cmd":"s3_smear_watch","lo":"0"}` — disarm.

## `callret_watch` — interp JALR call-resolution ring (native only)

64-entry ring (`runtime/src/dirty_ram_interp.c`) recording, for every
interpreted JALR whose call PC lies in a window, which resolution tier ran
the callee and the full post-call outcome — the complement of
`s3_smear_watch`: the tripwire names the callee that came back smeared, this
ring names the return path that let it come back.

- `{"cmd":"callret_watch","lo":"<hex>","hi":"<hex>"}` — arm (resets the ring).
- `{"cmd":"callret_watch"}` — dump (newest last): per entry `cyc`, `f`
  (frame), `pc`, `tgt`, `path` (`CRES_*` tier code, see the enum in
  dirty_ram_interp.c; `|0x100` = finish() escaped), pre-call
  `sp_b`/`ra_b`/`s0_b`/`s3_b`, post-call `pc_a`/`ra_a`/`sp_a`/`s0_a`/
  `s3_a`/`v0_a`, `bail`/`rfe`/`esc`/`in_exc` flags, `dstatic`/`dblocks`/
  `dexc` engine-attribution deltas across the call, `last_func`.
- `{"cmd":"callret_watch","disarm":true}` — disarm. The legacy
  `{"lo":"0"}` spelling (no `hi`) still disarms and says so in the reply.

The ring is armed iff the window is NON-EMPTY (`hi > lo`), so `lo` may be
`0` — `{"lo":"0","hi":"0x200000"}` really does watch the whole address
space. Every reply carries `armed`, and the dump carries `armed` plus the
active window, because an unarmed ring reports `total: 0` — which otherwise
reads exactly like a genuine "those calls never happened" answer. Passing
`lo` with no `hi` is refused rather than silently inheriting the previous
ceiling (usually `0`, i.e. an empty window).

## `hle_dump` — BIOS-HLE tier call ring (native only)

Always-on ring (`runtime/src/bios_hle.c`, 16K entries) recording every
A0/B0/C0 kernel-vector dispatch the HLE tier's hook observes, plus the boot
shell-skip event.

The hook is installed when EITHER axis is on (`bios_hle_plan.h`), so a run with
the boot-skip on but kernel calls left to LLE — the default on the bundled
OpenBIOS, which exports no `deliver_event_ret` — reports
`backend: "LLE (recompiled BIOS)"` and still fills the ring with `route: 0`
vector observations plus the one `route: 2` boot entry. Only a run with BOTH axes
off installs no hook and leaves the ring empty; use `bioscall_dump` for LLE-side
vector observation there.

- `{"cmd":"hle_dump"}` — status: `backend` (`HLE (LLE fallback)` /
  `LLE (recompiled BIOS)`), `boot_skip`, `boot_turbo_active`, `total`.
- `{"cmd":"hle_dump","tail":N}` — last N entries: `seq`, `cycle` (guest
  cycle), `vec` (0xA0/0xB0/0xC0, or 0x30000 for the boot skip), `fn` ($t1
  function number), `a0..a3`, `ra`, `v0` (result when HLE-serviced), `route`
  (0 = fell through to LLE, 1 = serviced in HLE, 2 = boot shell-skip).
- Filters: `"fn":N`, `"route":0|1|2`.

---

## Rule when the server can't answer your question

If an inspection need isn't covered by the existing commands, **do not fall back to printf or log files**. Instead:

1. Add a handler in `runtime/src/debug_server.c` (native)
2. Add the matching handler in `runtime/src/beetle_debug_server.c` (Beetle oracle)
   when the question needs a cross-check against hardware behaviour
3. Keep field names parallel between the two
4. Run `python tools/gen_tcp_commands.py` to refresh the generated index, and add
   a row to the curated inventory above if the command needs explaining

> Step 4 used to read "Update this file", and that did not survive contact with
> reality: this document described 47 commands while the servers registered 292.
> The index is now generated from the command tables, and
> `python tools/gen_tcp_commands.py --check` fails when it drifts. Prose in the
> curated inventory is still hand-written and still worth adding.

The TCP server is the canonical instrumentation surface. Rule 3 in `CLAUDE.md` is absolute: **no `fprintf(stderr, …)` in source code, ever, for any reason**.

<!-- BEGIN AUTOGENERATED COMMAND INDEX -- edit tools/gen_tcp_commands.py, not this block -->

## Complete command index (generated)

**318 commands registered** — 305 on the native server (`runtime/src/debug_server.c`), 61 on the Beetle server (`runtime/src/beetle_debug_server.c`).

51 of 318 have prose above; **267 are index-only**. An index-only command still works — it just has no description here yet. Send it `{"cmd":"<name>"}` and read the reply, or find its `handle_*` function in the server source.

Regenerate with `python tools/gen_tcp_commands.py`; `--check` fails if this block has drifted from the code.

| Command | Native | Beetle | Described above |
|---|:--:|:--:|:--:|
| `a0_history` | ✓ |  |  |
| `audio_events` | ✓ | ✓ |  |
| `audio_stats` | ✓ | ✓ |  |
| `audio_wav` | ✓ | ✓ |  |
| `autocompile_run` | ✓ |  |  |
| `autocompile_status` | ✓ |  |  |
| `bios_info` | ✓ |  | ✓ |
| `bioscall_dump` | ✓ |  |  |
| `c0_history` | ✓ |  |  |
| `call_focus_dump` | ✓ |  |  |
| `call_focus_reset` | ✓ |  |  |
| `call_focus_stats` | ✓ |  |  |
| `callret_watch` | ✓ |  | ✓ |
| `capture_freeze` | ✓ |  |  |
| `capture_quads` | ✓ |  |  |
| `card_buffer_dump` | ✓ |  |  |
| `card_data_writes` | ✓ |  |  |
| `card_data_writes_reset` | ✓ |  |  |
| `card_handoff` | ✓ |  |  |
| `card_mgr_clear` | ✓ |  |  |
| `card_mgr_trace` | ✓ |  |  |
| `card_read_summary` | ✓ |  |  |
| `card_read_summary_reset` | ✓ |  |  |
| `card_trace_dump` | ✓ |  |  |
| `card_txn_dump` | ✓ |  |  |
| `cd_overwrite` | ✓ |  |  |
| `cd_read_log` | ✓ |  |  |
| `cdc_volume` |  | ✓ |  |
| `cdrom_bursts` | ✓ |  |  |
| `cdrom_cmd_dump` |  | ✓ |  |
| `cdrom_cmd_reset` |  | ✓ |  |
| `cdrom_command_history` | ✓ |  |  |
| `cdrom_command_history_clear` | ✓ |  |  |
| `cdrom_instant_rate` | ✓ |  |  |
| `cdrom_sector_dump` | ✓ |  | ✓ |
| `cdrom_sector_history` | ✓ |  | ✓ |
| `cdrom_sector_history_clear` | ✓ |  | ✓ |
| `cdrom_state` | ✓ |  |  |
| `cdrom_timing` | ✓ |  |  |
| `cdrom_trace_clear` | ✓ |  |  |
| `cdrom_trace_dump` | ✓ |  |  |
| `ce_profile` | ✓ |  |  |
| `chain_trace` | ✓ |  |  |
| `clear_input` | ✓ | ✓ | ✓ |
| `continue` | ✓ |  | ✓ |
| `cyc_watch` | ✓ | ✓ |  |
| `cyc_watch_clear` | ✓ | ✓ |  |
| `cyc_watch_dump` | ✓ | ✓ |  |
| `cycles_to_next_event` | ✓ |  |  |
| `d44_ring` | ✓ |  |  |
| `data_shards` | ✓ |  |  |
| `devtrace_ctl` | ✓ | ✓ |  |
| `devtrace_dump` | ✓ | ✓ |  |
| `dirty_block_dump_file` | ✓ |  |  |
| `dirty_block_log` | ✓ |  |  |
| `dirty_break_clear` | ✓ |  |  |
| `dirty_break_range` | ✓ |  |  |
| `dirty_break_state` | ✓ |  |  |
| `dirty_flow_log` | ✓ |  |  |
| `dirty_insn_dump_file` | ✓ |  |  |
| `dirty_insn_gate` | ✓ |  |  |
| `dirty_insn_log` | ✓ |  |  |
| `dirty_ram_stats` | ✓ |  |  |
| `dirty_ram_unsupported` | ✓ |  |  |
| `disp_ring` | ✓ |  |  |
| `dispatch_check` | ✓ |  |  |
| `dispatch_stats` | ✓ |  |  |
| `dispatch_tail` | ✓ |  |  |
| `display_ring_aux` | ✓ |  |  |
| `display_ring_get` | ✓ |  |  |
| `display_ring_stats` | ✓ |  |  |
| `dma_cdrom_history` | ✓ |  |  |
| `dma_state` | ✓ |  | ✓ |
| `dma_trace_clear` | ✓ |  |  |
| `dma_trace_dump` | ✓ |  |  |
| `dual_input` | ✓ |  |  |
| `dual_state` | ✓ |  |  |
| `dump_buffer` | ✓ |  |  |
| `dump_ram` | ✓ | ✓ | ✓ |
| `evcb_snapshot` | ✓ |  |  |
| `evcb_walk_dump` | ✓ |  |  |
| `evcb_walk_stats` | ✓ |  |  |
| `event_ring_clear` | ✓ |  |  |
| `event_ring_dump` | ✓ |  |  |
| `event_ring_tail` | ✓ |  |  |
| `exc_ring` |  | ✓ |  |
| `first_failure` | ✓ | ✓ | ✓ |
| `fmv_state` | ✓ |  |  |
| `fn_clear` | ✓ |  |  |
| `fn_disable` | ✓ |  |  |
| `fn_entry_dump` | ✓ |  |  |
| `fn_entry_tail` | ✓ |  | ✓ |
| `fn_exit_dump` | ✓ |  |  |
| `fn_filter` | ✓ |  |  |
| `fn_stats` | ✓ |  |  |
| `fntrace_arm` | ✓ | ✓ |  |
| `fntrace_arm_clear` | ✓ |  |  |
| `fntrace_armed` | ✓ |  |  |
| `fntrace_arms` |  | ✓ |  |
| `fntrace_clear` | ✓ |  |  |
| `fntrace_disarm` |  | ✓ |  |
| `fntrace_dump` | ✓ | ✓ |  |
| `fntrace_reset` |  | ✓ |  |
| `fntrace_unfiltered` |  | ✓ |  |
| `frame` | ✓ |  | ✓ |
| `frame_fingerprint` | ✓ |  |  |
| `frame_perf` | ✓ |  |  |
| `frame_range` | ✓ | ✓ | ✓ |
| `frame_timeseries` | ✓ | ✓ | ✓ |
| `freeze_check` | ✓ |  |  |
| `game_options` | ✓ |  |  |
| `geom_correction` | ✓ |  | ✓ |
| `get_frame` | ✓ | ✓ | ✓ |
| `get_quads` | ✓ |  |  |
| `get_registers` | ✓ | ✓ | ✓ |
| `get_snapshots` | ✓ | ✓ | ✓ |
| `gl_coh_ring` | ✓ |  |  |
| `gl_fbo_peek` | ✓ |  |  |
| `gl_interp` | ✓ |  |  |
| `gl_present_ring` | ✓ |  |  |
| `gl_vram_diff` | ✓ |  |  |
| `gl_wide_fast` | ✓ |  |  |
| `gl_ws_ablate` | ✓ |  |  |
| `gp1_dump` | ✓ |  | ✓ |
| `gpu_frame_dump` | ✓ |  | ✓ |
| `gpu_opcodes` | ✓ |  |  |
| `gpu_ring_stats` | ✓ |  |  |
| `gpu_state` | ✓ |  | ✓ |
| `gte_frame_stats` | ✓ |  |  |
| `gte_intpl_dump` | ✓ |  |  |
| `gte_latch_dump` | ✓ |  |  |
| `gte_ring_dump` | ✓ |  |  |
| `gte_state` | ✓ |  |  |
| `history` | ✓ | ✓ | ✓ |
| `hle_dump` | ✓ |  | ✓ |
| `idle_skip` | ✓ |  |  |
| `imask_trace` | ✓ |  |  |
| `input_route_append` | ✓ |  |  |
| `input_route_clear` | ✓ |  |  |
| `input_route_start` | ✓ |  |  |
| `input_route_status` | ✓ |  |  |
| `input_route_stop` | ✓ |  |  |
| `insn_freeze` | ✓ |  |  |
| `insn_freeze_snapshot` | ✓ |  |  |
| `insn_freeze_status` | ✓ |  |  |
| `insn_freeze_target` | ✓ |  |  |
| `irq_state` | ✓ |  | ✓ |
| `irqctx_ring` | ✓ |  |  |
| `kernel_bless` | ✓ |  |  |
| `latency` | ✓ |  |  |
| `load_transitions` | ✓ |  |  |
| `lockstep` | ✓ |  |  |
| `lockstep_func` | ✓ |  |  |
| `mc_status` | ✓ |  |  |
| `mdec_state` | ✓ |  |  |
| `mdec_trace` | ✓ |  |  |
| `mdec_trace_clear` | ✓ |  |  |
| `mem_words` | ✓ |  |  |
| `mmio_clear` | ✓ |  | ✓ |
| `mmio_dump` | ✓ |  | ✓ |
| `mmx6_freshfix` | ✓ |  |  |
| `overclock` | ✓ |  |  |
| `overlay_candidates` | ✓ |  |  |
| `overlay_capture_dump` | ✓ |  |  |
| `overlay_cps_probe` | ✓ |  |  |
| `overlay_diff_off` | ✓ |  |  |
| `overlay_diff_on` | ✓ |  |  |
| `overlay_dump` | ✓ |  |  |
| `overlay_fp_dump` | ✓ |  |  |
| `overlay_irq_ratelimit` | ✓ |  |  |
| `overlay_irq_suppress_off` | ✓ |  |  |
| `overlay_irq_suppress_on` | ✓ |  |  |
| `overlay_loader_status` | ✓ |  |  |
| `overlay_native_block` | ✓ |  |  |
| `overlay_native_event_granularity` | ✓ |  |  |
| `overlay_native_off` | ✓ |  |  |
| `overlay_native_on` | ✓ |  |  |
| `overlay_native_ring` | ✓ |  |  |
| `overlay_rescan` | ✓ |  |  |
| `overlay_shadow_detail` | ✓ |  |  |
| `overlay_shadow_dump` | ✓ |  |  |
| `pace_state` | ✓ |  |  |
| `pad_status` | ✓ | ✓ |  |
| `parity_ctl` | ✓ | ✓ |  |
| `parity_dump` | ✓ | ✓ |  |
| `pause` | ✓ |  | ✓ |
| `pc_probe_arm` | ✓ |  |  |
| `pc_probe_clear` | ✓ |  |  |
| `pc_probe_dump` | ✓ |  |  |
| `pgxp` | ✓ |  |  |
| `pgxp_census` | ✓ |  |  |
| `pgxp_depth` | ✓ |  |  |
| `pgxp_texcensus` | ✓ |  |  |
| `phase_hot` | ✓ |  |  |
| `phase_profile` | ✓ |  |  |
| `ping` | ✓ | ✓ | ✓ |
| `present_ring` | ✓ |  |  |
| `present_shot` | ✓ |  | ✓ |
| `present_shot_seq` | ✓ |  | ✓ |
| `press` | ✓ | ✓ |  |
| `probe_clear` | ✓ |  |  |
| `probe_trace` | ✓ |  |  |
| `quit` | ✓ |  | ✓ |
| `ra_load_watch` | ✓ |  |  |
| `read_frame_ram` | ✓ | ✓ | ✓ |
| `read_ram` | ✓ | ✓ | ✓ |
| `record_frame` | ✓ |  |  |
| `record_frame_dump` | ✓ |  |  |
| `record_reads_dump` | ✓ |  |  |
| `restore_trace` | ✓ |  |  |
| `restore_trace_clear` | ✓ |  |  |
| `restore_trace_window` | ✓ |  |  |
| `rtrace_arm` | ✓ | ✓ |  |
| `rtrace_clear` | ✓ |  |  |
| `rtrace_disarm` |  | ✓ |  |
| `rtrace_disarm_all` |  | ✓ |  |
| `rtrace_dump` | ✓ | ✓ |  |
| `rtrace_ranges` | ✓ | ✓ |  |
| `rtrace_reset` |  | ✓ |  |
| `rtrace_stats` | ✓ | ✓ |  |
| `run_to_frame` | ✓ |  | ✓ |
| `s3_smear_watch` | ✓ |  | ✓ |
| `savestate` | ✓ |  |  |
| `screenshot` | ✓ | ✓ | ✓ |
| `screenshot_file` | ✓ | ✓ | ✓ |
| `screenshot_hires` | ✓ |  | ✓ |
| `set_input` | ✓ | ✓ | ✓ |
| `set_snapshot` | ✓ | ✓ | ✓ |
| `sio1_state` | ✓ |  |  |
| `sio_arm_audit` | ✓ |  |  |
| `sio_burst_stats` | ✓ |  |  |
| `sio_ctrl_reg_clear` | ✓ |  |  |
| `sio_ctrl_reg_trace` | ✓ |  |  |
| `sio_ctrl_reg_window` | ✓ |  |  |
| `sio_irq_dump` | ✓ |  |  |
| `sio_irq_window` | ✓ |  |  |
| `sio_pc_trace` | ✓ |  |  |
| `sio_pc_window` | ✓ |  |  |
| `sio_state` | ✓ |  | ✓ |
| `sio_trace` | ✓ | ✓ |  |
| `sio_trace_reset` |  | ✓ |  |
| `sio_trace_window` | ✓ |  |  |
| `sio_write_window` |  | ✓ |  |
| `sp_ring` | ✓ |  |  |
| `spu_events` | ✓ | ✓ |  |
| `spu_events_reset` | ✓ |  |  |
| `spu_ram` | ✓ |  |  |
| `spu_status` | ✓ |  |  |
| `spu_voices` | ✓ | ✓ |  |
| `sreg_trace_clear` | ✓ |  |  |
| `sreg_trace_dump` | ✓ |  |  |
| `sreg_trace_find` | ✓ |  |  |
| `sreg_trace_stats` | ✓ |  |  |
| `stack_profile` | ✓ |  |  |
| `starv_ring` | ✓ |  |  |
| `step` | ✓ |  | ✓ |
| `synth_recurse` | ✓ |  |  |
| `tex_pack` | ✓ |  |  |
| `thread_ctx_ring` | ✓ |  |  |
| `thread_trace` | ✓ |  |  |
| `thread_trace_clear` | ✓ |  |  |
| `timers_state` | ✓ |  |  |
| `turbo` | ✓ |  | ✓ |
| `turbo_audio_sink` | ✓ |  |  |
| `turbo_loads` | ✓ |  |  |
| `turbo_state` | ✓ |  | ✓ |
| `unknown_dispatch_log` | ✓ |  |  |
| `unwatch` | ✓ |  | ✓ |
| `vblank_rate` | ✓ |  |  |
| `vk_perf` | ✓ |  |  |
| `vram_peek` | ✓ | ✓ | ✓ |
| `vsync_query_hle` | ✓ |  |  |
| `warm_cd_route` | ✓ |  |  |
| `watch` | ✓ |  | ✓ |
| `wide_full` | ✓ |  |  |
| `wide_shot` | ✓ |  |  |
| `write_ram` | ✓ |  | ✓ |
| `ws_aspect` | ✓ |  |  |
| `ws_aspect_cone_site` | ✓ |  |  |
| `ws_backdrop_margin` | ✓ |  |  |
| `ws_backdrop_ring` | ✓ |  |  |
| `ws_backdrop_stretch` | ✓ |  |  |
| `ws_census` | ✓ |  |  |
| `ws_dbg_stretch` | ✓ |  |  |
| `ws_dome` | ✓ |  |  |
| `ws_dome_probe` | ✓ |  |  |
| `ws_far_threshold` | ✓ |  |  |
| `ws_hud_mode` | ✓ |  |  |
| `ws_margin` | ✓ |  |  |
| `ws_menu_edge_fill` | ✓ |  |  |
| `ws_nw` | ✓ |  |  |
| `ws_ui_groups` | ✓ |  |  |
| `wtrace_add` | ✓ |  |  |
| `wtrace_all_dump` | ✓ | ✓ |  |
| `wtrace_all_reset` | ✓ | ✓ |  |
| `wtrace_all_stats` | ✓ | ✓ |  |
| `wtrace_arm` | ✓ | ✓ |  |
| `wtrace_boot_dump` | ✓ |  |  |
| `wtrace_boot_reset` | ✓ |  |  |
| `wtrace_boot_stats` | ✓ |  |  |
| `wtrace_boot_summary` | ✓ |  |  |
| `wtrace_clear` | ✓ |  | ✓ |
| `wtrace_del` | ✓ |  |  |
| `wtrace_disarm` | ✓ | ✓ |  |
| `wtrace_disarm_all` | ✓ | ✓ |  |
| `wtrace_dump` | ✓ | ✓ | ✓ |
| `wtrace_range` | ✓ |  | ✓ |
| `wtrace_ranges` | ✓ | ✓ |  |
| `wtrace_reset` | ✓ | ✓ |  |
| `wtrace_stats` | ✓ | ✓ |  |
| `wtrace_trans_dump` | ✓ |  |  |
| `wtrace_trans_reset` | ✓ |  |  |
| `wtrace_trans_stats` | ✓ |  |  |
| `xlate` | ✓ |  |  |
| `xprobe` | ✓ |  |  |
| `xprobe_arm` | ✓ |  |  |
| `xprobe_watch` | ✓ |  |  |
| `zbuf` | ✓ |  |  |

<!-- END AUTOGENERATED COMMAND INDEX -->
