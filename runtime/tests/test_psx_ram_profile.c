#include "psx_ram.h"
#include "psx_ram_profile.h"

#ifndef EXPECTED_MAIN_RAM_BYTES
#error "EXPECTED_MAIN_RAM_BYTES must be defined by the test target"
#endif

int main(void) {
    const int expect_8mb = EXPECTED_MAIN_RAM_BYTES == PSX_RAM_8MB;
    PSXRamSizeRequest request = PSX_RAM_SIZE_REQUEST_INITIALIZER;

    if (PSX_MAIN_RAM_BYTES != EXPECTED_MAIN_RAM_BYTES)
        return 1;
    if (psx_ram_title_requires_8mb() != expect_8mb)
        return 2;
    if (psx_ram_resolve_8mb_request(0) != expect_8mb)
        return 3;
    if (!psx_ram_resolve_8mb_request(1))
        return 4;
    if (psx_ram_size_request_bytes(&request) != EXPECTED_MAIN_RAM_BYTES)
        return 5;

    psx_ram_size_request_set_mod(&request, 1);
    if (psx_ram_size_request_bytes(&request) != PSX_RAM_8MB)
        return 6;
    psx_ram_size_request_reset(&request);
    if (psx_ram_size_request_bytes(&request) != EXPECTED_MAIN_RAM_BYTES)
        return 7;
    psx_ram_size_request_set_mod(&request, 0);
    if (psx_ram_size_request_bytes(&request) != EXPECTED_MAIN_RAM_BYTES)
        return 8;
    return 0;
}
