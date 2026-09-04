/* nd_intro_ot.h — CTR Naughty Dog intro OT helpers (debug / accuracy shims). */
#ifndef PSXRECOMP_ND_INTRO_OT_H
#define PSXRECOMP_ND_INTRO_OT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Last WoodEmit batch OT base (model+228). Prefer digit-rain tags. */
void     psx_nd_note_wood_batch_ot(uint32_t base);
void     psx_nd_note_wood_batch_ot_tagged(uint32_t base, uint32_t tag);
uint32_t psx_nd_wood_batch_ot(void);

/* Sibling-scene bump hint at s4+0xb4 (captured at NdIntroSiblingRtptEmit). */
void     psx_nd_note_sibling_ot_hint(uint32_t base);
uint32_t psx_nd_sibling_ot_hint(void);

#ifdef __cplusplus
}
#endif

#endif
