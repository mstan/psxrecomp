/*
 * test_netplay_input_hist.c — pad map, invent hold-last, promote.
 *
 * Build/run: ctest -R netplay_input_hist_test
 * (needs the lib/recomp-net submodule; registration is conditional.)
 */
#define PSX_HAS_RECOMP_NET 1
#include "netplay_input_hist.h"
#include "recomp_net/input_contract.h"

#include <stdio.h>
#include <string.h>

/* Standalone: provide normalize used by pad↔frame (mirrors psx_netplay). */
void psx_netplay_normalize_pad(PsxNetPad *pad)
{
    const int dead = 24;
    if (!pad) return;
    if (pad->lx > (uint8_t)(0x80 - dead) && pad->lx < (uint8_t)(0x80 + dead))
        pad->lx = 0x80;
    if (pad->ly > (uint8_t)(0x80 - dead) && pad->ly < (uint8_t)(0x80 + dead))
        pad->ly = 0x80;
    if (pad->rx > (uint8_t)(0x80 - dead) && pad->rx < (uint8_t)(0x80 + dead))
        pad->rx = 0x80;
    if (pad->ry > (uint8_t)(0x80 - dead) && pad->ry < (uint8_t)(0x80 + dead))
        pad->ry = 0x80;
}

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

static uint8_t hc_yes(void *ctx) { (void)ctx; return 1; }
static uint8_t hc_no(void *ctx) { (void)ctx; return 0; }

int main(void)
{
    NetplayInputHist h;
    RNetRbFrame f, got;
    PsxNetPad pad, pad2;
    RNetInputContractFrame pub, wire;
    RNetInputContractParams params;
    RNetInputContractHostGates gates;
    RNetInputContractDecision d;

    netplay_ih_reset(&h, 2);
    CHECK(h.slot_count == 2, "slot_count");

    memset(&pad, 0, sizeof(pad));
    pad.buttons = 0xFDFFu; /* cross pressed (active-low) */
    pad.lx = 0x80 + 40;
    pad.ly = 0x80;
    pad.rx = pad.ry = 0x80;
    pad.analog = 1;
    pad.connected = 1;

    netplay_ih_pad_to_frame(&pad, 10, 0, &f);
    CHECK(f.tick == 10u, "frame tick");
    CHECK(f.buttons == 0xFDFFu, "buttons");
    CHECK(f.stick_x == 40, "stick_x");
    CHECK(f.stick_y == 0, "stick_y");
    CHECK(f.analog == 1u, "analog flag from pad");
    CHECK(!f.is_predicted && f.is_valid, "local auth flags");

    netplay_ih_frame_to_pad(&f, &pad2);
    CHECK(pad2.buttons == 0xFDFFu, "roundtrip buttons");
    CHECK(pad2.lx == (uint8_t)(0x80 + 40), "roundtrip lx");
    CHECK(pad2.analog == 1u, "roundtrip analog");

    /* Digital MotK path must not become DualShock through hist. */
    {
        RNetRbFrame dig;
        pad.analog = 0;
        pad.lx = pad.ly = 0x80;
        netplay_ih_pad_to_frame(&pad, 9, 0, &dig);
        CHECK(dig.analog == 0u, "digital frame");
        netplay_ih_frame_to_pad(&dig, &pad2);
        CHECK(pad2.analog == 0u, "digital roundtrip");
    }

    CHECK(netplay_ih_put(&h, 0, &f), "put local");
    CHECK(netplay_ih_get(&h, 0, 10, &got), "get local");
    CHECK(got.stick_x == 40, "get stick");
    CHECK(got.analog == 1u, "get analog");

    /* Invent remote at 11 with no history → neutral digital. */
    CHECK(netplay_ih_invent_hold_last(&h, 1, 11, &got), "invent neutral");
    CHECK(got.is_predicted && got.buttons == 0xFFFFu, "invent predicted neutral");
    CHECK(got.stick_x == 0 && got.stick_y == 0, "invent sticks zero");
    CHECK(got.analog == 0u, "invent neutral digital");
    CHECK(h.invent_count == 1u, "invent count");

    /* Store auth remote at 11 then invent 12 → hold-last (incl. analog). */
    f = got;
    f.tick = 11;
    f.buttons = 0xFBFFu;
    f.stick_x = -20;
    f.analog = 1u;
    f.is_predicted = 0;
    CHECK(netplay_ih_put(&h, 1, &f), "put auth remote");
    CHECK(netplay_ih_invent_hold_last(&h, 1, 12, &got), "invent hold-last");
    CHECK(got.buttons == 0xFBFFu && got.stick_x == -20, "held buttons/stick");
    CHECK(got.analog == 1u, "held analog");
    CHECK(got.is_predicted, "hold-last predicted");

    /* Idle invent ignores prior hold (MotK menu path). */
    CHECK(netplay_ih_invent_idle(&h, 1, 13, &got), "invent idle");
    CHECK(got.is_predicted && got.buttons == 0xFFFFu, "idle buttons");
    CHECK(got.stick_x == 0 && got.stick_y == 0 && got.analog == 0u, "idle sticks");
    CHECK(h.invent_count == 3u, "invent count after idle");

    /* Promote replaces predicted (tick 13 idle row). */
    f = got;
    f.stick_x = -18;
    f.is_predicted = 0;
    CHECK(netplay_ih_promote(&h, 1, &f), "promote");
    CHECK(netplay_ih_get(&h, 1, 13, &got), "get promoted");
    CHECK(!got.is_predicted && got.stick_x == -18, "promoted flags/stick");
    CHECK(h.promote_count == 1u, "promote count");

    /* Contract: predicted same-intent + hash_confirm → PromoteHashConfirm. */
    rnet_input_contract_params_init_defaults(&params);
    netplay_ih_frame_to_contract(&got, &pub);
    pub.is_predicted = 1;
    pub.stick_x = -20;
    wire = pub;
    wire.is_predicted = 0;
    wire.stick_x = -18;
    memset(&gates, 0, sizeof(gates));
    gates.hash_confirm_promote = hc_no;
    d = rnet_input_contract_stick_replace_decide(&pub, &wire, 1, &params, &gates);
    CHECK(rnet_input_contract_decision_is_rewind(d), "no hash_confirm → rewind");
    gates.hash_confirm_promote = hc_yes;
    d = rnet_input_contract_stick_replace_decide(&pub, &wire, 1, &params, &gates);
    CHECK(d == nRNetInputContractPromoteHashConfirm, "hash_confirm → promote");
    CHECK(!rnet_input_contract_decision_is_rewind(d), "promote not rewind");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
