#ifndef PSX_NETPLAY_INPUT_HIST_H
#define PSX_NETPLAY_INPUT_HIST_H

/*
 * MotK input history: portable ring/invent in retcomm-rbengine; PSX pad
 * conversion stays in netplay_input_hist.c.
 */

#if defined(PSX_HAS_RECOMP_NET)

#include <stdint.h>

#include "psx_netplay.h"
#include "retcomm_rbengine/input_hist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_INPUT_HIST_DEPTH     RBE_INPUT_HIST_DEPTH
#define NETPLAY_INPUT_HIST_MAX_SLOTS RBE_INPUT_HIST_MAX_SLOTS
typedef RbeInputHist NetplayInputHist;

#define netplay_ih_reset             rbe_ih_reset
#define netplay_ih_frame_to_contract rbe_ih_frame_to_contract
#define netplay_ih_put               rbe_ih_put
#define netplay_ih_get               rbe_ih_get
#define netplay_ih_invent_hold_last  rbe_ih_invent_hold_last
#define netplay_ih_invent_idle       rbe_ih_invent_idle
#define netplay_ih_promote           rbe_ih_promote

/* PsxNetPad ↔ RNetRbFrame (LX/LY only; RX/RY stay on the pad blob path). */
void netplay_ih_pad_to_frame(const PsxNetPad *pad, uint32_t tick, uint8_t predicted,
                             RNetRbFrame *out);
void netplay_ih_frame_to_pad(const RNetRbFrame *frame, PsxNetPad *pad);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HAS_RECOMP_NET */

#endif /* PSX_NETPLAY_INPUT_HIST_H */
