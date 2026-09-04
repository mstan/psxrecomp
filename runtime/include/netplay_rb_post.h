#ifndef PSX_NETPLAY_RB_POST_H
#define PSX_NETPLAY_RB_POST_H

/*
 * MotK compatibility shim → retcomm-rbengine rb_post tip filter.
 */

#include "retcomm_rbengine/rb_post.h"

#ifdef __cplusplus
extern "C" {
#endif

#define netplay_rb_peer_post_tip_ok rbe_rb_peer_post_tip_ok

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_RB_POST_H */
