// bios_address_model_test.cpp — proves the profile-driven BiosAddressModel is
// function-level equivalent to the hardcoded SCPH1001 helpers it replaced.
//
// The byte-identity check on the regenerated SCPH1001_*.c is artifact-level
// evidence; this test is the function-level version: every 4-byte address in
// the RAM and ROM ranges is swept through the model built from
// bios/SCPH1001.toml and compared against verbatim copies of the pre-refactor
// implementations (full_function_emitter.cpp / function_discovery.cpp as of
// the commit this refactor replaced). It also exercises the from_config
// invariants.
//
// Usage: bios_address_model_test [path/to/SCPH1001.toml]
//   default: bios/SCPH1001.toml relative to the CWD (run from framework root)

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "../src/bios_address_model.h"
#include "../src/config_loader.h"

using PSXRecompV4::BiosAddressModel;
using PSXRecompV4::BiosAddrCopy;
using PSXRecompV4::BiosConfig;

// ---------------------------------------------------------------------------
// Reference implementations: verbatim pre-refactor SCPH1001 hardcodes.
// ---------------------------------------------------------------------------

static uint32_t ref_normalize(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
        phys = phys - 0x1FC10000u + 0x00000500u;
    }
    if (phys >= 0x00030000u && phys <= 0x0005AFFFu) {
        phys = phys - 0x00030000u + 0x1FC18000u;
    }
    return phys;
}

static uint32_t ref_ram_alias_to_rom(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x00000500u && phys < 0x00008500u) {
        return 0xBFC10000u + (phys - 0x00000500u);
    }
    if (phys >= 0x00030000u && phys <= 0x0005AFFFu) {
        return 0xBFC18000u + (phys - 0x00030000u);
    }
    return addr;
}

static uint32_t ref_runtime_pc(uint32_t rom_pc) {
    uint32_t phys = rom_pc & 0x1FFFFFFFu;
    if (phys >= 0x1FC18000u && phys <= 0x1FC42FFFu) {
        return 0x80030000u + (phys - 0x1FC18000u);
    }
    if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
        return phys - 0x1FC10000u + 0x00000500u;
    }
    return rom_pc;
}

static uint32_t ref_relocate_j_target(uint32_t rom_addr, uint32_t target) {
    uint32_t phys = rom_addr & 0x1FFFFFFFu;
    if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
        uint32_t runtime_addr = phys - 0x1FC10000u + 0x00000500u;
        return (runtime_addr & 0xF0000000u) | (target & 0x0FFFFFFFu);
    }
    if (phys >= 0x1FC18000u && phys <= 0x1FC42FFFu) {
        uint32_t runtime_addr = 0x80030000u + (phys - 0x1FC18000u);
        return (runtime_addr & 0xF0000000u) | (target & 0x0FFFFFFFu);
    }
    return target;
}

static uint32_t ref_rom_to_ram_phys(uint32_t rom_addr) {
    uint32_t phys = rom_addr & 0x1FFFFFFFu;
    if (phys >= 0x1FC10000u && phys <= 0x1FC17FFFu) {
        return phys - 0x1FC10000u + 0x00000500u;
    }
    return phys;
}

// ---------------------------------------------------------------------------

static int g_failures = 0;

static void expect_eq(uint32_t got, uint32_t want, const char* what, uint32_t at) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s(0x%08X): got 0x%08X want 0x%08X\n",
                     what, at, got, want);
        if (++g_failures > 20) {
            std::fprintf(stderr, "too many failures, aborting\n");
            std::exit(1);
        }
    }
}

static void expect_throw(const char* what, void (*fn)(BiosConfig&), BiosConfig base) {
    try {
        fn(base);
        BiosAddressModel::from_config(base);
        std::fprintf(stderr, "FAIL invariant '%s': expected from_config to throw\n", what);
        g_failures++;
    } catch (const std::runtime_error&) {
        /* expected */
    }
}

static BiosConfig minimal_cfg() {
    BiosConfig cfg{};
    cfg.config_path  = "test://synthetic";
    cfg.load_address = 0xBFC00000u;
    cfg.text_size    = 0x80000u;
    return cfg;
}

static BiosAddrCopy mk(const char* name, uint32_t rom_lo, uint32_t rom_hi,
                       uint32_t ram_lo, uint32_t runtime_base,
                       bool key_is_ram, bool bless) {
    BiosAddrCopy c;
    c.name = name; c.rom_lo = rom_lo; c.rom_hi = rom_hi; c.ram_lo = ram_lo;
    c.runtime_base = runtime_base; c.key_is_ram = key_is_ram;
    c.kernel_bless = bless;
    return c;
}

int main(int argc, char** argv) {
    const std::string profile = argc > 1 ? argv[1] : "bios/SCPH1001.toml";

    BiosAddressModel model;
    try {
        const auto cfg = PSXRecompV4::load_bios_config(profile);
        model = BiosAddressModel::from_config(cfg);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "cannot load %s: %s\n(run from the framework root)\n",
                     profile.c_str(), ex.what());
        return 2;
    }

    // --- Sweep: RAM range + ROM range, every 4 bytes, all helpers ---------
    auto sweep = [&](uint32_t lo, uint32_t hi) {
        for (uint64_t a = lo; a < hi; a += 4) {
            const uint32_t addr = static_cast<uint32_t>(a);
            expect_eq(model.normalize(addr),        ref_normalize(addr),        "normalize", addr);
            expect_eq(model.ram_alias_to_rom(addr), ref_ram_alias_to_rom(addr), "ram_alias_to_rom", addr);
            expect_eq(model.runtime_pc(addr),       ref_runtime_pc(addr),       "runtime_pc", addr);
            expect_eq(model.rom_to_ram_phys(addr),  ref_rom_to_ram_phys(addr),  "rom_to_ram_phys", addr);
            // J-target relocation: source sweep with two representative
            // 26-bit targets (one per runtime region shape).
            expect_eq(model.relocate_j_target(addr, 0x0FC12344u),
                      ref_relocate_j_target(addr, 0x0FC12344u), "relocate_j(a)", addr);
            expect_eq(model.relocate_j_target(addr, 0x00030010u),
                      ref_relocate_j_target(addr, 0x00030010u), "relocate_j(b)", addr);
        }
    };
    sweep(0x00000000u, 0x00200000u);            // RAM (2 MiB)
    sweep(0x1FC00000u, 0x1FC80000u);            // BIOS ROM phys
    sweep(0xBFC00000u, 0xBFC80000u);            // BIOS ROM KSEG1 (mask path)
    sweep(0x80000000u, 0x80200000u);            // RAM KSEG0 (mask path)

    // Main RAM aliases cover the first 8 MiB of physical space. Dispatch
    // normalization must agree with memory access normalization at every
    // mirror, including the exact 0x80200000 boundary seen in indirect calls.
    expect_eq(model.normalize(0x00200000u), 0x00000000u, "normalize.ram_mirror", 0x00200000u);
    expect_eq(model.normalize(0x007FFFFCu), 0x001FFFFCu, "normalize.ram_mirror", 0x007FFFFCu);
    expect_eq(model.normalize(0x80200000u), 0x00000000u, "normalize.ram_mirror", 0x80200000u);
    expect_eq(model.normalize(0xA07FFFFCu), 0x001FFFFCu, "normalize.ram_mirror", 0xA07FFFFCu);

    // --- kbless / rom-keyed accessors match the SCPH1001 constants --------
    if (!model.has_kbless()) { std::fprintf(stderr, "FAIL: no kbless window\n"); g_failures++; }
    expect_eq(model.kbless_ram_lo(),  0x500u,   "kbless_ram_lo", 0);
    expect_eq(model.kbless_ram_hi(),  0x8500u,  "kbless_ram_hi", 0);
    expect_eq(model.kbless_rom_off(), 0x10000u, "kbless_rom_off", 0);
    expect_eq(model.rom_keyed_ram_lo(),      0x30000u, "rom_keyed_ram_lo", 0);
    expect_eq(model.rom_keyed_ram_hi_incl(), 0x5AFFFu, "rom_keyed_ram_hi_incl", 0);
    if (!model.is_install_slot(0xCF0u)) { std::fprintf(stderr, "FAIL: 0xCF0 not an install slot\n"); g_failures++; }
    if (model.is_install_slot(0xCF4u))  { std::fprintf(stderr, "FAIL: 0xCF4 claims install slot\n"); g_failures++; }

    // --- from_config invariants refuse bad profiles -----------------------
    expect_throw("overlapping ROM windows", [](BiosConfig& c) {
        c.address_copies = {
            mk("a", 0x1FC10000u, 0x1FC18000u, 0x500u,   0x500u,      true,  false),
            mk("b", 0x1FC14000u, 0x1FC43000u, 0x30000u, 0x80030000u, false, false),
        };
    }, minimal_cfg());
    expect_throw("two bless windows", [](BiosConfig& c) {
        c.address_copies = {
            mk("a", 0x1FC10000u, 0x1FC14000u, 0x500u,  0x500u,  true, true),
            mk("b", 0x1FC14000u, 0x1FC18000u, 0x9000u, 0x9000u, true, true),
        };
    }, minimal_cfg());
    expect_throw("misaligned bounds", [](BiosConfig& c) {
        c.address_copies = {
            mk("a", 0x1FC10002u, 0x1FC18000u, 0x500u, 0x500u, true, false),
        };
    }, minimal_cfg());
    expect_throw("ROM window outside image", [](BiosConfig& c) {
        c.address_copies = {
            mk("a", 0x1FB00000u, 0x1FB10000u, 0x500u, 0x500u, true, false),
        };
    }, minimal_cfg());
    expect_throw("runtime_base phys mismatch", [](BiosConfig& c) {
        c.address_copies = {
            mk("a", 0x1FC10000u, 0x1FC18000u, 0x500u, 0x80000600u, true, false),
        };
    }, minimal_cfg());
    expect_throw("bless on rom-keyed copy", [](BiosConfig& c) {
        c.address_copies = {
            mk("a", 0x1FC18000u, 0x1FC43000u, 0x30000u, 0x80030000u, false, true),
        };
    }, minimal_cfg());
    expect_throw("overlapping RAM windows / double-fold", [](BiosConfig& c) {
        // a folds ROM->RAM into [0x500,0x8500); b (rom-keyed) folds RAM
        // [0x500,...) back out — sequential normalize() would double-fold
        // a's output. Refused (as overlapping RAM destinations).
        c.address_copies = {
            mk("a", 0x1FC10000u, 0x1FC18000u, 0x500u, 0x500u, true,  false),
            mk("b", 0x1FC20000u, 0x1FC28000u, 0x500u, 0x500u, false, false),
        };
    }, minimal_cfg());

    // --- empty model degenerates to the KSEG mask -------------------------
    {
        BiosAddressModel empty;
        expect_eq(empty.normalize(0xBFC12345u), 0x1FC12345u, "empty.normalize", 0xBFC12345u);
        expect_eq(empty.runtime_pc(0xBFC12344u), 0xBFC12344u, "empty.runtime_pc", 0xBFC12344u);
        if (empty.has_kbless() || empty.has_rom_keyed_ram_window()) {
            std::fprintf(stderr, "FAIL: empty model claims windows\n");
            g_failures++;
        }
        const std::string n = empty.emit_normalize_c();
        if (n.find("0x1FC1") != std::string::npos) {
            std::fprintf(stderr, "FAIL: empty model emits copy folds\n");
            g_failures++;
        }
    }

    if (g_failures == 0) {
        std::printf("bios_address_model_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "bios_address_model_test: %d failure(s)\n", g_failures);
    return 1;
}
