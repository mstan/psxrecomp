#ifndef PGXP_HOOK_EMITTER_H
#define PGXP_HOOK_EMITTER_H

#include <cstdint>
#include <string>

namespace PSXRecomp {

/* PGXP dataflow-shadowing hook emission (docs/ENHANCEMENTS.md G1.10; runtime
 * pgxp_hooks.h). Shared by BOTH emitters — CodeGenerator (game) and
 * StrictTranslator (BIOS) — so the hook grammar can never drift between them.
 *
 * Wraps the translated statement `code` with a PGXP_*() macro invocation for
 * the instruction classes that move sub-pixel projection provenance. Source
 * operands the statement may clobber are captured into locals BEFORE it;
 * everything else is read after it. In the base build the macros preprocess
 * to ((void)0) and the optimizer erases the dead captures, so base objects
 * are unchanged; only a -DPSX_PGXP=1 TU pays.
 *
 * NOT applied to LWC2/SWC2 — those emissions capture the raw loaded/stored
 * word themselves (a masked GTE register write must validate against the
 * word as loaded, not the register's masked value).
 *
 * Deliberately unhooked: AND/XOR/NOR/SLT-family and the exotic immediates —
 * they only ever DESTROY precision, and the engine's validate-on-read drops
 * their stale shadows without help. */
/* True when `code`'s LAST line is a preprocessor directive.
 *
 * A directive owns its whole line: the preprocessor swallows anything after it
 * as "extra tokens" and silently DISCARDS it (gcc -Wendif-labels). Several
 * emissions end on a bare `#endif` -- the PSX_ENABLE_BLOCK_CYCLES muldiv
 * stall/latency blocks in both emitters (code_generator.cpp translate_mult/
 * multu/div/divu and translate_mfhi/mflo; strict_translator.cpp the same
 * shapes) -- so ANY text appended to such an emission on the same line is
 * dropped without a build failure.
 *
 * That is exactly the defect PR #171 fixed for the PGXP hooks, where 3,187
 * hooks per title were being discarded. This predicate exists so the remaining
 * same-line appenders can be guarded by construction instead of one at a time.
 *
 * Callers append "
" + indent instead of a space when this returns true. */
bool emission_ends_on_preprocessor_directive(const std::string& code);

void append_pgxp_hooks(uint32_t instr, std::string& code);

} // namespace PSXRecomp

#endif /* PGXP_HOOK_EMITTER_H */
