#pragma once

#include <cstdint>
#include <cstring>

namespace PSXRecomp {
namespace GTE {

// PSX GTE FLAG register bit definitions
// Bit 31: Error summary (set if any of bits 30-23 or 18-13 are set)
// Bits 30-25: MAC1/2/3 positive/negative overflow (>43 bits)
// Bits 24-22: IR1/2/3 saturated
// Bits 21-19: Color R/G/B saturated
// Bit 18: SZ3/OTZ saturated
// Bit 17: Divide overflow
// Bit 16: MAC0 positive overflow
// Bit 15: MAC0 negative overflow
// Bits 14-13: SX2/SY2 saturated
// Bit 12: IR0 saturated
enum GTEFlags : uint32_t {
    FLAG_MAC1_POS  = 1u << 30,  FLAG_MAC2_POS  = 1u << 29,  FLAG_MAC3_POS  = 1u << 28,
    FLAG_MAC1_NEG  = 1u << 27,  FLAG_MAC2_NEG  = 1u << 26,  FLAG_MAC3_NEG  = 1u << 25,
    FLAG_IR1_SAT   = 1u << 24,  FLAG_IR2_SAT   = 1u << 23,  FLAG_IR3_SAT   = 1u << 22,
    FLAG_COLOR_R   = 1u << 21,  FLAG_COLOR_G   = 1u << 20,  FLAG_COLOR_B   = 1u << 19,
    FLAG_SZ3_OTZ   = 1u << 18,  FLAG_DIV_OVF   = 1u << 17,
    FLAG_MAC0_POS  = 1u << 16,  FLAG_MAC0_NEG  = 1u << 15,
    FLAG_SX2_SAT   = 1u << 14,  FLAG_SY2_SAT   = 1u << 13,
    FLAG_IR0_SAT   = 1u << 12,
    FLAG_ERROR_MASK = 0x7F87E000u,
    FLAG_ERROR_BIT  = 0x80000000u,
};

struct GTEState {
    // === Data Registers ===
    int16_t V0[3], V1[3], V2[3];       // Vertex vectors (s3.12)
    int32_t SXY[4];                      // Screen XY FIFO (packed: X low16, Y high16)
    uint16_t SZ[4];                      // Screen Z FIFO (unsigned 16-bit)
    uint32_t RGB[4];                     // RGB color FIFO (packed R,G,B,CD bytes)

    // === Control Registers (matrices, vectors, config) ===
    int16_t RT[3][3];                    // Rotation matrix (s3.12)
    int32_t TR[3];                       // Translation vector (32-bit)
    int16_t L[3][3];                     // Light source direction matrix (s3.12)
    int32_t BK[3];                       // Background color (32-bit)
    int16_t LC[3][3];                    // Light color matrix (s3.12) — replaces LR/LG/LB
    int32_t FC[3];                       // Far color (32-bit)
    int32_t OFX, OFY;                   // Screen offset (s15.16)
    uint16_t H;                          // Projection plane distance (unsigned 16-bit)
    int16_t DQA;                         // Depth queing coefficient A (signed 16-bit)
    int32_t DQB;                         // Depth queing coefficient B (signed 32-bit!)
    int16_t ZSF3, ZSF4;                 // Z-scale factors (signed 16-bit)

    // === Intermediate/Result Registers ===
    int16_t IR0, IR1, IR2, IR3;          // Intermediate results (signed 16-bit)
    int32_t MAC0, MAC1, MAC2, MAC3;      // Multiply-accumulate results (32-bit)
    uint16_t OTZ;                        // Ordering table Z (unsigned 16-bit)
    int32_t LZCS;                        // Leading zero count source (32-bit)
    int32_t LZCR;                        // Leading zero count result (32-bit)

    // === Flag Register ===
    uint32_t FLAG;

    GTEState() { std::memset(this, 0, sizeof(GTEState)); }

    // --- Saturation helpers ---

    int16_t saturate_ir(int32_t value, int ir_num, bool lm = false) {
        int32_t min_val = lm ? 0 : -0x8000;
        if (value < min_val) {
            FLAG |= (1u << (25 - ir_num)); // IR1=24, IR2=23, IR3=22
            return static_cast<int16_t>(min_val);
        }
        if (value > 0x7FFF) {
            FLAG |= (1u << (25 - ir_num));
            return 0x7FFF;
        }
        return static_cast<int16_t>(value);
    }

    int16_t saturate_ir0(int32_t value) {
        if (value < 0) { FLAG |= FLAG_IR0_SAT; return 0; }
        if (value > 0x1000) { FLAG |= FLAG_IR0_SAT; return 0x1000; }
        return static_cast<int16_t>(value);
    }

    uint8_t saturate_color(int32_t value, int component) {
        if (value < 0)   { FLAG |= (1u << (21 - component)); return 0; }
        if (value > 255) { FLAG |= (1u << (21 - component)); return 255; }
        return static_cast<uint8_t>(value);
    }

    uint16_t saturate_sz(int32_t value) {
        if (value < 0)      { FLAG |= FLAG_SZ3_OTZ; return 0; }
        if (value > 0xFFFF) { FLAG |= FLAG_SZ3_OTZ; return 0xFFFF; }
        return static_cast<uint16_t>(value);
    }

    void check_mac_overflow(int64_t value, int mac_num) {
        // mac_num: 1,2,3
        if (value > 0x7FFFFFFFFLL)  FLAG |= (1u << (31 - mac_num)); // MAC1=30, MAC2=29, MAC3=28
        if (value < -0x800000000LL) FLAG |= (1u << (28 - mac_num)); // MAC1=27, MAC2=26, MAC3=25
    }

    void check_mac0_overflow(int64_t value) {
        if (value > 0x7FFFFFFFLL)  FLAG |= FLAG_MAC0_POS;
        if (value < -0x80000000LL) FLAG |= FLAG_MAC0_NEG;
    }

    void push_sxy(int64_t sx, int64_t sy) {
        // Saturate to screen coordinates and set flags
        int16_t sx_sat, sy_sat;
        if (sx < -0x400)     { sx_sat = -0x400; FLAG |= FLAG_SX2_SAT; }
        else if (sx > 0x3FF) { sx_sat = 0x3FF;  FLAG |= FLAG_SX2_SAT; }
        else                 { sx_sat = static_cast<int16_t>(sx); }
        if (sy < -0x400)     { sy_sat = -0x400; FLAG |= FLAG_SY2_SAT; }
        else if (sy > 0x3FF) { sy_sat = 0x3FF;  FLAG |= FLAG_SY2_SAT; }
        else                 { sy_sat = static_cast<int16_t>(sy); }
        // Push FIFO
        SXY[0] = SXY[1]; SXY[1] = SXY[2];
        SXY[2] = (static_cast<uint32_t>(sy_sat & 0xFFFF) << 16) |
                  (static_cast<uint32_t>(sx_sat & 0xFFFF));
        SXY[3] = SXY[2];
    }

    void push_sz(int32_t value) {
        SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
        SZ[3] = saturate_sz(value);
    }

    void push_rgb(uint8_t r, uint8_t g, uint8_t b) {
        uint8_t cd = (RGB[0] >> 24) & 0xFF; // Code byte from RGBC
        RGB[0] = RGB[1]; RGB[1] = RGB[2];
        RGB[2] = (cd << 24) | (b << 16) | (g << 8) | r;
    }

    void set_error_flag() {
        if (FLAG & FLAG_ERROR_MASK) FLAG |= FLAG_ERROR_BIT;
    }
};

// === GTE Operations ===
void gte_rtps_internal(GTEState* gte, int16_t* V, bool setMac0);
void gte_rtps(GTEState* gte, uint32_t instr);
void gte_rtpt(GTEState* gte, uint32_t instr);
void gte_nclip(GTEState* gte, uint32_t instr);
void gte_avsz3(GTEState* gte, uint32_t instr);
void gte_avsz4(GTEState* gte, uint32_t instr);
void gte_nccs_internal(GTEState* gte, int16_t* V);
void gte_nccs(GTEState* gte, uint32_t instr);
void gte_ncct(GTEState* gte, uint32_t instr);
void gte_ncds_internal(GTEState* gte, int16_t* V);
void gte_ncds(GTEState* gte, uint32_t instr);
void gte_ncdt(GTEState* gte, uint32_t instr);
void gte_ncs_internal(GTEState* gte, int16_t* V);
void gte_ncs(GTEState* gte, uint32_t instr);
void gte_nct(GTEState* gte, uint32_t instr);
void gte_dpcs(GTEState* gte, uint32_t instr);
void gte_dpct(GTEState* gte, uint32_t instr);
void gte_dpcl(GTEState* gte, uint32_t instr);
void gte_intpl(GTEState* gte, uint32_t instr);
void gte_cdp(GTEState* gte, uint32_t instr);
void gte_cc(GTEState* gte, uint32_t instr);
void gte_mvmva(GTEState* gte, uint32_t instr);
void gte_op(GTEState* gte, uint32_t instr);
void gte_sqr(GTEState* gte, uint32_t instr);
void gte_gpf(GTEState* gte, uint32_t instr);
void gte_gpl(GTEState* gte, uint32_t instr);

// Register transfer
void gte_mtc2(GTEState* gte, uint8_t reg, uint32_t value);
uint32_t gte_mfc2(GTEState* gte, uint8_t reg);
void gte_ctc2(GTEState* gte, uint8_t reg, uint32_t value);
uint32_t gte_cfc2(GTEState* gte, uint8_t reg);

} // namespace GTE
} // namespace PSXRecomp
