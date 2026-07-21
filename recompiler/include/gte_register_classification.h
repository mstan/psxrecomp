#pragma once

#include <cstdint>

/* Single source of truth for COP2 accesses that cannot use generated raw
 * CPUState-array fast paths.  Both emitters must agree when execution crosses
 * between static code, overlay shards, and the interpreter. */
namespace PSXRecompGTERegisters {

constexpr bool data_read_needs_helper(uint8_t reg) {
    return (reg >= 8 && reg <= 11) || reg == 15 || reg == 23 ||
           reg == 28 || reg == 29 || reg == 31;
}

constexpr bool data_write_needs_helper(uint8_t reg) {
    return reg == 1 || reg == 3 || reg == 5 ||
           (reg >= 7 && reg <= 19) || reg == 23 ||
           reg == 28 || reg == 29 || reg == 30 || reg == 31;
}

constexpr bool ctrl_read_needs_helper(uint8_t reg) {
    return reg == 26 || reg == 27 || reg == 29 || reg == 30 || reg == 31;
}

constexpr bool ctrl_write_needs_helper(uint8_t reg) {
    return reg == 4 || reg == 12 || reg == 20 || reg == 26 ||
           reg == 27 || reg == 29 || reg == 30 || reg == 31;
}

constexpr uint32_t helper_mask(bool (*predicate)(uint8_t)) {
    uint32_t mask = 0;
    for (uint8_t reg = 0; reg < 32; ++reg)
        if (predicate(reg)) mask |= uint32_t{1} << reg;
    return mask;
}

/* Independent architectural expectations. These deliberately do not derive
 * from one another: changing a predicate requires an explicit review of the
 * register mask, rather than letting both emitter tests agree on a bad table. */
static_assert(helper_mask(data_read_needs_helper)  == 0xB0808F00u,
              "GTE data-read helper set changed");
static_assert(helper_mask(data_write_needs_helper) == 0xF08FFFAAu,
              "GTE data-write helper set changed");
static_assert(helper_mask(ctrl_read_needs_helper)  == 0xEC000000u,
              "GTE control-read helper set changed");
static_assert(helper_mask(ctrl_write_needs_helper) == 0xEC101010u,
              "GTE control-write helper set changed");

} // namespace PSXRecompGTERegisters
