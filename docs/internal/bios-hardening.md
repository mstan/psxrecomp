# BIOS Hardening

**Date:** 2026-05-30
**Branch:** `bios-hardening`
**Depends on:** `overlay-discovery` (merged to master)

---

## What shipped in overlay-discovery

- **Static A0/B0/C0 dispatch** — BIOS vector trampolines pre-encoded from ROM tables;
  zero dirty_ram_interp at runtime for all syscall vectors.
- **0xBFC15F1C seed** — PAD/SIO interrupt handler compiled to static C; ~50K dirty_ram
  blocks eliminated. One seed, CFG discovers all internal branch targets.
- **SIO 0x0CF0 alias** — trampoline at 0x0CF0 (lui+addiu+jalr to 0x641C) registered as
  a direct dispatch alias; 30K dirty_ram blocks eliminated.
- **Disc speed 4x** — post-BIOS switch via game entry_pc, XA streaming guard for FMVs.
- **75% dirty_ram reduction** (119K → 29K blocks/session with Tomba save load).

---

## Outstanding BIOS-side work

### 1. Disc speed "instant" hang — root cause unknown

`disc_speed = "instant"` (divisor=0, 1-cycle floor) hangs the game at the PlayStation
logo (post-Sony-logo BIOS screen, before game starts). `disc_speed = "4x"` is stable.

**What we know:**
- Sony logo passes cleanly (entry_pc trigger fires correctly post-BIOS-boot).
- Hang occurs during early game initialization, meaning the game's disc reads are involved.
- 500-cycle floor tried, still hangs. 4x (divisor=4, ~112K cycle floor) is stable.

**Likely cause:** game's early disc read loop has a tight IRQ poll with a retry counter
that expires before the interrupt handler completes at sub-4x speeds.

**Next step:** binary search the minimum safe divisor. Try divisor=8 (2x faster than
working 4x). If stable, try 16, 32, etc. until hang reproduced, then back off one step.
The minimum safe value tells us the minimum inter-IRQ gap the game requires.

**Longer-term fix:** instrument the CD-ROM IRQ delivery path to log inter-IRQ intervals
at runtime and compare against the game's retry counter depth.

---

### 2. Dispatch hit/miss counters

Discussed but not implemented. Currently no way to measure static dispatch hit rate
(binary-search hit) vs dirty_ram fallback without adding counters.

**Implementation:** emit `g_dispatch_static_hits` (uint64_t) increment at the binary
search hit in `emit_dispatch()`, expose via existing `dispatch_stats` TCP command
(alongside `dispatch_miss_total`). Two lines in the generated dispatch + one TCP handler
extension.

**Value:** lets us verify static coverage quantitatively after any seed change. Should
show near-100% static hits for BIOS functions after our work.

---

### 3. Exception chain continuations (0x0DF8, 0x0E08)

Measured: 7711 hits × 2 insns each = 15K interpreted instructions per session. Low
priority but completable.

**What they are:** mid-function continuation points in the BIOS exception chain walker.
The walker calls chained handlers via `jalr s1`; these addresses are the continuations
after each call returns. They show up in dirty_ram because they're in the dirty kernel
copy pages and the recompiler never sees them as function starts.

**Fix:** find the containing function (the exception chain walker itself) and verify it's
already seeded. If it is, these continuations should already be compiled as internal
labels within that function. If not, seed the function start. Do not seed these
continuation addresses directly (they're mid-function and will crash the recompiler).

---

### 4. Trampoline resolution logging gap

`psx_unknown_dispatch` in `traps.c` has 5 trampoline patterns (J, JR, lui+addiu+jr,
lui+ori+jr, A0/B0/C0 vector dispatch). When a pattern matches, it resolves silently and
returns — **no counter, no ring buffer entry**.

This means we have no visibility into how many dispatches go through trampoline resolution
vs the ring buffer vs dirty_ram. Add a `g_trampoline_resolutions` counter and expose it
on the `dispatch_stats` TCP command alongside `dispatch_miss_total`.

---

## #3 — Overlay discovery (B+A) — next after Tomba seeding

Once Tomba seeding is complete, the remaining dirty_ram traffic will be genuinely
overlay code (addresses > 0x98000, loaded from disc). That's when B+A slots in.

See `overlay-discovery.md` for the full architecture. Short version:

- **Layer B** (ship first): on CD DMA completion, log `(hash, load_addr, size)` to
  `logs/<SCUS_ID>/overlay_map.jsonl`. At next build, recompile those overlays
  statically. No runtime linker needed.
- **Layer A** (after B has real coverage): on DMA completion, check a per-game cache
  of compiled DLLs. Cache hit → `LoadLibrary` + register. Cache miss → dirty-RAM
  fallback + background compile → cache populated for next encounter.

B+A starts after Tomba seeding because: (a) seeding eliminates the non-overlay
dirty_ram noise so you know exactly what's left, and (b) Layer B's first real overlay
log needs clean data to hash against.

---

## Tomba game dispatch misses (game-side, seeding)

Captured 16 dirty_ram dispatch misses in the Tomba game text range (0x10000-0x98000).
Saved to `TombaRecomp/seeds/dirty_ram_misses.txt`. The 10 high-hit addresses (~38K
hits/session each) are the priority.

**Before seeding:** verify each address in Ghidra (SCUS_942.36_no_header) is a true
function start (look for stack frame prologue or confirmed jal/jalr call site). Do NOT
seed mid-function addresses — this caused a boot hang in this session.

**High-hit addresses to verify:**
```
0x8004DF00   38837 hits, 194185 insns
0x8004DF7C   38837 hits, 310696 insns
0x8002D4C8   38837 hits, 194185 insns
0x8002D4E0   38837 hits, 233022 insns
0x8002D524   38837 hits, 116511 insns
0x8004DF68   38837 hits, 53860 insns
0x8002D504   38837 hits, 38837 insns
0x8004DF28   38837 hits, 13465 insns
0x8004DF30   38837 hits, 13465 insns
0x8002D4FC   38837 hits, 13465 insns
```

All 10 have identical hit counts (38837) suggesting they are all called together —
likely internal branch targets of 1-2 functions. Seed only the entry points;
CFG will discover the rest.

**Tools:** `tools/collect_game_misses.py` accumulates new addresses each session.
Run after every gameplay session to grow the log before seeding.
