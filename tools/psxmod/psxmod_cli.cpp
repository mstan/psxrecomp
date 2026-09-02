/* psxmod — the mod author's command line.
 *
 * Until now the only thing that could tell an author whether their manifest
 * was valid was the runtime, and only by loading it: pack, install, and watch
 * the mod silently fail to appear. Every rule lived in ModPackageManager and
 * was unreachable from a shell.
 *
 * This tool links mod_packages.cpp and calls the SAME read_manifest() the
 * runtime calls, so there is exactly one implementation of the manifest rules.
 * A validator that reimplemented them would drift from the parser on its first
 * format bump and start blessing manifests the runtime rejects -- which is
 * worse than having no validator, because it would be believed.
 *
 * Subcommands:
 *   validate <path>...   parse manifests and report what the runtime would see
 */

#include "mod_packages.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

namespace {

/* The version an author should be writing today. Older ones still load -- the
 * runtime keeps them readable so installed packages survive an update -- but a
 * new manifest written at an old version opts out of everything since. */
constexpr uint32_t kCurrentFormatVersion = 6;

struct Counts {
    size_t failed = 0;
    size_t warned = 0;
    size_t passed = 0;
};

void usage() {
    std::cout <<
        "psxmod — mod package tools for psxrecomp\n"
        "\n"
        "Usage:\n"
        "  psxmod validate [--strict] [--quiet] <path>...\n"
        "\n"
        "A <path> may be a manifest.toml, a package source directory holding\n"
        "one, or a catalog directory holding <id>/<version>/manifest.toml.\n"
        "\n"
        "Options:\n"
        "  --strict   treat warnings as failures (use this in CI)\n"
        "  --quiet    print only failures and warnings\n"
        "\n"
        "Exit status is non-zero when any manifest fails to parse, or when any\n"
        "warning was raised under --strict.\n";
}

/* Expand one argument into the manifests it names. A directory holding a
 * manifest is one package; a directory holding <id>/<version>/manifest.toml is
 * a catalog, which is how both mods/bundled and mods/installed are laid out. */
bool collect(const fs::path& path, std::vector<fs::path>& out, std::string& error) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        out.push_back(path);
        return true;
    }
    if (!fs::is_directory(path, ec)) {
        error = "no such file or directory: " + path.string();
        return false;
    }
    if (fs::is_regular_file(path / "manifest.toml", ec)) {
        out.push_back(path / "manifest.toml");
        return true;
    }
    /* A catalog is <id>/<version>/manifest.toml, which is how mods/bundled and
     * mods/installed are laid out. A working directory holding several package
     * source dirs is <name>/manifest.toml. Accept both: an author should not
     * have to know which shape the tool wanted. */
    const size_t before = out.size();
    for (const fs::directory_entry& child : fs::directory_iterator(path, ec)) {
        if (ec) break;
        if (!child.is_directory()) continue;
        if (fs::is_regular_file(child.path() / "manifest.toml", ec)) {
            out.push_back(child.path() / "manifest.toml");
            continue;
        }
        for (const fs::directory_entry& version_dir :
             fs::directory_iterator(child.path(), ec)) {
            if (ec) break;
            if (!version_dir.is_directory()) continue;
            const fs::path manifest = version_dir.path() / "manifest.toml";
            if (fs::is_regular_file(manifest, ec)) out.push_back(manifest);
        }
    }
    if (out.size() == before) {
        error = "no manifest.toml found in or under: " + path.string();
        return false;
    }
    return true;
}

size_t count_for_feature(const ModPackage& package, const std::string& feature_id) {
    size_t n = 0;
    for (const ModOption& option : package.options)
        if (option.feature_id == feature_id) ++n;
    return n;
}

bool validate_one(const fs::path& manifest, bool quiet, Counts& counts) {
    ModPackage package;
    std::string error;
    if (!ModPackageManager::read_manifest(manifest, package, &error)) {
        /* read_manifest prefixes its own errors with the path. The heading
         * already carries it, so do not say it twice. */
        const std::string prefix = manifest.string() + ": ";
        if (error.rfind(prefix, 0) == 0) error.erase(0, prefix.size());
        std::cout << "FAIL  " << manifest.string() << "\n"
                  << "      " << error << "\n";
        ++counts.failed;
        return false;
    }

    /* Warnings are defects: the runtime will refuse this package, or the
     * author has done something the doctrine says not to. Notes are things
     * worth knowing that are not wrong -- they never fail --strict, because a
     * CI gate that fires on "a newer format exists" trains people to pass
     * --no-verify. */
    std::vector<std::string> warnings, notes;

    /* scan() requires the directory names to match the manifest, and rejects
     * the package outright when they do not. That is a confusing way to find
     * out, so say it here where the fix is obvious. */
    const fs::path version_dir = manifest.parent_path();
    const fs::path package_dir = version_dir.parent_path();
    if (!version_dir.empty() && !package_dir.empty()) {
        const std::string dir_version = version_dir.filename().string();
        const std::string dir_package = package_dir.filename().string();
        const bool looks_installed =
            dir_package != "." && !dir_package.empty() &&
            (dir_version == package.version || dir_package == package.id);
        if (looks_installed &&
            (dir_package != package.id || dir_version != package.version)) {
            warnings.push_back(
                "directory layout does not match the manifest: expected " +
                package.id + "/" + package.version + "/manifest.toml, found " +
                dir_package + "/" + dir_version +
                "/manifest.toml. The runtime skips packages whose path and "
                "manifest disagree.");
        }
    }

    if (package.format_version < kCurrentFormatVersion) {
        notes.push_back(
            "format_version " + std::to_string(package.format_version) +
            " still loads, but " + std::to_string(kCurrentFormatVersion) +
            " is current. Older versions cannot use the fields added since.");
    }

    bool has_legacy_feature = false;
    for (const ModFeature& feature : package.features)
        if (feature.legacy) has_legacy_feature = true;
    if (has_legacy_feature) {
        warnings.push_back(
            "this is a package-style manifest with no [[feature]] entries, so "
            "the runtime synthesises a single feature named \"legacy\" for it. "
            "Write explicit [[feature]] entries instead: feature identity is "
            "(package_id, feature_id), and everything a player toggles hangs "
            "off it.");
    }

    for (const ModFeature& feature : package.features) {
        if (feature.default_enabled) {
            warnings.push_back(
                "feature \"" + feature.id +
                "\" is default_enabled. The faithful path is the product: a "
                "feature should be something the player turns on.");
        }
        if (feature.channel == ModChannel::Developer) {
            notes.push_back(
                "feature \"" + feature.id +
                "\" is on the developer channel, so it will not appear in a "
                "release build at all. That is the point of the channel, not a "
                "problem — said here so it is not a surprise later.");
        }
    }

    /* --quiet is for a CI log and for validating a whole catalog: show the
     * defects, not the inventory. Notes are not defects, so they do not break
     * the silence -- if they did, --quiet would print every package that is
     * merely one format version behind, which is all of them. */
    if (!quiet || !warnings.empty()) {
        std::cout << (warnings.empty() ? "OK    " : "WARN  ")
                  << manifest.string() << "\n";
        std::cout << "      " << package.id << " " << package.version
                  << "  (format " << package.format_version << ", "
                  << package.features.size() << " feature"
                  << (package.features.size() == 1 ? "" : "s") << ")\n";
        for (const ModFeature& feature : package.features) {
            std::cout << "        - " << feature.id << "  ["
                      << mod_channel_name(feature.channel) << "]"
                      << (feature.default_enabled ? " (default on)" : "")
                      << (feature.hidden ? " (hidden)" : "");
            const size_t options = count_for_feature(package, feature.id);
            if (options)
                std::cout << "  " << options << " option"
                          << (options == 1 ? "" : "s");
            std::cout << "\n";
        }
        if (!package.patches.empty() || !package.overlays.empty() ||
            !package.plugins.empty() || !package.resources.empty()) {
            std::cout << "        operations:";
            if (!package.patches.empty())
                std::cout << " " << package.patches.size() << " patch";
            if (!package.overlays.empty())
                std::cout << " " << package.overlays.size() << " overlay";
            if (!package.plugins.empty())
                std::cout << " " << package.plugins.size() << " plugin";
            if (!package.resources.empty())
                std::cout << " " << package.resources.size() << " resource";
            std::cout << "\n";
        }
    }
    for (const std::string& warning : warnings)
        std::cout << "      warning: " << warning << "\n";
    if (!quiet)
        for (const std::string& note : notes)
            std::cout << "      note: " << note << "\n";

    if (warnings.empty()) ++counts.passed; else ++counts.warned;
    return true;
}

int validate(int argc, char** argv) {
    bool strict = false, quiet = false;
    std::vector<fs::path> args;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--strict") == 0) { strict = true; continue; }
        if (std::strcmp(argv[i], "--quiet") == 0) { quiet = true; continue; }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            std::cerr << "psxmod: unknown option " << argv[i] << "\n";
            return 2;
        }
        args.push_back(argv[i]);
    }
    if (args.empty()) {
        std::cerr << "psxmod validate: no path given\n";
        return 2;
    }

    std::vector<fs::path> manifests;
    for (const fs::path& arg : args) {
        std::string error;
        if (!collect(arg, manifests, error)) {
            std::cerr << "psxmod: " << error << "\n";
            return 2;
        }
    }

    Counts counts;
    for (const fs::path& manifest : manifests)
        validate_one(manifest, quiet, counts);

    std::cout << counts.passed << " ok, " << counts.warned << " with warnings, "
              << counts.failed << " failed\n";
    if (counts.failed) return 1;
    if (strict && counts.warned) {
        std::cerr << "psxmod: warnings are failures under --strict\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        usage();
        return 0;
    }
    if (command == "validate") return validate(argc - 2, argv + 2);

    std::cerr << "psxmod: unknown command \"" << command << "\"\n\n";
    usage();
    return 2;
}
