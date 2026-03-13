#include "annotations.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace PSXRecomp {

const std::string AnnotationTable::empty_;

static const char* ltrim(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

bool AnnotationTable::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        line[strcspn(line, "\r\n")] = '\0';
        const char* p = ltrim(line);
        if (!*p || *p == '#') continue;

        // Parse address
        char* end;
        uint32_t addr = (uint32_t)strtoul(p, &end, 0);
        if (end == p || *end != ',') continue;
        p = ltrim(end + 1);

        // Remainder is the note (may contain commas)
        if (!*p) continue;

        table_[addr] = std::string(p);
    }

    fclose(f);
    return !table_.empty();
}

const std::string& AnnotationTable::lookup(uint32_t addr) const {
    auto it = table_.find(addr);
    if (it != table_.end()) return it->second;
    return empty_;
}

} // namespace PSXRecomp
