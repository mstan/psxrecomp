#include "iso_reader.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

static int failures;

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

static void write_file(const std::filesystem::path& path,
                       const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

int main() {
    const auto root = std::filesystem::current_path() / "iso_reader_sbi_scratch";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto image = root / "protected.bin";
    write_file(image, std::vector<uint8_t>(2352 * 20));

    const std::array<uint8_t, 10> replacement = {
        0x41, 0x01, 0x01, 0x23, 0x06, 0x05, 0x00, 0x03, 0x08, 0x01};
    // SBI positions are absolute MSF. 00:02:00 is disc LBA zero after the
    // standard 150-frame lead-in.
    std::vector<uint8_t> sbi = {'S', 'B', 'I', 0, 0x00, 0x02, 0x00, 0x01};
    sbi.insert(sbi.end(), replacement.begin(), replacement.end());
    write_file(root / "protected.sbi", sbi);

    PS1::ISOReader reader;
    check(reader.Open(image.string()), "valid same-basename SBI mounts");
    std::array<uint8_t, 12> subq = {};
    bool valid = true;
    check(reader.ReadSubChannelQ(0, subq.data(), &valid),
          "replacement sub-Q can be read");
    check(!valid, "SBI replacement is reported with intentionally invalid CRC");
    check(std::equal(replacement.begin(), replacement.end(), subq.begin()),
          "SBI replacement payload is preserved");

    check(reader.ReadSubChannelQ(1, subq.data(), &valid),
          "ordinary sub-Q can be generated");
    check(valid, "ordinary generated sub-Q has valid CRC");
    check(subq[0] == 0x41 && subq[1] == 0x01 && subq[2] == 0x01,
          "generated data-track identity is correct");
    check(subq[6] == 0x00 && subq[7] == 0x02 && subq[8] == 0x01,
          "generated absolute position includes the 150-sector lead-in");
    reader.Close();

    write_file(root / "protected.sbi", {'B', 'A', 'D'});
    check(!reader.Open(image.string()), "malformed SBI fails the mount closed");

    std::filesystem::remove(root / "protected.sbi");
    check(reader.Open(image.string()), "image without SBI still mounts");
    check(!reader.HasSubChannelReplacements(),
          "image without SBI keeps the compatibility path inactive");
    check(reader.ReadSubChannelQ(0, subq.data(), &valid) && valid,
          "image without SBI generates valid sub-Q");
    reader.Close();
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
