#pragma once
/*
 * annotations.hpp — PS1 address annotation table
 *
 * Generic mechanism: loads a CSV file of (address, note) tuples and makes them
 * available to the code generator for inline comment emission.
 *
 * The CSV is game-specific data; this module is game-agnostic.
 * The code generator calls annotation_lookup(addr) at each function emit point
 * and prepends the result as a C block comment if present.
 *
 * CSV format (one entry per line):
 *   # comment lines and blank lines are ignored
 *   0x8001dfd4, entity tick dispatcher — iterates 200 entity slots
 *
 *   address: hex with 0x prefix (e.g. 0x8001dfd4) or decimal
 *   note:    free text; may contain commas; appears as [NOTE] in generated C
 *
 * Convention: place annotation files at annotations/<exe_stem>_annotations.csv
 * in the project root. The recompiler looks for this path relative to CWD.
 */

#include <cstdint>
#include <string>
#include <unordered_map>

namespace PSXRecomp {

class AnnotationTable {
public:
    // Load CSV from path. Silently succeeds with empty table if file not found.
    // Returns true if any entries were loaded.
    bool load(const char* csv_path);

    // Returns the note string for addr, or empty string if no entry exists.
    const std::string& lookup(uint32_t addr) const;

    int count() const { return static_cast<int>(table_.size()); }

private:
    std::unordered_map<uint32_t, std::string> table_;
    static const std::string empty_;
};

} // namespace PSXRecomp
