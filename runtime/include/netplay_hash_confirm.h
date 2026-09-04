#ifndef PSX_NETPLAY_HASH_CONFIRM_H
#define PSX_NETPLAY_HASH_CONFIRM_H

/*
 * MotK compatibility shim → retcomm-rbengine hash_confirm.
 * Prefer rbe_hc_* in new code.
 */

#include "retcomm_rbengine/hash_confirm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_HC_RING RBE_HC_RING
typedef RbeHashConfirm NetplayHashConfirm;

#define netplay_hc_reset            rbe_hc_reset
#define netplay_hc_prime_after      rbe_hc_prime_after
#define netplay_hc_note_local       rbe_hc_note_local
#define netplay_hc_note_peer        rbe_hc_note_peer
#define netplay_hc_resolved_through rbe_hc_resolved_through
#define netplay_hc_confirm_through  rbe_hc_confirm_through
#define netplay_hc_local_digest     rbe_hc_local_digest
#define netplay_hc_peer_digest      rbe_hc_peer_digest
#define netplay_hc_peek_mismatch    rbe_hc_peek_mismatch
#define netplay_hc_heal_stale_gap   rbe_hc_heal_stale_gap

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_HASH_CONFIRM_H */
