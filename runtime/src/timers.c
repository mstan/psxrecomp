/*
 * timers.c — PS1 root counters (3 timers), v4 clean port.
 *
 * Pure hardware simulation. No BIOS state, no HLE, no interpreter.
 *
 * Per-timer registers (base = 0x1F801100 + timer*0x10):
 *   +0x00: Counter value (16-bit)
 *   +0x04: Counter mode
 *   +0x08: Counter target
 *
 * Clock sources:
 *   Timer 0: system clock or dotclock
 *   Timer 1: system clock or hblank
 *   Timer 2: system clock or system/8
 */

#include "timers.h"
#include <string.h>

/* Mode register bit definitions */
#define MODE_SYNC_EN       (1 << 0)
#define MODE_SYNC_MASK     (3 << 1)
#define MODE_RESET_TARGET  (1 << 3)
#define MODE_IRQ_TARGET    (1 << 4)
#define MODE_IRQ_OVERFLOW  (1 << 5)
#define MODE_IRQ_REPEAT    (1 << 6)
#define MODE_IRQ_TOGGLE    (1 << 7)
#define MODE_CLK_SRC0      (1 << 8)
#define MODE_CLK_SRC1      (1 << 9)
#define MODE_IRQ_REQUEST   (1 << 10)
#define MODE_TARGET_FLAG   (1 << 11)
#define MODE_OVERFLOW_FLAG (1 << 12)

/* IRQ bit positions in I_STAT for timers */
#define IRQ_TIMER0 4
#define IRQ_TIMER1 5
#define IRQ_TIMER2 6

static const int timer_irq[3] = { IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2 };

typedef struct {
    uint16_t counter;
    uint32_t mode;
    uint16_t target;
    int      irq_line; /* toggle mode state */
} Timer;

static Timer timers[3];
static uint32_t timer_frac[3];

/* I_STAT is owned by memory.c — we poke it directly via this extern. */
extern uint32_t i_stat;

void timers_init(void) {
    memset(timers, 0, sizeof(timers));
    memset(timer_frac, 0, sizeof(timer_frac));
}

/* Determine whether this timer uses system clock ticks */
static int timer_uses_sysclk(int t) {
    int src = (timers[t].mode >> 8) & 3;
    switch (t) {
        case 0: return (src == 0 || src == 2); /* 0,2=sysclk  1,3=dotclock */
        case 1: return (src == 0 || src == 2); /* 0,2=sysclk  1,3=hblank */
        case 2: return (src == 0 || src == 1); /* 0,1=sysclk  2,3=sysclk/8 */
        default: return 1;
    }
}

/* Fire IRQ for a timer — sets I_STAT bit. No CPU exception delivery here;
 * the recompiled BIOS checks (i_stat & i_mask) on its own schedule. */
static void timer_fire_irq(int t) {
    Timer* tm = &timers[t];

    if (tm->mode & MODE_IRQ_TOGGLE) {
        tm->irq_line ^= 1;
        if (tm->irq_line) {
            tm->mode &= ~MODE_IRQ_REQUEST;
            i_stat |= (1 << timer_irq[t]);
        } else {
            tm->mode |= MODE_IRQ_REQUEST;
        }
    } else {
        /* Pulse mode */
        tm->mode &= ~MODE_IRQ_REQUEST;
        i_stat |= (1 << timer_irq[t]);
        tm->mode |= MODE_IRQ_REQUEST;
    }

    if (!(tm->mode & MODE_IRQ_REPEAT)) {
        tm->mode &= ~(MODE_IRQ_TARGET | MODE_IRQ_OVERFLOW);
    }
}

/* Tick a single timer by one count. Returns nothing; updates flags/IRQ. */
static void timer_tick_one(int t) {
    Timer* tm = &timers[t];
    uint16_t old = tm->counter;
    tm->counter++;

    if (tm->counter == tm->target) {
        tm->mode |= MODE_TARGET_FLAG;
        if (tm->mode & MODE_IRQ_TARGET)
            timer_fire_irq(t);
        if (tm->mode & MODE_RESET_TARGET)
            tm->counter = 0;
    }
    if (old == 0xFFFF) {
        tm->mode |= MODE_OVERFLOW_FLAG;
        if (tm->mode & MODE_IRQ_OVERFLOW)
            timer_fire_irq(t);
    }
}

static void timer_advance_counts(int t, uint32_t ticks) {
    Timer* tm = &timers[t];
    while (ticks > 0) {
        uint32_t target_dist = ((uint32_t)tm->target - (uint32_t)tm->counter) & 0xFFFFu;
        uint32_t overflow_dist = 0x10000u - (uint32_t)tm->counter;
        if (target_dist == 0) target_dist = 0x10000u;

        uint32_t step = ticks;
        if (step > target_dist) step = target_dist;
        if (step > overflow_dist) step = overflow_dist;
        if (step == 0) step = 1;

        uint16_t old = tm->counter;
        tm->counter = (uint16_t)(tm->counter + step);
        ticks -= step;

        if (step == target_dist) {
            tm->mode |= MODE_TARGET_FLAG;
            if (tm->mode & MODE_IRQ_TARGET)
                timer_fire_irq(t);
            if (tm->mode & MODE_RESET_TARGET)
                tm->counter = 0;
        }
        if (old + step > 0xFFFFu || step == overflow_dist) {
            tm->mode |= MODE_OVERFLOW_FLAG;
            if (tm->mode & MODE_IRQ_OVERFLOW)
                timer_fire_irq(t);
        }
    }
}

static void timer_advance_divided(int t, uint32_t cycles, uint32_t divisor) {
    if (divisor == 0) divisor = 1;
    uint32_t total = timer_frac[t] + cycles;
    uint32_t ticks = total / divisor;
    timer_frac[t] = total % divisor;
    if (ticks != 0)
        timer_advance_counts(t, ticks);
}

void timers_advance(uint32_t cycles) {
    if (cycles == 0) return;

    for (int t = 0; t < 3; t++) {
        int src = (timers[t].mode >> 8) & 3;
        if (t == 2 && (src == 2 || src == 3)) {
            timer_advance_divided(t, cycles, 8);
        } else if (timer_uses_sysclk(t)) {
            timer_advance_counts(t, cycles);
        } else if (t == 1) {
            /* Timer 1 HBlank clock. NTSC has about 263 HBlanks per frame. */
            timer_advance_divided(t, cycles, 2146);
        } else if (t == 0) {
            /* Timer 0 dotclock approximation; exact divider depends on GPU mode. */
            timer_advance_divided(t, cycles, 5);
        }
    }
}

uint32_t timers_read(uint32_t addr) {
    int timer = (addr - TIMER_BASE) >> 4;
    int reg   = (addr - TIMER_BASE) & 0x0F;

    if (timer < 0 || timer > 2) return 0;

    switch (reg) {
        case 0x00: {
#ifndef PSX_ENABLE_BLOCK_CYCLES
            /* Advance counter by a small amount to simulate continuous
             * counting between bulk timers_tick calls. */
            int inc = 8;
            if (timer == 2 && (timers[timer].mode & MODE_CLK_SRC1))
                inc = 1; /* sysclk/8 mode */
            uint16_t old = timers[timer].counter;
            timers[timer].counter += inc;
            if (timers[timer].target != 0 &&
                old < timers[timer].target &&
                timers[timer].counter >= timers[timer].target) {
                timers[timer].mode |= MODE_TARGET_FLAG;
                if (timers[timer].mode & MODE_RESET_TARGET)
                    timers[timer].counter = 0;
            }
#endif
            return timers[timer].counter;
        }
        case 0x04: {
            uint32_t val = timers[timer].mode;
            timers[timer].mode &= ~(MODE_TARGET_FLAG | MODE_OVERFLOW_FLAG);
            return val;
        }
        case 0x08:
            return timers[timer].target;
        default:
            return 0;
    }
}

void timers_write(uint32_t addr, uint32_t value) {
    int timer = (addr - TIMER_BASE) >> 4;
    int reg   = (addr - TIMER_BASE) & 0x0F;

    if (timer < 0 || timer > 2) return;

    switch (reg) {
        case 0x00:
            timers[timer].counter = value & 0xFFFF;
            break;
        case 0x04:
            timers[timer].mode = (value & 0x03FF) | MODE_IRQ_REQUEST;
            timers[timer].counter = 0;
            timers[timer].irq_line = 0;
            timer_frac[timer] = 0;
            break;
        case 0x08:
            timers[timer].target = value & 0xFFFF;
            break;
        default:
            break;
    }
}

void timers_tick(int cycles) {
    /* Timer 1 in HBlank mode: ~263 HBlanks per NTSC frame */
    if (!timer_uses_sysclk(1)) {
        for (int h = 0; h < 263; h++)
            timer_tick_one(1);
    }

    for (int t = 0; t < 3; t++) {
        int ticks;
        if (timer_uses_sysclk(t)) {
            ticks = cycles;
        } else if (t == 2) {
            ticks = cycles / 8;
        } else {
            continue;
        }
        for (int c = 0; c < ticks; c++)
            timer_tick_one(t);
    }
}
