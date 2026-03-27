/*
 * interp_stub.c — Stubs for interpreter-only builds
 *
 * When INTERPRETER_ONLY is defined, the generated *_full.c and *_dispatch.c
 * files are excluded from the build. This file provides the symbols that
 * would normally come from those files.
 */
#ifdef INTERPRETER_ONLY

#include "psx_runtime.h"

/* Stub dispatch — no compiled functions in interpreter-only mode */
int psx_dispatch_compiled(CPUState* cpu, uint32_t addr)
{
    (void)cpu;
    (void)addr;
    return 0;
}

#endif /* INTERPRETER_ONLY */
