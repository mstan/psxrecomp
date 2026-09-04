#include "ws_prepass_guard.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint32_t packet[] = {
        0x64010203u, 0x00100020u, 0x00040008u, 0x00100010u
    };
    WsPrepassPacketGuard guard = ws_prepass_packet_guard(packet, 4u);

    assert(ws_prepass_packet_matches(&guard, packet, 4u));

    /* A guest rewrite after the DMA kick must reject the cached geometry. */
    packet[3] = 0x00200010u;
    assert(!ws_prepass_packet_matches(&guard, packet, 4u));

    /* A command-length change is stale even when the shared prefix matches. */
    packet[3] = 0x00100010u;
    assert(!ws_prepass_packet_matches(&guard, packet, 3u));

    puts("ws_prepass_guard_test: PASS");
    return 0;
}
