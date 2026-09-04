/* psx_bss.h — force large zero-init objects into BSS (not stored in the PE).
 *
 * MinGW + LTO has been observed to place multi‑MiB uninitialized globals into
 * .rdata as literal zeros, bloating release .exe size by ~100+ MiB (e.g.
 * g_fntrace_ring). Explicit .bss placement keeps them RAM-only on disk.
 *
 * Mach-O requires "segment,section" (not ELF ".bss"); Apple Clang rejects
 * section(".bss") at compile time.
 */
#ifndef PSXRECOMP_PSX_BSS_H
#define PSXRECOMP_PSX_BSS_H

#if defined(__APPLE__)
#define PSX_BSS __attribute__((section("__DATA,__bss")))
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_BSS __attribute__((section(".bss")))
#elif defined(_MSC_VER)
/* MSVC already puts uninitialized globals in .bss by default. */
#define PSX_BSS
#else
#define PSX_BSS
#endif

#endif /* PSXRECOMP_PSX_BSS_H */
