/* psx_align.h — portable alignment attribute for SIMD scratch buffers.
 *
 * The SSE2/NEON kernels store to local scratch arrays with aligned stores
 * (_mm_store_si128 and friends), so the alignment is load-bearing, not a
 * hint: an under-aligned destination faults.
 *
 * GCC/Clang spell this __attribute__((aligned(n))); MSVC spells it
 * __declspec(align(n)). Both are valid in prefix position on a declaration,
 * which is the one placement the two spellings share -- hence the macro is
 * written to be used as a prefix:
 *
 *     PSX_ALIGN(16) int32_t tmp[4];
 */
#ifndef PSXRECOMP_PSX_ALIGN_H
#define PSXRECOMP_PSX_ALIGN_H

#if defined(_MSC_VER) && !defined(__clang__)
#define PSX_ALIGN(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_ALIGN(n) __attribute__((aligned(n)))
#else
#define PSX_ALIGN(n) _Alignas(n)
#endif

#endif /* PSXRECOMP_PSX_ALIGN_H */
