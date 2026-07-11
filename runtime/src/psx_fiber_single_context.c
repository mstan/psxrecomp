/* Web backend for the legacy host-fiber bridge.
 *
 * The shipping web target requires the deterministic HLE TCB scheduler, which
 * does not suspend native C stacks. Emscripten has no ucontext implementation;
 * keep a real current-context identity for exception ownership and fail loudly
 * if a configuration attempts to enter the unsupported legacy fiber tier.
 */

#include "psx_fiber.h"

#include <stdlib.h>

static int s_main_context;

psx_fiber_t psx_fiber_convert_thread(void) { return &s_main_context; }
psx_fiber_t psx_fiber_current(void) { return &s_main_context; }

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry,
                             void *arg) {
    (void)stack_size;
    (void)entry;
    (void)arg;
    return NULL;
}

void psx_fiber_switch(psx_fiber_t target) {
    if (target && target != &s_main_context) abort();
}

void psx_fiber_destroy(psx_fiber_t fiber) { (void)fiber; }
