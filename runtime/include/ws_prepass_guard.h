#ifndef PSXRECOMP_WS_PREPASS_GUARD_H
#define PSXRECOMP_WS_PREPASS_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t word_count;
    uint64_t fingerprint;
} WsPrepassPacketGuard;

static inline uint64_t ws_prepass_packet_fingerprint(const uint32_t *words,
                                                     uint32_t word_count) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t i = 0; i < word_count; i++) {
        uint32_t word = words[i];
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (uint8_t)(word >> shift);
            hash *= UINT64_C(1099511628211);
        }
    }
    hash ^= word_count;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static inline WsPrepassPacketGuard ws_prepass_packet_guard(
        const uint32_t *words, uint32_t word_count) {
    WsPrepassPacketGuard guard;
    guard.word_count = word_count;
    guard.fingerprint = ws_prepass_packet_fingerprint(words, word_count);
    return guard;
}

static inline int ws_prepass_packet_matches(const WsPrepassPacketGuard *guard,
                                            const uint32_t *words,
                                            uint32_t word_count) {
    return guard && guard->word_count == word_count &&
           guard->fingerprint ==
               ws_prepass_packet_fingerprint(words, word_count);
}

#ifdef __cplusplus
}
#endif

#endif
