/* test_func_override.c — pins the func_override tier's contract.
 *
 * The tier's failure modes are all SILENT: a registration that never gets
 * consulted, a package override that survives clear-mods, a guard that
 * corrupts instead of declining. None of those announce themselves at
 * runtime, so they need pinning here rather than field observation.
 *
 * The tests drive the real dispatcher entry point — g_psx_func_override_hook,
 * the same pointer the generated dispatch and the interpreter call through —
 * so nothing here exercises a test-only path.
 */

#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "func_override.h"

extern int (*g_psx_func_override_hook)(CPUState *cpu, uint32_t phys);

static int g_fail = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail = 1;                                                       \
        }                                                                     \
    } while (0)

/* ---- test doubles ------------------------------------------------------- */

/* Guest RAM the residency guard peeks at. func_override.c reads through
 * psx_peek_word_untraced specifically so this stays a plain host read with no
 * lockstep/oracle side effects — that is the property under test in
 * guard_declines_on_mismatch, and the reason this double is trivial. */
#define FAKE_RAM_WORDS 64
static uint32_t s_ram[FAKE_RAM_WORDS];

uint32_t psx_peek_word_untraced(uint32_t addr)
{
    const uint32_t idx = (addr & 0x1FFFFFFFu) >> 2;
    return (idx < FAKE_RAM_WORDS) ? s_ram[idx] : 0xDEADBEEFu;
}

/* Cycle-core doubles: func_override.c charges fixed credits through the
 * psx_advance_cycles inline (psx_cycles.h), which reads these globals. The
 * doubles keep it a plain counter bump so the credit tests can assert
 * against psx_cycle_count deltas directly. */
uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
int      psx_in_device_service = 0;
int      g_event_step_conservative = 0;
int      g_ls_replay_active = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
void psx_devices_service_to_now(void) {}
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }

/* Stands in for the recompiled dispatcher. Re-consults the hook exactly as
 * psx_dispatch_impl does, so func_override_call_original's one-shot bypass is
 * exercised through its real mechanism rather than assumed. */
static int s_original_runs = 0;
static int s_dispatch_depth = 0;

/* When set, the "original" body at this address calls ITSELF once — modelling
 * a self-recursive guest function, the case where the one-shot bypass must
 * already have been consumed so the recursive call re-consults the override
 * (what a guest-level wrap would observe). */
static uint32_t s_recursive_original_at = 0;
static int s_recursion_budget = 0;

void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t return_addr)
{
    (void)return_addr;
    if (s_dispatch_depth > 8) return;   /* runaway guard, not part of the contract */
    s_dispatch_depth++;
    if (!g_psx_func_override_hook ||
        !g_psx_func_override_hook(cpu, addr & 0x1FFFFFFFu)) {
        /* Nothing handled it: this is the original body running. */
        s_original_runs++;
        cpu->gpr[2] = 0xAAAAu;
        if (s_recursive_original_at &&
            (addr & 0x1FFFFFFFu) == (s_recursive_original_at & 0x1FFFFFFFu) &&
            s_recursion_budget > 0) {
            s_recursion_budget--;
            psx_dispatch_call(cpu, addr, return_addr);
        }
    }
    s_dispatch_depth--;
}

/* ---- override implementations ------------------------------------------- */

static int s_impl_calls, s_decline_calls, s_wrap_calls;

static int impl_handles(CPUState *cpu)
{
    s_impl_calls++;
    cpu->gpr[2] = 0x1234u;
    return 1;
}

static int impl_declines(CPUState *cpu)
{
    (void)cpu;
    s_decline_calls++;
    return 0;   /* the decline-only probe idiom the TCP inventory relies on */
}

static int impl_wraps(CPUState *cpu)
{
    s_wrap_calls++;
    func_override_call_original(cpu);
    cpu->gpr[2] += 1u;   /* prove we ran after the original */
    return 1;
}

/* ---- helpers ------------------------------------------------------------ */

static void reset_all(void)
{
    /* No teardown for direct registrations exists by design (they are
     * always-on), so each test group runs in a fresh process section by
     * using distinct addresses instead of clearing. Counters do reset. */
    s_impl_calls = s_decline_calls = s_wrap_calls = 0;
    s_original_runs = 0;
    s_recursive_original_at = 0;
    s_recursion_budget = 0;
    memset(s_ram, 0, sizeof(s_ram));
}

static int consult(CPUState *cpu, uint32_t addr)
{
    if (!g_psx_func_override_hook) return 0;
    return g_psx_func_override_hook(cpu, addr & 0x1FFFFFFFu);
}

/* ---- tests -------------------------------------------------------------- */

static void test_install_is_null_until_registered(void)
{
    /* The whole "costs nothing when unused" claim rests on this: with nothing
     * registered the hook pointer must stay NULL so dispatch matches a build
     * without the tier. */
    func_override_install();
    CHECK(g_psx_func_override_hook == NULL,
          "hook must stay NULL with nothing registered");
}

static void test_add_rejects_bad_input(void)
{
    CHECK(func_override_add("t.null_fn", 0x80001000u, NULL, 0) == FO_ERR_ARGS,
          "NULL fn must be refused");
    /* phys 0 collides with the "not inside an override" sentinel. */
    CHECK(func_override_add("t.zero", 0x80000000u, impl_handles, 0) == FO_ERR_ARGS,
          "address normalising to phys 0 must be refused");
    CHECK(func_override_add("t.zero2", 0x00000000u, impl_handles, 0) == FO_ERR_ARGS,
          "raw address 0 must be refused");
    static const uint32_t g[1] = {0};
    CHECK(func_override_add_guarded("t.g0", 0x80001000u, impl_handles, g, 0, 0)
              == FO_ERR_ARGS,
          "guarded with n_words 0 must be refused");
    CHECK(func_override_add_guarded("t.gmax", 0x80001000u, impl_handles, g,
                                    FO_MAX_GUARD_WORDS + 1, 0) == FO_ERR_ARGS,
          "guard word count over the cap must be refused");
    CHECK(func_override_add_guarded("t.gnull", 0x80001000u, impl_handles, NULL,
                                    2, 0) == FO_ERR_ARGS,
          "guarded with NULL words must be refused");
}

static void test_duplicate_address_refused(void)
{
    const uint32_t addr = 0x80002000u;
    CHECK(func_override_add("t.first", addr, impl_handles, 0) == FO_OK,
          "first registration must succeed");
    /* A silent last-wins here would be untraceable, which is why this is an
     * error and not an overwrite. KSEG vs physical must not defeat it. */
    CHECK(func_override_add("t.second", addr, impl_declines, 0) == FO_ERR_DUPLICATE,
          "same address twice must be refused");
    CHECK(func_override_add("t.second_phys", addr & 0x1FFFFFFFu, impl_declines, 0)
              == FO_ERR_DUPLICATE,
          "same address in physical form must also be refused");
}

static void test_handled_and_declined_both_count_as_consults(void)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    reset_all();

    const uint32_t handled_at = 0x80003000u;
    const uint32_t declined_at = 0x80003100u;
    CHECK(func_override_add("t.handled", handled_at, impl_handles, 0) == FO_OK, "add handled");
    CHECK(func_override_add("t.declined", declined_at, impl_declines, 0) == FO_OK, "add declined");
    func_override_install();
    CHECK(g_psx_func_override_hook != NULL, "hook must install once populated");

    CHECK(consult(&cpu, handled_at) == 1, "handled override must report handled");
    CHECK(cpu.gpr[2] == 0x1234u, "handled override must write $v0");
    CHECK(s_impl_calls == 1, "handled impl must run once");

    CHECK(consult(&cpu, declined_at) == 0, "declining override must report not-handled");
    CHECK(s_decline_calls == 1, "declining impl must still run");

    /* An unregistered address must not be claimed. */
    CHECK(consult(&cpu, 0x8000FF00u) == 0, "unregistered address must not be handled");

    /* calls counts CONSULTS, so a decline-only probe proves reachability. */
    int found_handled = 0, found_declined = 0;
    for (int i = 0; i < func_override_count(); i++) {
        char id[FO_MAX_ID];
        uint32_t addr = 0;
        uint64_t calls = 0, misses = 0;
        int guarded = -1;
        if (!func_override_get_ex(i, id, sizeof(id), &addr, &calls, &misses,
                                  &guarded, NULL))
            continue;
        if (addr == (handled_at & 0x1FFFFFFFu)) {
            found_handled = 1;
            CHECK(calls == 1, "handled entry calls==1, got %llu",
                  (unsigned long long)calls);
            CHECK(misses == 0, "handled entry must have no guard misses");
            CHECK(guarded == 0, "handled entry is unguarded");
            CHECK(strcmp(id, "t.handled") == 0, "id round-trips, got '%s'", id);
        }
        if (addr == (declined_at & 0x1FFFFFFFu)) {
            found_declined = 1;
            CHECK(calls == 1, "DECLINED entry must still count a consult, got %llu",
                  (unsigned long long)calls);
        }
    }
    CHECK(found_handled && found_declined, "both entries must be enumerable");
}

static void test_guard_declines_on_mismatch(void)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    reset_all();

    /* Address inside the fake RAM window so the guard can be satisfied. */
    const uint32_t addr = 0x80000040u;   /* phys 0x40 -> word index 16 */
    static const uint32_t prologue[2] = {0x27BDFFE0u, 0xAFB20018u};
    CHECK(func_override_add_guarded("t.guarded", addr, impl_handles, prologue, 2, 0)
              == FO_OK, "guarded add must succeed");
    func_override_install();

    /* Wrong code resident: must DECLINE and never run the impl. Corrupting
     * here instead of declining is the failure the guard exists to prevent. */
    s_ram[16] = 0xFFFFFFFFu;
    s_ram[17] = 0xFFFFFFFFu;
    CHECK(consult(&cpu, addr) == 0, "guard mismatch must decline");
    CHECK(s_impl_calls == 0, "guard mismatch must NOT run the override body");

    /* Right code resident: must fire. */
    s_ram[16] = prologue[0];
    s_ram[17] = prologue[1];
    CHECK(consult(&cpu, addr) == 1, "guard match must handle");
    CHECK(s_impl_calls == 1, "guard match must run the override body once");

    /* Partial match is still a miss. */
    s_ram[17] = 0u;
    CHECK(consult(&cpu, addr) == 0, "partial guard match must decline");
    CHECK(s_impl_calls == 1, "partial match must not run the body");

    for (int i = 0; i < func_override_count(); i++) {
        char id[FO_MAX_ID];
        uint32_t a = 0;
        uint64_t calls = 0, misses = 0;
        int guarded = 0;
        if (!func_override_get_ex(i, id, sizeof(id), &a, &calls, &misses,
                                  &guarded, NULL))
            continue;
        if (a != (addr & 0x1FFFFFFFu)) continue;
        CHECK(guarded == 2, "guard word count must be reported, got %d", guarded);
        CHECK(misses == 2, "two guard misses expected, got %llu",
              (unsigned long long)misses);
    }
}

static void test_call_original_is_one_shot(void)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    reset_all();

    const uint32_t addr = 0x80004000u;
    CHECK(func_override_add("t.wrap", addr, impl_wraps, FO_CREDIT_SELF) == FO_OK, "add wrap");
    func_override_install();

    CHECK(consult(&cpu, addr) == 1, "wrap must handle");
    CHECK(s_wrap_calls == 1, "wrap body runs once");
    /* The bypass must let the ORIGINAL through exactly once — not zero times
     * (wrap becomes replace) and not unboundedly (infinite re-entry). */
    CHECK(s_original_runs == 1, "original must run exactly once, ran %d",
          s_original_runs);
    CHECK(cpu.gpr[2] == 0xAAAAu + 1u,
          "wrap must observe then adjust the original's $v0, got 0x%X",
          cpu.gpr[2]);

    /* Bypass must not persist: a second call re-consults the override. */
    CHECK(consult(&cpu, addr) == 1, "second call must still be handled");
    CHECK(s_wrap_calls == 2, "wrap body runs again on the second call");
    CHECK(s_original_runs == 2, "original runs once more, total %d",
          s_original_runs);
}

static void test_bypass_is_consumed_so_recursion_reconsults(void)
{
    /* The header promises: "if the original recursively calls itself, the
     * recursive calls consult the override again." That requires the one-shot
     * bypass to be CONSUMED (cleared) at the moment it is honoured, not merely
     * restored when call_original returns — restoring alone looks correct for
     * a non-recursive wrap and hides the bug. */
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    reset_all();

    const uint32_t addr = 0x80004100u;
    CHECK(func_override_add("t.wrap_rec", addr, impl_wraps, FO_CREDIT_SELF) == FO_OK,
          "add recursive wrap");
    func_override_install();

    s_recursive_original_at = addr;
    s_recursion_budget = 1;   /* the original self-calls exactly once */

    CHECK(consult(&cpu, addr) == 1, "recursive wrap must handle");

    /* Outer override runs, calls the original; the original self-calls, and
     * that inner call must land on the OVERRIDE again (wrap body twice), not
     * on the original a second time. */
    CHECK(s_wrap_calls == 2,
          "override must be re-consulted on the original's self-call, wrap ran %d",
          s_wrap_calls);
    CHECK(s_original_runs == 2,
          "each override invocation runs the original once, original ran %d",
          s_original_runs);
}

static void test_call_original_outside_override_is_a_noop(void)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    reset_all();
    func_override_call_original(&cpu);   /* must not dispatch anything */
    CHECK(s_original_runs == 0,
          "call_original outside an override must do nothing");
}

static void test_package_reset_drops_only_package_entries(void)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    reset_all();

    const uint32_t direct_at = 0x80005000u;
    const uint32_t pkg_at = 0x80005100u;
    CHECK(func_override_add("t.direct", direct_at, impl_handles, 0) == FO_OK,
          "direct add");
    CHECK(func_override_add_package("pkg.feature:a", pkg_at, impl_handles, NULL, 0, 0)
              == FO_OK, "package add");
    func_override_install();

    CHECK(consult(&cpu, direct_at) == 1, "direct override armed");
    CHECK(consult(&cpu, pkg_at) == 1, "package override armed");

    const int before = func_override_count();
    const int dropped = func_override_reset_package_armed();
    CHECK(dropped >= 1, "reset must report dropping the package entry");
    CHECK(func_override_count() == before - dropped,
          "count must shrink by exactly the dropped entries");

    /* This is the netplay-divergence regression: after clear-mods the package
     * override must be gone, while the game's own always-on reimplementation
     * survives. */
    CHECK(consult(&cpu, pkg_at) == 0,
          "package override must NOT fire after reset (netplay divergence)");
    CHECK(consult(&cpu, direct_at) == 1,
          "direct override must survive reset");

    /* Compaction must not corrupt surviving entries. */
    int seen_direct = 0;
    for (int i = 0; i < func_override_count(); i++) {
        char id[FO_MAX_ID];
        uint32_t a = 0;
        uint64_t calls = 0;
        if (!func_override_get_ex(i, id, sizeof(id), &a, &calls, NULL, NULL,
                                  NULL))
            continue;
        if (a == (direct_at & 0x1FFFFFFFu)) {
            seen_direct = 1;
            CHECK(strcmp(id, "t.direct") == 0,
                  "surviving id intact after compaction, got '%s'", id);
        }
        CHECK(a != (pkg_at & 0x1FFFFFFFu),
              "dropped package entry must not remain enumerable");
    }
    CHECK(seen_direct, "direct entry must remain enumerable");

    /* The address is free again, so a later plan can re-arm it. */
    CHECK(func_override_add_package("pkg.feature:a", pkg_at, impl_handles, NULL, 0, 0)
              == FO_OK, "re-arming after reset must succeed");
    CHECK(func_override_reset_package_armed() >= 1, "and drop again");
}

static void test_get_ex_bounded_id_copy(void)
{
    /* The id buffer size is the caller's to state; a short buffer must
     * truncate with termination, never overrun. */
    int checked = 0;
    for (int i = 0; i < func_override_count(); i++) {
        char small[4];
        uint32_t a = 0;
        memset(small, 0x7F, sizeof(small));
        if (!func_override_get_ex(i, small, sizeof(small), &a, NULL, NULL, NULL,
                                  NULL))
            continue;
        CHECK(small[sizeof(small) - 1] == '\0',
              "short id buffer must be NUL-terminated");
        CHECK(strlen(small) <= sizeof(small) - 1, "short id must be truncated");
        checked = 1;
    }
    CHECK(checked, "expected at least one entry to enumerate");

    CHECK(func_override_get_ex(-1, NULL, 0, NULL, NULL, NULL, NULL, NULL) == 0,
          "negative index must fail");
    CHECK(func_override_get_ex(func_override_count(), NULL, 0, NULL, NULL, NULL,
                               NULL, NULL) == 0,
          "out-of-range index must fail");
}

static void test_credit_policy(void)
{
    reset_all();
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    /* The credit is a required statement: FO_CREDIT_SELF or >= 0. Any other
     * negative value is a typo, not a policy. */
    CHECK(func_override_add("t.badcredit", 0x80006000u, impl_handles, -2)
              == FO_ERR_ARGS,
          "credit below FO_CREDIT_SELF must be refused");

    CHECK(func_override_add("t.credit40", 0x80006100u, impl_handles, 40)
              == FO_OK, "fixed-credit add");
    CHECK(func_override_add("t.credit40d", 0x80006200u, impl_declines, 40)
              == FO_OK, "fixed-credit decline probe add");
    CHECK(func_override_add("t.creditself", 0x80006300u, impl_handles,
                            FO_CREDIT_SELF)
              == FO_OK, "self-credit add");
    CHECK(func_override_add("t.credit0", 0x80006400u, impl_handles, 0)
              == FO_OK, "zero-credit add");
    func_override_install();

    /* A handled call charges exactly the declared credit... */
    uint64_t before = psx_cycle_count;
    CHECK(consult(&cpu, 0x80006100u) == 1, "fixed-credit override handles");
    CHECK(psx_cycle_count - before == 40u,
          "handled call must charge the declared credit");

    /* ...a DECLINE charges nothing (the original runs and self-charges
       through the normal backends, so a tier-side charge would double). */
    before = psx_cycle_count;
    CHECK(consult(&cpu, 0x80006200u) == 0, "decline probe declines");
    CHECK(psx_cycle_count == before, "a decline must charge nothing");

    /* FO_CREDIT_SELF and 0 charge nothing from the tier. */
    before = psx_cycle_count;
    CHECK(consult(&cpu, 0x80006300u) == 1, "self-credit override handles");
    CHECK(psx_cycle_count == before, "SELF must not be charged by the tier");
    before = psx_cycle_count;
    CHECK(consult(&cpu, 0x80006400u) == 1, "zero-credit override handles");
    CHECK(psx_cycle_count == before, "credit 0 must charge nothing");

    /* The declared policy is inspectable, not just documented. */
    int seen40 = 0, seenself = 0;
    for (int i = 0; i < func_override_count(); i++) {
        char id[FO_MAX_ID];
        uint32_t a = 0;
        int32_t credit = -99;
        if (!func_override_get_ex(i, id, sizeof(id), &a, NULL, NULL, NULL,
                                  &credit))
            continue;
        if (a == 0x00006100u) { seen40   = (credit == 40); }
        if (a == 0x00006300u) { seenself = (credit == FO_CREDIT_SELF); }
    }
    CHECK(seen40, "get_ex must report the fixed credit");
    CHECK(seenself, "get_ex must report FO_CREDIT_SELF");
}

int main(void)
{
    test_install_is_null_until_registered();
    test_add_rejects_bad_input();
    test_duplicate_address_refused();
    test_handled_and_declined_both_count_as_consults();
    test_guard_declines_on_mismatch();
    test_call_original_is_one_shot();
    test_bypass_is_consumed_so_recursion_reconsults();
    test_call_original_outside_override_is_a_noop();
    test_package_reset_drops_only_package_entries();
    test_get_ex_bounded_id_copy();
    test_credit_policy();

    if (g_fail) {
        printf("func_override: FAILURES\n");
        return 1;
    }
    printf("func_override: all checks passed (%d entries registered)\n",
           func_override_count());
    return 0;
}
