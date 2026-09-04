#if !defined(PSX_HAS_RECOMP_NET)
/* Empty TU when recomp-net is not linked. */
#else

#include "netplay_input_hist.h"

#include <string.h>

/* PSX pad ↔ RNetRbFrame only. Ring/invent/promote live in retcomm-rbengine. */

static int8_t u8_to_i8_stick(uint8_t v)
{
    int d = (int)v - 0x80;
    if (d > 127) d = 127;
    if (d < -128) d = -128;
    return (int8_t)d;
}

static uint8_t i8_to_u8_stick(int8_t v)
{
    return (uint8_t)((int)v + 0x80);
}

void netplay_ih_pad_to_frame(const PsxNetPad *pad, uint32_t tick, uint8_t predicted,
                             RNetRbFrame *out)
{
    PsxNetPad n;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->is_predicted = predicted ? 1u : 0u;
    out->is_valid = 1u;
    out->analog = 0u; /* MotK default digital; tip/capture may override */
    if (!pad) {
        out->buttons = 0xFFFFu;
        return;
    }
    n = *pad;
    psx_netplay_normalize_pad(&n);
    out->buttons = n.buttons;
    out->stick_x = u8_to_i8_stick(n.lx);
    out->stick_y = u8_to_i8_stick(n.ly);
    out->analog = n.analog ? 1u : 0u;
}

void netplay_ih_frame_to_pad(const RNetRbFrame *frame, PsxNetPad *pad)
{
    if (!pad) return;
    memset(pad, 0, sizeof(*pad));
    pad->buttons = 0xFFFFu;
    pad->lx = pad->ly = pad->rx = pad->ry = 0x80u;
    pad->analog = 0; /* digital until a valid frame says DualShock */
    pad->connected = 1;
    if (!frame || !frame->is_valid) return;
    pad->buttons = frame->buttons;
    pad->lx = i8_to_u8_stick(frame->stick_x);
    pad->ly = i8_to_u8_stick(frame->stick_y);
    pad->analog = frame->analog ? 1u : 0u;
    psx_netplay_normalize_pad(pad);
}

#endif /* PSX_HAS_RECOMP_NET */
