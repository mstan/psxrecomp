# Draft RFC: FMV callback overhead without guest-state changes

Status: design review only. No IRQ batching implementation or runtime flag is
added. Tracking: `beads-eio.3.150`. The contributor's actual prototype patch,
instrumentation patch, PE capture bytes and raw measurements are not available.

## What is established

WO-10 correctly identifies repeated generated block callbacks. With precise
slicing disabled, the runtime slice body returns early, but the DLL still calls
through its callback shim. That observation does not quantify the host cost.
WO-11's own trials did not demonstrate a repeatable batching speedup.

The reported missing live CFG predecessors were independently reproduced.
The live address-order loop heuristic also labels acyclic backward jumps as
loops. These are separately addressed in [PR #354](https://github.com/RetroPortingToolKit/psxrecomp/pull/354).
Even corrected static metadata does not enumerate every indirect, alias or CPS
resume edge; it cannot prove that a block is runtime-private.

## A concrete merge blocker

The supplied design suppresses `psx_icache_fetch` along with IRQ checks.
That is not simply reducing host calls. The added native test executes the real
cache model with a header, a cold body on another line, and the header again:

| State | Preserve body fetch | Omit body fetch |
| --- | ---: | ---: |
| Charged fetch cycles | 14 | 7 |
| Pending selected load give-back | 0 | 99 |
| Body cache tag | `0x80010020` | invalid (`1`) |

The header executes in both cases. The effect matches the in-tree Beetle
`PS_CPU::ReadInstruction` miss charging and `ReadAbsorb` clearing. This is a
counterexample to the described operation, not an execution of the missing
prototype and not certification of every detail of the current cache model.

## Required invariants for any future patch

1. Every executed instruction retains its guest cycle charge, including delay
   slots and cache/memory/multiply/GTE effects. Cache tags and load give-back
   evolve identically; removing a host callback must not remove its semantics.
2. Devices and interrupts remain visible at required guest boundaries. One loop
   iteration or four blocks is not a cycle/deadline bound. Account for pending
   IRQs, COP0, nested/irreducible loops, memory effects and long blocks.
3. Every supported CPS/alias/indirect entry remains valid, including entries
   not represented by static predecessor edges. A single predecessor is not a
   privacy proof and static unreachability is not permission to delete code.
4. Unsupported analysis fails closed to the existing faithful path. No per-game
   exclusions, generated-C edits or default-on speculative behavior.
5. Behavior-changing flags participate in emitted-artifact/cache identity.
   Prove cold/warm and off/on/off transitions cannot reuse the wrong DLL.

Natural-loop classification requires graph structure, not address order; see
[LLVM's loop definition](https://llvm.org/docs/LoopTerminology.html#loop-definition).

## Supplemental evidence requested

- Exact base revision, complete prototype patch, diagnostic schema patch and
  the exact enabled flags/build commands. Do not reconstruct the patch from
  prose and call it the contributor's tested implementation.
- For PE-specific attribution: capture JSON with bytes, `game.toml`, seeds,
  matching DLL/manifest identities and bounded raw timing records. Those assets
  are unavailable locally and must not be invented.
- Inclusive versus exclusive callback timing; treatment of host preemption,
  reentrancy and CPS continuations. Unserialized RDTSC/global counters need
  explicit validation before reporting cycles per guest iteration.
- Repeatable content-aligned trials with every target invocation represented,
  not only frames where it was the maximum dispatch. Include per-frame tail
  latency, audio underruns and guest-state/event-order comparison.

## Acceptance before implementation is considered mergeable

- Synthetic cold/conflicting/uncached fetch, load give-back, pending IRQ,
  deadline crossing, COP0, delay-slot, alias/CPS, nested-loop and irreducible-CFG
  tests; explicit first-divergence comparison where state/timing can change.
- Unmodified default-path output comparison and cache-key transition tests.
- Regenerated Tomba/MMX6/Ape LLE boot, FMV, menus and gameplay regression with
  screenshots and progress/state evidence; BIOS/device timing calibrated against
  the independent Beetle oracle. A visual smoke run alone is insufficient.
- A demonstrated benefit, measured separately from PGO/LTO/debug configuration.

## Other unresolved brief items

WO-8's PE decoder is a profiling lead, not proof that the guest algorithm alone
causes the stalls. Any predecode cache or worker-thread proposal first needs
verified input identity, memory ownership, dependencies and ordering.

WO-9's mismatched DLL/manifest rejection is correct. If a longer rejection
history is wanted, propose a bounded ring exposed over TCP, preserving pair
validation and atomic publication. Do not add runtime stderr/file logging.

Current `pgo-train` already finishes the successful profile-use/debug-OFF build.
Its failure report and exact source revision are needed to investigate the
claimed early stop. Runtime PGO/LTO do not automatically cover independently
compiled overlay DLLs. Those build experiments are separate from this RFC.

Nothing in this draft should be merged as an IRQ performance fix.
