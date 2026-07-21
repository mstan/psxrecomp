/* Backward compatibility for game dispatchers generated before
 * psx_game_text_native_ok() was added to the emitter.
 *
 * New dispatchers provide a strong definition that validates every emitted
 * CFG range.  Older dispatchers fall back to the pre-existing text-image
 * check so dirty or byte-diverged game text is still never run natively.
 */

#include <stdint.h>

#include "dirty_ram_interp.h"

#if defined(PSX_HAS_GAME_DISPATCH) && \
    !defined(PSX_GAME_DISPATCH_HAS_NATIVE_OK)

extern int psx_game_address_in_text(uint32_t addr);

int psx_game_text_native_ok(uint32_t addr)
{
    const uint32_t phys = addr & 0x1FFFFFFFu;
    return psx_game_address_in_text(addr) && dirty_ram_text_native_ok(phys);
}

#elif !defined(PSX_HAS_GAME_DISPATCH)

/* The BIOS-only runtime still links shared dispatch observability. There is no
 * game image to classify, so both predicates are necessarily false. */
int psx_game_address_in_text(uint32_t addr)
{
    (void)addr;
    return 0;
}

int psx_game_text_native_ok(uint32_t addr)
{
    (void)addr;
    return 0;
}

#endif /* PSX_HAS_GAME_DISPATCH */
