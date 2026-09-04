# Recomp GPT consult response — Tomba 2 EPC de-overload (2026-06-22)

Verbatim design response from the Recomp GPT (thread "Recomp - Tomba 2 Debugging
Analysis"). Converges on **Option A** as a scoped **dual-mode** exception system, with
safety rails. Implementation reference for the fix.

## Core: hard resume-mode split

```
compiled/native interrupted context:
  EPC = sentinel
  resume = host longjmp + host GPR restore        (UNCHANGED — T1/MMX6/Ape)

dirty-interpreter interrupted context:
  EPC = real guest PC
  resume = architectural cpu->pc = EPC
  no sentinel dependency, no host GPR restore
```

An explicit `PsxExceptionResumeMode { NATIVE_SENTINEL, DIRTY_REAL_EPC }` on an
exception frame. At delivery: if the interrupt source is a dirty-interp safe point,
`mode=DIRTY_REAL_EPC`, `EPC = cpu->pc` (the committed next guest PC), do NOT make EPC a
host token (GPR snapshot optional, diagnostics only). Else `mode=NATIVE_SENTINEL`,
`EPC=0x80000048`, save GPRs for host restore (as today).

`ReturnFromException` becomes mode-driven:
- DIRTY_REAL_EPC: `in_exception=false; cpu->pc = cop0_epc; restore dispatch_depth; pop
  frame; return` to dispatcher/dirty re-entry (NO host longjmp, NO host GPR restore —
  trust the guest handler's TCB save/restore).
- NATIVE_SENTINEL: restore GPRs from host snapshot; restore dispatch_depth; pop frame;
  `psx_exception_longjmp()` (as today).
- No active frame + EPC is a real guest PC: `cpu->pc = cop0_epc; in_exception=false`
  (async/game-driven RFE — the Tomba 2 case).
- No active frame + EPC == sentinel: **INVALID** — latch `RFE_SENTINEL_ESCAPED` /
  fatal. Do NOT silently NOP the sentinel (that's the current bug's escape hatch).

## Q1 — host GPR save/restore on the dirty path?
For the **RFE exit**: NO. Trust the guest/BIOS handler's architectural TCB
save+restore. Host-restoring GPRs after that risks undoing legitimate handler work or
masking a broken restore. Keep the host snapshot as a diagnostic latch only. A handler
returning *normally* (non-RFE) in dirty-real-EPC mode should be a diagnostic failure /
guarded legacy fallback, not the expected path.

## Q2 — drop the in_exception gate?
NOT globally. Scope the sentinel longjmp strictly to
`EPC==sentinel && mode==NATIVE_SENTINEL && frame.active`. Allow async/game-driven RFE
when EPC is a real PC even if `in_exception==0` (resume via `cpu->pc=EPC`).

## Q3 — dispatch-depth / nested dirty-pump hazard
Real-EPC is safer than sentinel here, but enforce entry preconditions for dirty
delivery: cpu->pc committed to the precise next guest insn; no half-executed insn; not
between branch and delay slot unless BD/EPC modeled; dirty local state written back to
cpu; no active dirty local-flow frame expecting a host C return; dispatch_depth saved.
**Block nested dirty-pump entry:**
```c
if (cpu->in_exception || cpu->exception_depth > 0) {
    cpu->pending_interrupt_after_exception = true;
    return;
}
```
Keep `g_psx_dispatch_depth` save/restore in BOTH modes.

## Q4 — Option A vs B
Option A (lower risk). Option B (force the game handler inside our window) keeps EPC
overloaded/fragile and still relies on host timing/lifetime semantics; Tomba 2 is
showing a valid PSX pattern (HookEntryInt handler that calls ReturnFromException
itself). A fixes the abstraction: dirty interp HAS a real guest PC → EPC should be real
→ RFE can be architectural. Scoped to dirty safe points; compiled-native unchanged.

## Per-title failure modes to watch
- **Tomba 1:** if a T1 interrupt is delivered from dirty interp and previously relied on
  host GPR restore, real-EPC could expose a BIOS handler context-restore bug. Expect T1
  to stay mostly native-sentinel, or dirty-real-EPC resumes with real PCs and no
  sentinel async RFE.
- **MMX6:** tighter interrupt timing / delay-slot-adjacent poll points → real EPC could
  be off by one control-flow unit. Mitigation: only allow DIRTY_REAL_EPC at poll
  boundaries NOT in delay-slot state, OR correctly set CAUSE.BD and EPC=branch_pc when
  interrupted in a delay slot.
- **Ape Escape:** nested interrupt depth / dispatch-depth imbalance. Assert
  exception_depth never exceeds 1 unless explicitly supported; assert dispatch_depth
  before==after RFE; assert no sentinel EPC reaches the guest TCB on the dirty path.

## Confirmation probe (paired ledger, always-on)
`EXC_ENTRY{frame,cycle,mode,source(native/dirty),cpu_pc_before,epc_written,ra,sp,
dispatch_depth,in_exception_before}` → `TCB_SAVE{saved_epc,saved_ra,saved_sp}` →
`RFE_CALL{caller_pc,t1,in_exception,epc,ra,sp,mode,dispatch_depth}` →
`RFE_EXIT{action=real_pc_resume|sentinel_longjmp|invalid_sentinel, pc_after,ra_after,
sp_after,dispatch_depth_after}`.
Success for Tomba 2: EXC_ENTRY mode=DIRTY_REAL_EPC, epc_written=real pc; TCB_SAVE
saved_epc=same real pc; RFE_CALL in_exception may be 0, epc=real pc; RFE_EXIT
action=real_pc_resume; no dispatch pc=0 exit.
Hard tripwire: `if (EPC==0x80000048 && !active_native_sentinel_frame) latch
RFE_SENTINEL_ESCAPED`.

## Bottom line
Make the sentinel **less** permissive and explicitly host-only. The bug is a host token
escaping into guest-visible EPC/TCB state; the fix prevents that escape anywhere a real
guest PC exists.
