// bios_address_model.cpp — see bios_address_model.h for the design contract.

#include "bios_address_model.h"

#include <stdexcept>

#include "fmt/format.h"

#include "config_loader.h"

namespace PSXRecompV4 {

// ---------------------------------------------------------------------------
// Construction + invariants
// ---------------------------------------------------------------------------

BiosAddressModel BiosAddressModel::from_config(const BiosConfig& cfg) {
    BiosAddressModel m;
    m.copies_        = cfg.address_copies;
    m.install_slots_ = cfg.install_slots;
    m.rom_base_phys_ = cfg.load_address & 0x1FFFFFFFu;
    m.rom_size_      = cfg.text_size;

    const std::string where = cfg.config_path.string();

    auto fail = [&](const std::string& msg) {
        throw std::runtime_error(fmt::format(
            "{}: [recompiler.address_model] {}", where, msg));
    };

    for (size_t i = 0; i < m.copies_.size(); ++i) {
        const BiosAddrCopy& c = m.copies_[i];
        if (c.rom_lo >= c.rom_hi)
            fail(fmt::format("copy '{}': rom_lo >= rom_hi", c.name));
        if ((c.rom_lo | c.rom_hi | c.ram_lo | c.runtime_base) & 3u)
            fail(fmt::format("copy '{}': bounds must be 4-aligned", c.name));
        if (c.rom_lo < m.rom_base_phys_ ||
            c.rom_hi > m.rom_base_phys_ + m.rom_size_)
            fail(fmt::format("copy '{}': ROM window outside the image "
                             "[0x{:08X},0x{:08X})",
                             c.name, m.rom_base_phys_,
                             m.rom_base_phys_ + m.rom_size_));
        if ((c.runtime_base & 0x1FFFFFFFu) != c.ram_lo)
            fail(fmt::format("copy '{}': runtime_base must be a vaddr of "
                             "ram_lo (phys mismatch)", c.name));

        // Disjoint input windows: a ROM byte belongs to at most one copy, and
        // a RAM byte to at most one destination ("duplicate logical function
        // -> refuse to build", docs/RELOCATION_MANIFEST_FORMAT.md).
        for (size_t j = 0; j < i; ++j) {
            const BiosAddrCopy& p = m.copies_[j];
            if (c.rom_lo < p.rom_hi && p.rom_lo < c.rom_hi)
                fail(fmt::format("copies '{}' and '{}': ROM windows overlap",
                                 p.name, c.name));
            if (c.ram_lo < p.ram_hi() && p.ram_lo < c.ram_hi())
                fail(fmt::format("copies '{}' and '{}': RAM windows overlap",
                                 p.name, c.name));
        }
    }

    // normalize() is emitted as SEQUENTIAL ifs with no else; that is only
    // equivalent to first-match-wins if no copy's fold OUTPUT can land inside
    // a later copy's fold INPUT. Check every pair, both key directions.
    for (const BiosAddrCopy& a : m.copies_) {
        const uint32_t out_lo = a.key_is_ram ? a.ram_lo : a.rom_lo;
        const uint32_t out_hi = a.key_is_ram ? a.ram_hi() : a.rom_hi;
        for (const BiosAddrCopy& b : m.copies_) {
            if (&a == &b) continue;
            const uint32_t in_lo = b.key_is_ram ? b.rom_lo : b.ram_lo;
            const uint32_t in_hi = b.key_is_ram ? b.rom_hi : b.ram_hi();
            if (out_lo < in_hi && in_lo < out_hi)
                fail(fmt::format(
                    "copy '{}' fold output [0x{:08X},0x{:08X}) intersects "
                    "copy '{}' fold input [0x{:08X},0x{:08X}) — sequential "
                    "normalize() would double-fold",
                    a.name, out_lo, out_hi, b.name, in_lo, in_hi));
        }
    }

    for (size_t i = 0; i < m.copies_.size(); ++i) {
        if (m.copies_[i].kernel_bless) {
            if (m.kbless_idx_ >= 0)
                fail("at most one copy may set kernel_bless = true (the "
                     "runtime models a single bless window)");
            if (!m.copies_[i].key_is_ram)
                fail(fmt::format("copy '{}': kernel_bless requires "
                                 "dispatch_key = \"ram\"", m.copies_[i].name));
            m.kbless_idx_ = static_cast<int>(i);
        }
        if (!m.copies_[i].key_is_ram && m.rom_keyed_idx_ < 0)
            m.rom_keyed_idx_ = static_cast<int>(i);
    }

    for (uint32_t slot : m.install_slots_) {
        if (slot & 3u)
            fail(fmt::format("install slot 0x{:08X} must be 4-aligned", slot));
    }

    return m;
}

// ---------------------------------------------------------------------------
// Address mapping
// ---------------------------------------------------------------------------

uint32_t BiosAddressModel::normalize(uint32_t addr) const {
    uint32_t phys = addr & 0x1FFFFFFFu;
    // The 2 MiB main RAM is mirrored four times across the first 8 MiB of
    // physical address space. Dispatch keys must use the same hardware alias
    // as ordinary memory reads, or a valid call through 0x80200000+
    // incorrectly becomes an unmapped-PC failure.
    if (phys < 0x00800000u)
        phys &= 0x001FFFFFu;
    // Sequential per-copy folds, same shape as the emitted normalize() —
    // equivalence to first-match-wins is enforced by from_config.
    for (const BiosAddrCopy& c : copies_) {
        if (c.key_is_ram) {
            if (phys >= c.rom_lo && phys < c.rom_hi)
                phys = phys - c.rom_lo + c.ram_lo;
        } else {
            if (phys >= c.ram_lo && phys < c.ram_hi())
                phys = phys - c.ram_lo + c.rom_lo;
        }
    }
    return phys;
}

uint32_t BiosAddressModel::ram_alias_to_rom(uint32_t addr) const {
    uint32_t phys = addr & 0x1FFFFFFFu;
    for (const BiosAddrCopy& c : copies_) {
        if (phys >= c.ram_lo && phys < c.ram_hi())
            return (0xA0000000u | c.rom_lo) + (phys - c.ram_lo);
    }
    return addr;
}

uint32_t BiosAddressModel::runtime_pc(uint32_t rom_pc) const {
    uint32_t phys = rom_pc & 0x1FFFFFFFu;
    for (const BiosAddrCopy& c : copies_) {
        if (phys >= c.rom_lo && phys < c.rom_hi)
            return c.runtime_base + (phys - c.rom_lo);
    }
    return rom_pc;
}

uint32_t BiosAddressModel::relocate_j_target(uint32_t rom_addr, uint32_t target) const {
    uint32_t phys = rom_addr & 0x1FFFFFFFu;
    for (const BiosAddrCopy& c : copies_) {
        if (phys >= c.rom_lo && phys < c.rom_hi) {
            uint32_t runtime_addr = c.runtime_base + (phys - c.rom_lo);
            return (runtime_addr & 0xF0000000u) | (target & 0x0FFFFFFFu);
        }
    }
    return target;
}

uint32_t BiosAddressModel::rom_to_ram_phys(uint32_t rom_addr) const {
    uint32_t phys = rom_addr & 0x1FFFFFFFu;
    for (const BiosAddrCopy& c : copies_) {
        if (c.key_is_ram && phys >= c.rom_lo && phys < c.rom_hi)
            return phys - c.rom_lo + c.ram_lo;
    }
    return phys;
}

uint32_t BiosAddressModel::remap_relocated_j_target(uint32_t target,
                                                    uint32_t source_rom_addr) const {
    uint32_t src_phys = source_rom_addr & 0x1FFFFFFFu;
    for (const BiosAddrCopy& c : copies_) {
        if (src_phys < c.rom_lo || src_phys >= c.rom_hi) continue;

        // The 26-bit target inherits the RUNTIME region of the source.
        uint32_t runtime_addr = c.runtime_base + (src_phys - c.rom_lo);
        uint32_t ram_target = (runtime_addr & 0xF0000000u) | (target & 0x0FFFFFFFu);
        uint32_t ram_phys = ram_target & 0x1FFFFFFFu;

        // Fold back to a followable KSEG1 ROM address through any copy's RAM
        // window.
        for (const BiosAddrCopy& c2 : copies_) {
            if (ram_phys >= c2.ram_lo && ram_phys < c2.ram_hi())
                return (0xA0000000u | c2.rom_lo) + (ram_phys - c2.ram_lo);
        }
        // Target already inside the ROM image: return it in KSEG1 form.
        if (ram_phys >= rom_base_phys_ && ram_phys < rom_base_phys_ + rom_size_)
            return 0xA0000000u | ram_phys;
        return target;
    }
    return target;
}

// ---------------------------------------------------------------------------
// kbless / install slots / rom-keyed window accessors
// ---------------------------------------------------------------------------

uint32_t BiosAddressModel::kbless_ram_lo() const { return copies_[kbless_idx_].ram_lo; }
uint32_t BiosAddressModel::kbless_ram_hi() const { return copies_[kbless_idx_].ram_hi(); }
uint32_t BiosAddressModel::kbless_rom_off() const {
    return copies_[kbless_idx_].rom_lo - rom_base_phys_;
}
bool BiosAddressModel::in_kbless(uint32_t norm) const {
    return has_kbless() && norm >= kbless_ram_lo() && norm < kbless_ram_hi();
}

bool BiosAddressModel::is_install_slot(uint32_t ram_pc) const {
    for (uint32_t slot : install_slots_) {
        if (ram_pc == slot) return true;
    }
    return false;
}

uint32_t BiosAddressModel::rom_keyed_ram_lo() const {
    return copies_[rom_keyed_idx_].ram_lo;
}
uint32_t BiosAddressModel::rom_keyed_ram_hi_incl() const {
    return copies_[rom_keyed_idx_].ram_hi() - 1u;
}

// ---------------------------------------------------------------------------
// Emitted-C text
// ---------------------------------------------------------------------------

std::string BiosAddressModel::emit_normalize_c() const {
    std::string out;
    out += "static uint32_t normalize(uint32_t addr) {\n";
    out += "    uint32_t phys = addr & 0x1FFFFFFFu;\n";
    out += "    /* PSX main RAM: 2 MiB mirrored through physical 0x007FFFFF. */\n";
    out += "    if (phys < 0x00800000u) phys &= 0x001FFFFFu;\n";
    for (const BiosAddrCopy& c : copies_) {
        if (c.key_is_ram) {
            out += fmt::format("    /* {}: ROM 0x{:X}+ -> RAM 0x{:X}+ */\n",
                               c.name, c.rom_lo, c.ram_lo);
            out += fmt::format("    if (phys >= 0x{:08X}u && phys <= 0x{:08X}u)\n",
                               c.rom_lo, c.rom_hi - 1u);
            out += fmt::format("        phys = phys - 0x{:08X}u + 0x{:08X}u;\n",
                               c.rom_lo, c.ram_lo);
        } else {
            out += fmt::format("    /* {}: RAM 0x{:X}+ -> ROM physical 0x{:X}+ */\n",
                               c.name, c.ram_lo, c.rom_lo);
            out += fmt::format("    if (phys >= 0x{:08X}u && phys <= 0x{:08X}u)\n",
                               c.ram_lo, c.ram_hi() - 1u);
            out += fmt::format("        phys = phys - 0x{:08X}u + 0x{:08X}u;\n",
                               c.ram_lo, c.rom_lo);
        }
    }
    out += "    return phys;\n";
    out += "}\n\n";
    return out;
}

} // namespace PSXRecompV4
