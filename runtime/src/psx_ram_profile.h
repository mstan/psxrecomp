#ifndef PSX_RAM_PROFILE_H
#define PSX_RAM_PROFILE_H

#include "psx_ram.h"

typedef struct PSXRamSizeRequest {
    int expanded;
} PSXRamSizeRequest;

#define PSX_RAM_SIZE_REQUEST_INITIALIZER \
    { PSX_MAIN_RAM_BYTES == PSX_RAM_8MB }

static inline void psx_ram_size_request_reset(PSXRamSizeRequest *request) {
    request->expanded = psx_ram_title_requires_8mb();
}

static inline void psx_ram_size_request_set_mod(PSXRamSizeRequest *request,
                                                 int enabled) {
    request->expanded = psx_ram_resolve_8mb_request(enabled);
}

static inline uint32_t psx_ram_size_request_bytes(
    const PSXRamSizeRequest *request) {
    return request->expanded ? PSX_RAM_8MB : PSX_RAM_2MB;
}

#endif /* PSX_RAM_PROFILE_H */
