/* Browser backend for the optional Tier-2 overlay JIT.
 *
 * WebAssembly cannot emit and execute host machine-code shards or dlopen a
 * cached native library. Reporting the producer as unavailable routes dynamic
 * PSX RAM code through the existing interpreter correctness floor; no guest
 * behavior is synthesized here.
 */

#include "overlay_sljit.h"

#include <stdlib.h>
#include <string.h>

int overlay_sljit_available(void) { return 0; }
int overlay_sljit_selftest(void) { return 0; }

void overlay_sljit_try_compile(uint32_t entry,
                               const uint8_t *bytes, uint32_t size,
                               uint32_t image_base_vram,
                               OverlaySljitResult *out) {
    (void)entry;
    (void)bytes;
    (void)size;
    (void)image_base_vram;
    if (out) memset(out, 0, sizeof(*out));
}

void overlay_sljit_get_status(int *available, int *selftest_ok,
                              uint64_t *compiles, uint64_t *declines,
                              uint64_t *bytes_emitted) {
    if (available) *available = 0;
    if (selftest_ok) *selftest_ok = 0;
    if (compiles) *compiles = 0;
    if (declines) *declines = 0;
    if (bytes_emitted) *bytes_emitted = 0;
}

void overlay_sljit_init_helpers(CPUState *cpu) { (void)cpu; }

OverlaySljitFn overlay_sljit_deserialize(const void *blob,
                                         unsigned long blob_size) {
    (void)blob;
    (void)blob_size;
    return NULL;
}

void overlay_sljit_free_serialized(void *p) { free(p); }
