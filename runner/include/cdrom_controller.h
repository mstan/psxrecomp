#pragma once
#include <vector>
#include <cstdint>
#include <string>

namespace PS1 {
// Stub CDROMController — not needed until overlay loading is implemented.
class CDROMController {
public:
    virtual ~CDROMController() = default;
    virtual std::vector<uint8_t> ReadFile(const std::string& path) { return {}; }
};
} // namespace PS1
