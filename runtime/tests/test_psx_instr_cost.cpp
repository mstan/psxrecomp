#include "psx_instr_cost.h"

#include <cstdint>
#include <cstdio>

static uint32_t reference_dep_res_mask(uint32_t insn) {
    const uint32_t op = insn >> 26;
    const uint32_t rs = (insn >> 21) & 0x1Fu;
    const uint32_t rt = (insn >> 16) & 0x1Fu;
    const uint32_t rd = (insn >> 11) & 0x1Fu;
    const uint32_t m_rs = 1u << rs;
    const uint32_t m_rt = 1u << rt;
    const uint32_t m_rd = 1u << rd;
    switch (op) {
    case 0x00:
        switch (insn & 0x3Fu) {
        case 0x00: case 0x02: case 0x03: return m_rt | m_rd;
        case 0x04: case 0x06: case 0x07: return m_rs | m_rt | m_rd;
        case 0x08: case 0x09: return m_rs | m_rd;
        case 0x10: case 0x12: return m_rd;
        case 0x11: case 0x13: return m_rs;
        case 0x18: case 0x19: case 0x1A: case 0x1B: return m_rs | m_rt;
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x2A: case 0x2B: return m_rs | m_rt | m_rd;
        default: return 0u;
        }
    case 0x01:
        return m_rs | (((rt & 0x1Eu) == 0x10u) ? (1u << 31) : 0u);
    case 0x02: return 0u;
    case 0x03: return 1u << 31;
    case 0x04: case 0x05: return m_rs | m_rt;
    case 0x06: case 0x07: return m_rs;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: return m_rs | m_rt;
    case 0x0F: return m_rt;
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: return m_rs;
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2E: return m_rs | m_rt;
    default: return 0u;
    }
}

int main() {
    for (uint32_t op = 0; op < 64u; ++op) {
        const uint32_t function_count = (op == 0u) ? 64u : 1u;
        for (uint32_t fn = 0; fn < function_count; ++fn) {
            for (uint32_t rs = 0; rs < 32u; ++rs) {
                for (uint32_t rt = 0; rt < 32u; ++rt) {
                    for (uint32_t rd = 0; rd < 32u; ++rd) {
                        const uint32_t insn = (op << 26) | (rs << 21) |
                                              (rt << 16) | (rd << 11) | fn;
                        const uint32_t expected = reference_dep_res_mask(insn);
                        const uint32_t actual = psx_cyc_dep_res_mask(insn);
                        if (actual != expected) {
                            std::fprintf(stderr,
                                         "dep/res mismatch insn=%08x expected=%08x actual=%08x\n",
                                         insn, expected, actual);
                            return 1;
                        }
                    }
                }
            }
        }
    }
    std::puts("PASS: table-driven dependency masks match the frozen decoder");
    return 0;
}
