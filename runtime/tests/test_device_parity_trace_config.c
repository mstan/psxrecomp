/* Focused two-mode contract test for device_trace.c and parity_trace.c.
 * Build once normally and once with PSX_NO_DEBUG_TOOLS=1. */
#include "device_trace.h"
#include "parity_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if DEVICE_TRACE_ENABLED != PARITY_TRACE_ENABLED
#error "device and parity trace build modes disagree"
#endif

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

#if DEVICE_TRACE_ENABLED

static uint32_t s_frame = 7u;
static uint64_t s_cycle = 99u;

uint32_t parity_host_frame(void) { return s_frame; }
uint64_t parity_host_cycle(void) { return s_cycle; }

static uint32_t read_word(void *ctx, uint32_t addr)
{
    (void)ctx;
    switch (addr) {
        case 0x00000108u: return 0x00000200u; /* TCBH pointer */
        case 0x00000200u: return 0x00000300u; /* current TCB */
        case 0x00000388u: return 0x80012340u; /* watched TCB EPC */
        case 0x00000300u: return 0x00004000u; /* watched TCB state */
        case 0x00001000u: return 0x11223344u; /* watch word */
        default: return 0u;
    }
}

static int test_debug(void)
{
    DevEvent events[2];
    ParityEntry entries[2];
    ParityEntry read_entry;
    const uint32_t watch = 0x00001000u;

    device_trace_reset();
    device_trace_arm(1);
    device_trace_note(DEV_IRQ_DMA, 3u);
    s_frame = 8u;
    s_cycle = 123u;
    device_trace_note(DEV_IRQ_CDROM, 9u);
    if (!device_trace_is_armed() || device_trace_total() != 2u ||
        device_trace_get(events, 2u) != 2u)
        return fail("debug device totals");
    if (events[0].seq != 0u || events[0].cycle != 99u ||
        events[0].frame != 7u || events[0].source != DEV_IRQ_DMA ||
        events[0].detail != 3u || events[1].seq != 1u ||
        events[1].cycle != 123u || events[1].frame != 8u ||
        events[1].source != DEV_IRQ_CDROM || events[1].detail != 9u)
        return fail("debug device event contents");
    if (strcmp(device_source_str(DEV_IRQ_DMA), "dma") != 0)
        return fail("debug device source name");

    parity_trace_config(0x00000300u, 0x00004444u, 0x88u, 0u, &watch, 1);
    parity_trace_reset();
    parity_trace_arm(1);
    parity_trace_record(PARITY_KIND_DISPATCH, 1u, 2u, 3u, 4u,
                        read_word, NULL);
    parity_trace_note_write(watch, 4u, 0x00001234u);
    parity_trace_note_read(watch, 0xAABBCCDDu, 0x00005678u);
    parity_trace_record(PARITY_KIND_YIELD, 5u, 6u, 7u, 0x00004444u,
                        read_word, NULL);
    if (!parity_trace_is_armed() || !parity_trace_is_frozen() ||
        parity_trace_total() != 2u || parity_trace_get(entries, 2u) != 2u)
        return fail("debug parity totals or trigger freeze");
    if (entries[0].kind != PARITY_KIND_DISPATCH ||
        entries[0].current_tcb != 0x00000300u ||
        entries[0].epc != 0x80012340u ||
        entries[0].tcb_state != 0x00004000u ||
        entries[0].watch[0] != 0x11223344u ||
        entries[1].kind != PARITY_KIND_TRIGGER ||
        entries[1].watch_wpc[0] != 0x00001234u ||
        entries[1].watch_wcycle[0] != 123u ||
        entries[1].watch_wframe[0] != 8u ||
        entries[1].watch_wtcb[0] != 0x00000300u)
        return fail("debug parity entry contents");
    if (parity_trace_reads_total() != 1u ||
        parity_trace_reads_get(&read_entry, 1u) != 1u ||
        read_entry.pc != 0x00005678u || read_entry.epc != watch ||
        read_entry.target != 0xAABBCCDDu ||
        read_entry.watch[0] != 0xAABBCCDDu ||
        read_entry.watch_wpc[0] != 0x00001234u)
        return fail("debug parity read provenance");
    if (strcmp(parity_kind_str(PARITY_KIND_YIELD), "yield") != 0)
        return fail("debug parity kind name");

    device_trace_reset();
    parity_trace_reset();
    if (device_trace_total() != 0u || parity_trace_total() != 0u ||
        parity_trace_reads_total() != 0u || parity_trace_is_frozen())
        return fail("debug reset");
    return 0;
}

#else

static int test_lean(void)
{
    int evaluated = 0;
    DevEvent device_event;
    ParityEntry parity_entry;

    /* Function-like macros must discard producer/control argument evaluation. */
    device_trace_arm(++evaluated);
    device_trace_note((uint32_t)++evaluated, (uint32_t)++evaluated);
    parity_trace_config((uint32_t)++evaluated, (uint32_t)++evaluated,
                        (uint32_t)++evaluated, (uint32_t)++evaluated, NULL,
                        ++evaluated);
    parity_trace_arm(++evaluated);
    parity_trace_record(PARITY_KIND_DISPATCH, (uint32_t)++evaluated,
                        (uint32_t)++evaluated, (uint32_t)++evaluated,
                        (uint32_t)++evaluated, NULL, NULL);
    parity_trace_note_write((uint32_t)++evaluated, (uint32_t)++evaluated,
                            (uint32_t)++evaluated);
    parity_trace_note_read((uint32_t)++evaluated, (uint32_t)++evaluated,
                           (uint32_t)++evaluated);
    if (evaluated != 0)
        return fail("lean producer evaluated an argument");

    if (device_trace_is_armed() || device_trace_total() ||
        device_trace_get(&device_event, 1u) || parity_trace_is_armed() ||
        parity_trace_is_frozen() || parity_trace_total() ||
        parity_trace_get(&parity_entry, 1u) || parity_trace_reads_total() ||
        parity_trace_reads_get(&parity_entry, 1u))
        return fail("lean header queries are not empty");

    /* Function-like macros do not expand when a bare function name is used.
     * Taking and calling every address proves the external ABI remains linkable
     * and that its disabled implementations are inert zero-result stubs. */
    {
        void (*device_arm_fn)(int) = device_trace_arm;
        void (*device_reset_fn)(void) = device_trace_reset;
        int (*device_armed_fn)(void) = device_trace_is_armed;
        void (*device_note_fn)(uint32_t, uint32_t) = device_trace_note;
        uint32_t (*device_get_fn)(DevEvent *, uint32_t) = device_trace_get;
        uint64_t (*device_total_fn)(void) = device_trace_total;
        const char *(*device_name_fn)(uint32_t) = device_source_str;
        void (*parity_config_fn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                 const uint32_t *, int) = parity_trace_config;
        void (*parity_arm_fn)(int) = parity_trace_arm;
        void (*parity_reset_fn)(void) = parity_trace_reset;
        int (*parity_armed_fn)(void) = parity_trace_is_armed;
        int (*parity_frozen_fn)(void) = parity_trace_is_frozen;
        void (*parity_record_fn)(parity_kind_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, parity_read_word_fn, void *) =
            parity_trace_record;
        void (*parity_write_fn)(uint32_t, uint32_t, uint32_t) =
            parity_trace_note_write;
        void (*parity_read_fn)(uint32_t, uint32_t, uint32_t) =
            parity_trace_note_read;
        uint32_t (*parity_get_fn)(ParityEntry *, uint32_t) = parity_trace_get;
        uint64_t (*parity_total_fn)(void) = parity_trace_total;
        const char *(*parity_name_fn)(uint32_t) = parity_kind_str;
        uint32_t (*parity_reads_get_fn)(ParityEntry *, uint32_t) =
            parity_trace_reads_get;
        uint64_t (*parity_reads_total_fn)(void) = parity_trace_reads_total;

        device_arm_fn(1);
        device_note_fn(DEV_IRQ_DMA, 3u);
        device_reset_fn();
        parity_config_fn(1u, 2u, 3u, 4u, NULL, 0);
        parity_arm_fn(1);
        parity_record_fn(PARITY_KIND_YIELD, 1u, 2u, 3u, 4u, NULL, NULL);
        parity_write_fn(1u, 4u, 2u);
        parity_read_fn(1u, 2u, 3u);
        parity_reset_fn();
        if (device_armed_fn() || device_total_fn() ||
            device_get_fn(&device_event, 1u) || parity_armed_fn() ||
            parity_frozen_fn() || parity_total_fn() ||
            parity_get_fn(&parity_entry, 1u) || parity_reads_total_fn() ||
            parity_reads_get_fn(&parity_entry, 1u))
            return fail("lean external ABI stubs are not empty");
        if (strcmp(device_name_fn(DEV_IRQ_DMA), "dma") != 0 ||
            strcmp(parity_name_fn(PARITY_KIND_YIELD), "yield") != 0)
            return fail("lean name helpers changed");
    }
    return 0;
}

#endif

int main(void)
{
#if DEVICE_TRACE_ENABLED
    if (test_debug()) return 1;
#else
    if (test_lean()) return 1;
#endif
    puts("PASS: device/parity trace configuration contract");
    return 0;
}
