/* func_override.h — replace a guest function with hand-written C.
 *
 * WHY THIS EXISTS
 *
 * A recomp turns the game into C, but that C is a build artifact: regenerated
 * from the player's own disc, never hand-edited (CLAUDE.md rule 4). So there
 * is no supported way to say "this function should do something else" — which
 * is the single most important thing a port needs to grow beyond a faithful
 * replay of the original.
 *
 * This is that mechanism. Register hand-written C against a guest address and
 * the dispatcher calls yours instead of the recompiled original.
 *
 * NO SIZE LIMIT. This is not a ROM patch; there is no slot to fit inside, no
 * code cave, nothing to shift. Your replacement is native code in a native
 * binary — five instructions or five thousand lines. That freedom is the
 * entire point of recompiling rather than hacking bytes.
 *
 * WHERE IT HOOKS, AND WHY IT MATTERS
 *
 * The hook is consulted in psx_dispatch_impl after the BIOS tiers (so an
 * override can never shadow a kernel service vector) and BEFORE every game
 * code backend. That placement is deliberate and load-bearing: one
 * address-keyed hook covers the STATIC EXE, runtime-loaded OVERLAYS and
 * DIRTY RAM alike. The interpreter's JAL/JALR call-resolution tiers consult
 * it on the SAME terms — before enter-compiled, before overlay-native and
 * before the local pc-chain (the reserved CRES_OVERRIDE slot) — so no call
 * the interpreter resolves itself can bypass an override. Ordering there is
 * not cosmetic: interp_enter_compiled calls psx_dispatch_game_compiled ->
 * entry->fn(cpu) directly and never re-enters psx_dispatch_impl, so a hook
 * placed after it is unreachable for any override on a statically-compiled
 * function reached from interpreted code, while registration and the
 * func_override inventory both still look healthy.
 *
 * SCOPE: overrides are CALL-site keyed. A guest tail transfer (j / jr) into
 * an overridden address from the dirty-RAM block loop is a TRANSFER, not a
 * call, and does not consult the hook — see dirty_ram_interp.c's compiled
 * handoff sites. Registering on a function only ever entered by tail
 * transfer will show calls == 0.
 *
 * THE CONTRACT
 *
 * Your function receives the CPUState and must obey the guest ABI:
 *
 *     int my_impl(CPUState *cpu) {
 *         uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5];   // arguments
 *         ...
 *         cpu->gpr[2] = result;                           // $v0 = return value
 *         return 1;
 *     }
 *
 * Return 1 and the guest resumes at $ra exactly as if the original had run
 * `jr $ra`. Return 0 and the original recompiled or interpreted code runs
 * untouched, so an override can decline case by case (a conditional
 * pre-hook: mutate state, return 0, original runs).
 *
 * POINTERS MUST BE GUEST ADDRESSES. The caller is recompiled too: when it
 * dereferences your return value it performs a guest load. A host pointer
 * will read garbage. If you need to hand back a buffer, write it into guest
 * RAM (psx_write_byte and friends, or psx_mod_alloc_guest_memory) and return
 * that address.
 *
 * DETERMINISM. Keep ALL mutable state in guest RAM. Rollback/rewind and
 * netplay snapshot guest state only; host-side statics in an override
 * desync replays. Reading host config that never changes mid-session is
 * fine.
 *
 * CYCLE ACCOUNTING — REQUIRED AT REGISTRATION.
 * On hardware the replaced function took guest time, and every device in
 * the machine schedules against that clock; a replacement that takes zero
 * cycles shifts IRQ phase for the rest of the session. So every
 * registration MUST state its timing intent via the `credit` argument —
 * there is no default to inherit. Two forms:
 *
 *   credit >= 0        The tier charges exactly `credit` guest cycles
 *                      (psx_advance_cycles) on every HANDLED call, after
 *                      the body runs. Declines charge nothing — the
 *                      original runs and accrues its own cycles. 0 is
 *                      legal and means "deliberately free": right for a
 *                      player-toggled mod whose behavior has no hardware
 *                      analog, indefensible for an always-on
 *                      reimplementation claiming to be indistinguishable.
 *
 *   FO_CREDIT_SELF     The tier charges nothing; the BODY owns its timing
 *                      and calls psx_advance_cycles() itself. Use this
 *                      when the original's cost is data-dependent
 *                      (charge base + per_iteration * n, the bios_hle.c
 *                      TIMING NOTE pattern), and ALWAYS for wraps: the
 *                      original re-dispatched by func_override_call_original
 *                      accrues its exact cycles by executing, so a fixed
 *                      nonzero credit on a wrap double-counts. Guest code
 *                      invoked via func_override_guest_call likewise
 *                      self-charges — credit only the code you REPLACED.
 *
 * Credits are approximations of the original's dynamic instruction count
 * unless measured; LLE remains the timing oracle (run with the override
 * declined and sample the cycle delta across the call boundary — the
 * callret ring — to measure a real distribution). See FAITHFUL_TIMING_PLAN.
 *
 * WHAT IT DOES NOT DO
 *
 * It does not lift the game's own assumptions. If the caller copies your
 * result into a fixed-size field, that field is still fixed. The override
 * removes the code limit, not every limit.
 */

#ifndef PSXRECOMP_FUNC_OVERRIDE_H
#define PSXRECOMP_FUNC_OVERRIDE_H

#include <stddef.h>
#include <stdint.h>

struct CPUState;

#ifdef __cplusplus
extern "C" {
#endif

#define FO_MAX_OVERRIDES   128
#define FO_MAX_ID          64
#define FO_MAX_GUARD_WORDS 4

/* `credit` value: the override body states its own timing by calling
 * psx_advance_cycles() itself (mandatory for wraps — see CYCLE ACCOUNTING
 * above). Any other negative value is FO_ERR_ARGS. */
#define FO_CREDIT_SELF   (-1)

/* func_override_add return codes. */
#define FO_OK            0
#define FO_ERR_FULL      1
#define FO_ERR_ARGS      2
#define FO_ERR_DUPLICATE 3

/* An override implementation. Reads arguments from cpu->gpr[4..7], writes
 * the return value to cpu->gpr[2]. Returning nonzero means "handled";
 * returning 0 declines this call and the original code runs instead. */
typedef int (*FuncOverrideFn)(struct CPUState *cpu);

/* Register `fn` for guest address `addr` (KSEG or physical — normalised).
 * `id` is copied, used for diagnostics, and may be NULL. Registering the
 * same address twice is an error rather than a silent last-wins, because a
 * silent shadow is impossible to debug. `credit` is the guest-cycle policy
 * (CYCLE ACCOUNTING above): >= 0 charged per handled call, or
 * FO_CREDIT_SELF when the body charges its own. */
int func_override_add(const char *id, uint32_t addr, FuncOverrideFn fn,
                      int32_t credit);

/* Like func_override_add, plus a residency guard: before each consult the
 * first `n_words` guest words at `addr` are compared against
 * `expected_words`; on mismatch the override silently declines (the guard
 * miss is counted, see func_override_get_ex). Use this when the address
 * belongs to overlay/dirty RAM that other code may occupy at other times —
 * the guard makes "wrong code resident" a decline instead of a corruption.
 * n_words must be 1..FO_MAX_GUARD_WORDS. */
int func_override_add_guarded(const char *id, uint32_t addr, FuncOverrideFn fn,
                              const uint32_t *expected_words, int n_words,
                              int32_t credit);

/* Call a guest function from inside an override (or any native code on the
 * dispatch thread): args already placed in cpu->gpr[4..7], returns after the
 * callee completes; result in cpu->gpr[2]. `site_ra` is the return address
 * the callee should observe in $ra — pass the original call site's return
 * address when re-creating a call the original code would have made, so
 * callees, fntrace and crash forensics see authentic values. $ra is
 * restored around the call. */
void func_override_guest_call(struct CPUState *cpu, uint32_t target,
                              uint32_t site_ra);

/* Run the ORIGINAL code of the function currently being overridden — the
 * wrap primitive. Callable only from inside an executing override; arms a
 * one-shot bypass for this address, re-dispatches it, and returns after the
 * original completes (result in cpu->gpr[2]). Typical post-hook:
 *
 *     int wrap(CPUState *cpu) {
 *         func_override_call_original(cpu);   // original runs fully
 *         ...inspect / adjust results...
 *         return 1;                            // we handled the call
 *     }
 *
 * The bypass is one-shot per invocation: if the original recursively calls
 * itself, the recursive calls consult the override again (matching what a
 * guest-level wrap would observe). */
void func_override_call_original(struct CPUState *cpu);

/* Register on behalf of a mod package plan. Identical to the two calls above
 * (pass n_words 0 for unguarded) except the entry is tagged package-armed, so
 * func_override_reset_package_armed can drop it when the plan is cleared.
 * The mod runtime uses this; game code registering its own always-on
 * reimplementations should use func_override_add/_add_guarded. */
int func_override_add_package(const char *id, uint32_t addr, FuncOverrideFn fn,
                              const uint32_t *expected_words, int n_words,
                              int32_t credit);

/* Install the dispatcher hook. Call once at startup AFTER registering (the
 * mod runtime calls it again after arming package-gated overrides — safe).
 * Cheap with nothing registered — it leaves the hook NULL, so dispatch is
 * byte-identical to a build without the tier. */
void func_override_install(void);

/* Drop every package-armed override, keep direct registrations, re-install
 * (so the hook goes back to NULL if the table empties). Returns how many were
 * dropped. THIS IS REQUIRED FOR CORRECTNESS, not a convenience: a cleared mod
 * plan stops activation/vblank callbacks simply by no longer being iterated,
 * but an armed override lives in this module's own table, so without an
 * explicit reset it keeps firing after the plan is gone — including after
 * netplay's clear-mods announces a vanilla session, which is a peer
 * divergence. Direct registrations are deliberately kept: they are the game's
 * own always-on faithful reimplementations, present identically in both
 * peers' builds and not player-selectable. */
int func_override_reset_package_armed(void);

/* Introspection, surfaced by the `func_override` TCP command. An override
 * whose `calls` stays 0 was never reached: wrong address, or that code path
 * never ran. `guard_misses` counts consults declined by the residency
 * guard. `credit_out` reports the declared cycle policy (FO_CREDIT_SELF or
 * the fixed per-call charge), so every armed override's timing intent is
 * inspectable, not just documented. */
int  func_override_count(void);
int  func_override_get(int index, char *id_out, size_t id_cap,
                       uint32_t *addr_out, uint64_t *calls_out);
int  func_override_get_ex(int index, char *id_out, size_t id_cap,
                          uint32_t *addr_out, uint64_t *calls_out,
                          uint64_t *guard_misses_out, int *guarded_out,
                          int32_t *credit_out);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_FUNC_OVERRIDE_H */
