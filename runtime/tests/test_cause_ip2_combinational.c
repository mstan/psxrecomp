/*
 * Validate that COP0.CAUSE bit 10 (IP2) behaves as a COMBINATIONAL mirror of
 * the interrupt-controller line, not as a latch.
 *
 * On R3000A the Cause.IP field is not storage — it reflects the current state
 * of the interrupt input pins. On the PSX only IP2 is wired, and it carries the
 * INTC output, i.e. (I_STAT & I_MASK) != 0. So IP2 must RISE when a device
 * raises and FALL the instant the guest acks I_STAT or masks the source, with
 * no CPU involvement in either direction.
 *
 * This was previously wrong in two independent places: both the compiled
 * delivery path (interrupts.c) and the standalone interpreter delivery path
 * (psx_interpreter.c) only ever OR'd bit 10 in at delivery and never cleared
 * it. A kernel exception dispatcher that loops on CAUSE.IP & SR.IM to decide
 * whether to service again then sees a pending interrupt that no longer exists.
 *
 * Cross-checked against the independent Beetle oracle rather than asserted:
 * beetle-psx/mednafen/psx/irq.cpp recomputes the line as
 * AssertIRQ(0, (Status & Mask)) at power-on, at every assert, and at BOTH the
 * status-ack and mask-write halves of a register write; cpu.cpp's AssertIRQ
 * clears bit (10+n) unconditionally before re-setting it from the level.
 *
 * The mirror under test is deliberately tiny and depends only on i_stat/i_mask
 * plus a caller-supplied CAUSE pointer, so it is exercised directly here rather
 * than by linking the whole interrupt subsystem.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -Werror \
 *      -o test_cause_ip2_combinational test_cause_ip2_combinational.c
 */
#include <stdint.h>
#include <stdio.h>

/* ---- The unit under test -------------------------------------------------
 * Mirrors runtime/src/interrupts.c. Kept in lockstep with it; if the two ever
 * disagree this test is the thing that should be updated last, not first. */
uint32_t i_stat;
uint32_t i_mask;

static uint32_t *s_cause_ptr;

static void psx_irq_refresh_cause_ip2(void)
{
    if (!s_cause_ptr) return;
    if ((i_stat & i_mask & 0x7FFu) != 0u)
        *s_cause_ptr |= (1u << 10);
    else
        *s_cause_ptr &= ~(1u << 10);
}

static void psx_irq_set_cause_ptr(uint32_t *p)
{
    s_cause_ptr = p;
    psx_irq_refresh_cause_ip2();
}

static void psx_irq_raise(uint32_t bit)
{
    i_stat |= (1u << bit);
    psx_irq_refresh_cause_ip2();
}

/* Mirrors memory.c's interrupt_write_stat_masked: writing I_STAT ACKS bits —
 * a zero bit in the written value clears the corresponding pending bit. */
static void write_i_stat(uint32_t val, uint32_t mask)
{
    uint32_t ack_mask = mask & 0x7FFu;
    i_stat = (i_stat & ~ack_mask) | (i_stat & val & ack_mask);
    psx_irq_refresh_cause_ip2();
}

static void write_i_mask(uint32_t val, uint32_t mask)
{
    i_mask = ((i_mask & ~mask) | (val & mask)) & 0x7FFu;
    psx_irq_refresh_cause_ip2();
}

/* ---- Harness ------------------------------------------------------------ */
static int failures;

#define IP2(cause) (((cause) >> 10) & 1u)

#define CHECK(cond, label) do {                                               \
    if (!(cond)) {                                                            \
        fprintf(stderr, "FAIL: %s\n", (label));                               \
        failures++;                                                           \
    }                                                                         \
} while (0)

#define IRQ_VBLANK 0
#define IRQ_SPU    9

int main(void)
{
    uint32_t cause;

    /* --- power-on recompute (Beetle's IRQ_Power -> Recalc) --------------- */
    i_stat = 0; i_mask = 0;
    cause = 0xFFFFFFFFu;   /* worst case: bit 10 already set from junk state */
    psx_irq_set_cause_ptr(&cause);
    CHECK(IP2(cause) == 0, "wiring the mirror recomputes IP2 at power-on");

    /* --- a raise on a MASKED source must not assert the line ------------- */
    i_stat = 0; i_mask = 0; cause = 0;
    psx_irq_set_cause_ptr(&cause);
    psx_irq_raise(IRQ_VBLANK);
    CHECK(i_stat == 1u, "raise sets the I_STAT bit regardless of mask");
    CHECK(IP2(cause) == 0, "raise with the source masked leaves IP2 clear");

    /* --- unmasking an already-pending source asserts the line ----------- */
    write_i_mask(0x0001u, 0xFFFFu);
    CHECK(IP2(cause) == 1, "unmasking a pending source raises IP2");

    /* --- re-masking drops it again, with I_STAT untouched ---------------- */
    write_i_mask(0x0000u, 0xFFFFu);
    CHECK(i_stat == 1u, "masking does not clear the pending I_STAT bit");
    CHECK(IP2(cause) == 0, "masking a pending source drops IP2");

    /* --- raise on an UNMASKED source asserts immediately ----------------- */
    i_stat = 0; i_mask = 0x0001u; cause = 0;
    psx_irq_set_cause_ptr(&cause);
    CHECK(IP2(cause) == 0, "no pending source means IP2 clear");
    psx_irq_raise(IRQ_VBLANK);
    CHECK(IP2(cause) == 1, "raise on an unmasked source raises IP2");

    /* --- THE REGRESSION: acking I_STAT must drop IP2 --------------------- */
    /* Writing 0 to a pending bit acknowledges it. Before the fix, bit 10 was
     * latched at delivery and survived this, so a dispatcher looping on
     * CAUSE.IP & SR.IM re-serviced an interrupt that no longer existed. */
    write_i_stat(0x0000u, 0xFFFFu);
    CHECK(i_stat == 0u, "writing zero acks the pending bit");
    CHECK(IP2(cause) == 0, "acking the last pending source drops IP2");

    /* --- a partial ack leaves the line up while anything remains --------- */
    i_stat = 0; i_mask = (1u << IRQ_VBLANK) | (1u << IRQ_SPU); cause = 0;
    psx_irq_set_cause_ptr(&cause);
    psx_irq_raise(IRQ_VBLANK);
    psx_irq_raise(IRQ_SPU);
    CHECK(IP2(cause) == 1, "two pending sources raise IP2");
    write_i_stat(~(uint32_t)(1u << IRQ_VBLANK), 0xFFFFu);  /* ack VBlank only */
    CHECK(i_stat == (1u << IRQ_SPU), "partial ack clears only the acked bit");
    CHECK(IP2(cause) == 1, "IP2 stays asserted while another source is pending");
    write_i_stat(0x0000u, 0xFFFFu);
    CHECK(IP2(cause) == 0, "IP2 drops once the last source is acked");

    /* --- the mirror must not disturb other CAUSE bits -------------------- */
    /* ExcCode, BD and the software-interrupt bits IP0/IP1 are all real state
     * the guest or the exception path owns; only bit 10 belongs to the line. */
    i_stat = 0; i_mask = 0x0001u;
    cause = 0x8000000Cu | (1u << 8) | (1u << 9);   /* BD | ExcCode=3 | IP0 | IP1 */
    psx_irq_set_cause_ptr(&cause);
    uint32_t preserved = cause & ~(1u << 10);
    psx_irq_raise(IRQ_VBLANK);
    CHECK(IP2(cause) == 1, "raise asserts IP2 alongside existing CAUSE bits");
    CHECK((cause & ~(1u << 10)) == preserved,
          "asserting IP2 preserves ExcCode / BD / software-interrupt bits");
    write_i_stat(0x0000u, 0xFFFFu);
    CHECK((cause & ~(1u << 10)) == preserved,
          "clearing IP2 preserves ExcCode / BD / software-interrupt bits");

    /* --- byte-width register writes must still recompute ----------------- */
    /* I_STAT/I_MASK are byte- and halfword-writable; the mask argument is how
     * partial writes are expressed, and every one of them can move the line. */
    i_stat = 0; i_mask = 0; cause = 0;
    psx_irq_set_cause_ptr(&cause);
    psx_irq_raise(IRQ_SPU);                 /* bit 9 — in the high byte */
    CHECK(IP2(cause) == 0, "masked SPU raise leaves IP2 clear");
    write_i_mask(0x0200u, 0xFF00u);         /* halfword-high write unmasks it */
    CHECK(IP2(cause) == 1, "a partial-width mask write still recomputes IP2");

    /* --- bits above the 11-bit INTC range never drive the line ---------- */
    i_stat = 0; i_mask = 0; cause = 0;
    psx_irq_set_cause_ptr(&cause);
    i_stat = 0xF800u;                       /* outside the 0x7FF INTC range */
    i_mask = 0xF800u;
    psx_irq_refresh_cause_ip2();
    CHECK(IP2(cause) == 0, "out-of-range I_STAT/I_MASK bits do not assert IP2");

    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    printf("test_cause_ip2_combinational: all checks passed\n");
    return 0;
}
