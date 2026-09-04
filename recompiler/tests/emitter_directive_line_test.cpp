/*
 * A preprocessor directive owns its whole line. Anything appended after it on
 * that line is swallowed as "extra tokens" and silently DISCARDED -- no build
 * failure, just a -Wendif-labels warning nobody reads. PR #171 was exactly
 * this: `append_pgxp_hooks` space-joined its PGXP_*() call onto emissions that
 * end on a bare `#endif` (the PSX_ENABLE_BLOCK_CYCLES muldiv stall/latency
 * blocks), so 3,187 hooks per title were thrown away.
 *
 * This pins the predicate that now guards the remaining same-line appenders,
 * so the hazard cannot come back by construction.
 *
 * Build/run: ctest -R emitter_directive_line_test
 */

#include "pgxp_hook_emitter.h"

#include <cstdio>
#include <string>

static int failures;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
        else         { std::printf("ok:   %s\n", (msg)); }                      \
    } while (0)

using PSXRecomp::emission_ends_on_preprocessor_directive;

int main() {
    /* The shape that caused PR #171: translate_mult's real emission. */
    const std::string muldiv =
        "{ int64_t result = (int64_t)(int32_t)cpu->gpr[4] * (int64_t)(int32_t)cpu->gpr[5];"
        " cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }"
        "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_set(cpu, 13u);\n#endif";
    CHECK(emission_ends_on_preprocessor_directive(muldiv),
          "a muldiv emission ending on a bare #endif is detected");

    /* translate_mfhi with rd==$zero: the stall block with no statement. */
    CHECK(emission_ends_on_preprocessor_directive(
              "/* nop: write to $zero */"
              "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_muldiv_stall(cpu);\n#endif"),
          "a zero-reg mfhi/mflo emission ending on #endif is detected");

    /* Leading whitespace is legal before a directive. */
    CHECK(emission_ends_on_preprocessor_directive("foo();\n    #endif"),
          "an indented directive still owns its line");
    CHECK(emission_ends_on_preprocessor_directive("foo();\n\t#else"),
          "a tab-indented directive still owns its line");
    CHECK(emission_ends_on_preprocessor_directive("#endif"),
          "a single-line emission that is only a directive is detected");

    /* Ordinary statements must NOT be flagged -- a false positive here would
     * inject stray newlines into every instruction's output. */
    CHECK(!emission_ends_on_preprocessor_directive("cpu->gpr[4] = cpu->hi;"),
          "a plain statement is not a directive line");
    CHECK(!emission_ends_on_preprocessor_directive(
              "\n#ifdef PSX_ENABLE_BLOCK_CYCLES\n    psx_gte_stall(cpu);\n#endif\n    "),
          "an emission whose directive block is followed by a fresh indented "
          "line is safe to append to");
    CHECK(!emission_ends_on_preprocessor_directive(
              "{ uint32_t _pgxa = cpu->gpr[4]; foo();\n    PGXP_LOAD(0u, _pgxa, x); }"),
          "an already-fixed hook emission is not flagged");
    CHECK(!emission_ends_on_preprocessor_directive(""),
          "an empty emission is not flagged");
    CHECK(!emission_ends_on_preprocessor_directive("x = 1; /* #endif in a comment */"),
          "a '#' that is not the line's first token is not a directive");

    std::printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
