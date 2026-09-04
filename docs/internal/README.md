# Internal dev-logs & design notes

These are the working design documents and burn-down logs the project was built
from. They are **historical/working notes**, not contributor onboarding docs —
they capture decisions, phased plans, and deep subsystem investigations in the
order they happened, and some sections are superseded by later ones.

If you're new here, start with the top-level [`README.md`](../../README.md),
then [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md),
[`docs/EXECUTION_MODEL.md`](../EXECUTION_MODEL.md),
[`docs/BUILDING.md`](../BUILDING.md), and
[`CONTRIBUTING.md`](../../CONTRIBUTING.md). Come here when you need the *why*
behind a specific subsystem.

## Plans & roadmap
- [`PLAN.md`](PLAN.md) — the original phased build plan (P1–P6).
- [`FAITHFUL_TIMING_PLAN.md`](FAITHFUL_TIMING_PLAN.md) — the authoritative
  cycle-accuracy north-star plan and status log.
- [`FIRST_MILESTONE.md`](FIRST_MILESTONE.md) — the first-boot milestone gate.
- [`STUBS_TO_FIX.md`](STUBS_TO_FIX.md) — hardware-stub burn-down (pre-game gate).

## Cycle accuracy & timing
- [`PRECISE_IRQ_SLICE.md`](PRECISE_IRQ_SLICE.md) — exact-instruction IRQ delivery.
- [`CYCLE_MODEL_BEETLE.md`](CYCLE_MODEL_BEETLE.md) — the Beetle-derived cycle model.
- [`ACCURACY_BURNDOWN.md`](ACCURACY_BURNDOWN.md) — full accuracy burn-down across
  all axes (semantics, cycle, IRQ, MMIO, peripherals, determinism).
- [`COSIM_ORACLE.md`](COSIM_ORACLE.md) — the first-divergence co-sim oracle.

## BIOS / scheduler
- [`HLE_SCHEDULER_CARVEOUT_PLAN.md`](HLE_SCHEDULER_CARVEOUT_PLAN.md) — the
  cooperative-thread scheduler HLE carve-out.
- [`WEDGE_load4_shell_rootcause.md`](WEDGE_load4_shell_rootcause.md) — a specific
  boot-shell wedge root-cause writeup.

## Renderer / widescreen
- [`GL_RENDERER_HANDOFF.md`](GL_RENDERER_HANDOFF.md) — OpenGL renderer design.
- [`NATIVE_WIDE_PLAN.md`](NATIVE_WIDE_PLAN.md) — native-wide 16:9 (GTE FOV) plan.
- [`BACKDROP_PRELOAD.md`](BACKDROP_PRELOAD.md) — widescreen backdrop preload.

## Project conventions & tracking
- [`PRINCIPLES.md`](PRINCIPLES.md) — this repo's debugging and enhancement
  principles (first-divergence, verified-enhancement HLE). Source comments cite
  it as `docs/internal/PRINCIPLES.md`; a "recomp-template PRINCIPLES.md"
  citation means a *different* repo's file.
- [`ISSUES.md`](ISSUES.md) — the historical framework issue log. Live framework
  work is tracked in Beads (epic `beads-eio.3`), not here.
- [`DEBUG.md`](DEBUG.md) — the debugging playbook behind the TCP debug server;
  the command reference itself is [`../TCP_COMMANDS.md`](../TCP_COMMANDS.md).
- [`audit_inventory.md`](audit_inventory.md) — what the codegen/GTE audit
  pipeline covers and why.

## Overlay design & discovery
- [`overlay-recompilation-design.md`](overlay-recompilation-design.md) — the
  design and rationale for overlay recompilation.
- [`overlay-plan.md`](overlay-plan.md) — the phased overlay implementation plan.
- [`overlay-discovery.md`](overlay-discovery.md) — how overlays are discovered
  on disc and at runtime.
- [`overlay-status.md`](overlay-status.md) — living what-works / what's-broken
  status for the overlay capture/compile/cache path.
- [`CASE_A_AOT_GAP.md`](CASE_A_AOT_GAP.md) — the Case-A AOT dispatch-miss gap;
  cited by the `rationale` fields in `recompiler/seeds/*.json`.

## BIOS / boot hardening
- [`bios-hardening.md`](bios-hardening.md) — BIOS boot hardening notes.

## GTE / CPU investigations
- [`GTE_LM_FIX.md`](GTE_LM_FIX.md) — the GTE `lm` saturation-flag fix writeup.
- [`TOMBA2_EXC_CONSULT_RESPONSE.md`](TOMBA2_EXC_CONSULT_RESPONSE.md) — exception
  -path consultation response for the Tomba 2 bring-up.

## Upstream contributor reviews
`upstream/` holds per-PR review notes for community contributions, one file per
change, named `<contributor>-<pr>-<topic>.md`.
- [`upstream/UPSTREAM_2026-08-20.md`](upstream/UPSTREAM_2026-08-20.md) — the
  2026-08-20 upstreaming sweep: which contributed changes are framework-general
  and which are title configuration.

## Overlay cache internals
- `SLJIT_PERSIST_CACHE.md` — **removed.** It described the persisted shard cache
  of the sljit Tier-2 backend, which was deleted in `5b7e69b4` (2026-07-15).
  For the current model see `runtime/src/overlay_loader.c` (per-entry candidate
  validity), `tools/compile_overlays.py` (shard formation and the `.ranges`
  manifest), and [`../COMPILING_OVERLAYS.md`](../COMPILING_OVERLAYS.md).

## Removed
- `CYCLE_TIMING_ARCH.md` — **removed.** Its cycle-timing architecture sketch was
  superseded and nothing linked to it. For the current model see
  [`FAITHFUL_TIMING_PLAN.md`](FAITHFUL_TIMING_PLAN.md) (the authoritative plan),
  [`CYCLE_MODEL_BEETLE.md`](CYCLE_MODEL_BEETLE.md) (the Beetle-derived cycle
  model), and [`PRECISE_IRQ_SLICE.md`](PRECISE_IRQ_SLICE.md) (exact-instruction
  IRQ delivery).
