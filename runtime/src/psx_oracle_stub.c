/*
 * psx_oracle_stub.c -- Stub oracle backend for builds without DuckStation.
 *
 * When ENABLE_DUCKSTATION_ORACLE is NOT defined, provide the g_psx_oracle
 * global and convenience wrappers as no-ops so the code compiles cleanly.
 */

#if !defined(ENABLE_DUCKSTATION_ORACLE) && !defined(ENABLE_BEETLE_PSX_ORACLE)

#include "psx_oracle_backend.h"

const psx_oracle_backend_t *g_psx_oracle = 0;

int psx_oracle_init(const char *bios_path) {
    (void)bios_path;
    return -1; /* no backend available */
}

void psx_oracle_shutdown(void) {}

void psx_oracle_run_frame(uint16_t pad1_buttons) {
    (void)pad1_buttons;
}

#endif /* !ENABLE_DUCKSTATION_ORACLE && !ENABLE_BEETLE_PSX_ORACLE */
