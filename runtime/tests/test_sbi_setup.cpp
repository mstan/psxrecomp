#include "sbi_setup.h"
#include <cassert>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    const auto data = root / "source.bin";
    const auto cue = root / "selected.cue";
    const auto sbi = root / "selected.sbi";
    { std::ofstream file(data); file << "source-owned-disc"; }
    { std::ofstream file(sbi, std::ios::binary); file << "source-owned-companion"; }
    const auto data_hash = PSXRecompV4::sbi_setup_hash(data);
    const auto sbi_hash = PSXRecompV4::sbi_setup_hash(sbi);
    const PSXRecompV4::SbiRequirement rule = {
        std::filesystem::file_size(data), data_hash.c_str(), sbi_hash.c_str(), "Fixture", "TEST-00000"};
    auto check = [&]() { return PSXRecompV4::check_sbi_setup(data, cue, &rule, 1); };
    assert(check().ready && check().required);
    std::filesystem::remove(sbi);
    auto missing = check();
    assert(missing.required && !missing.ready);
    assert(missing.message.find("SBI file is missing") != std::string::npos);
    assert(missing.message.find(sbi.string()) != std::string::npos);
    assert(missing.message.find("TEST-00000") != std::string::npos);
    { std::ofstream file(root / "source.sbi"); file << "source-owned-companion"; }
    assert(!check().ready); // CUE basename, not data-track basename.
    { std::ofstream file(sbi); file << "wrong-companion"; }
    assert(!check().ready);
    { std::ofstream file(cue); file << "FILE \"source.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"; }
    const auto imported = PSXRecompV4::import_sbi_setup(data, cue, root / "source.sbi", root / "imports", &rule, 1);
    assert(PSXRecompV4::check_sbi_setup(data, imported, &rule, 1).ready);
    assert(PSXRecompV4::sbi_setup_hash(data) == data_hash);
    assert(!check().ready); // Original selection is not modified.
    bool rejected = false;
    try { PSXRecompV4::import_sbi_setup(data, cue, sbi, root / "imports", &rule, 1); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    { std::ofstream file(data); file << "other-owned-disc!"; }
    assert(std::filesystem::file_size(data) == rule.size);
    assert(!check().required && check().ready); // Size alone does not identify protection.
    const auto unknown = PSXRecompV4::check_sbi_setup(data, cue);
    assert(!unknown.required && unknown.ready); // Production registry does not classify fixtures.
    std::cout << "PASS: native SBI setup gate\n";
}
