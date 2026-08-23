#include "mod_packages.h"
#include "psx_sha256.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static int failures;

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

static void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

static std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

static void write_deflated_package(const fs::path& path) {
    static const char* compressed_hex =
        "4bcb2fca4d2c892f4b2d2acecccf53b05530e4ca4c01524a5599057ab9f929"
        "4a5c082925433d033d0325aebcc4dc541037ca3340c117a4a428b5383f07a80"
        "e2498929a9c93589458925996aac4151d5d9258949e5a121bcb950ed4140f313"
        "ad827345837c4353844890b00";
    std::vector<uint8_t> compressed;
    for (const char* p = compressed_hex; *p; p += 2)
        compressed.push_back((uint8_t)std::stoul(std::string(p, 2), nullptr, 16));
    std::vector<uint8_t> zip;
    auto le16 = [&](uint16_t v) {
        zip.push_back((uint8_t)v); zip.push_back((uint8_t)(v >> 8));
    };
    auto le32 = [&](uint32_t v) {
        le16((uint16_t)v); le16((uint16_t)(v >> 16));
    };
    const std::string name = "manifest.toml";
    le32(0x04034b50); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), compressed.begin(), compressed.end());
    const uint32_t central_offset = (uint32_t)zip.size();
    le32(0x02014b50); le16(20); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0); le16(0); le16(0); le16(0);
    le32(0); le32(0);
    zip.insert(zip.end(), name.begin(), name.end());
    const uint32_t central_size = (uint32_t)zip.size() - central_offset;
    le32(0x06054b50); le16(0); le16(0); le16(1); le16(1);
    le32(central_size); le32(central_offset); le16(0);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)zip.data(), (std::streamsize)zip.size());
}

static std::string manifest(const std::string& id, const std::string& version,
                            const std::string& extra = {}) {
    return
        "format_version = 1\n"
        "id = \"" + id + "\"\n"
        "version = \"" + version + "\"\n"
        "name = \"" + id + "\"\n"
        "author = \"Test Author\"\n"
        "source_name = \"Upstream project\"\n"
        "source_url = \"https://example.com/project\"\n"
        "resolver = \"declarative\"\n"
        "[[author_link]]\n"
        "name = \"Test Author\"\n"
        "url = \"https://example.com/author\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-TEST\"\n" + extra;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-package-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    {
        const uint8_t abc[] = {'a', 'b', 'c'};
        uint8_t one_shot[32], streamed[32];
        psx_sha256_compute(abc, sizeof(abc), one_shot);
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        psx_sha256_update(&hash, abc, 1);
        psx_sha256_update(&hash, abc + 1, 2);
        psx_sha256_final(&hash, streamed);
        check(std::equal(one_shot, one_shot + 32, streamed),
              "streaming SHA-256 must match one-shot hashing");
    }

    write_text(root / "packages/base.mod/1.0.0/manifest.toml",
               manifest("base.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"difficulty\"\n"
                   "label = \"Difficulty\"\n"
                   "type = \"choice\"\n"
                   "default = \"normal\"\n"
                   "[[option.choice]]\nvalue = \"normal\"\nlabel = \"Normal\"\n"
                   "[[option.choice]]\nvalue = \"hard\"\nlabel = \"Hard\"\n"
                   "[[patch]]\n"
                   "target = \"main_exe\"\n"
                   "address = 2147487744\n"
                   "expected = \"01 02 03 04\"\n"
                   "replace = \"05 06 07 08\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"
                   "[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/base.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 123456\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"));
    write_text(root / "packages/base.mod/1.0.0/assets/base.xdelta3", "test");
    write_text(root / "packages/addon.mod/2.0.0/manifest.toml",
               manifest("addon.mod", "2.0.0",
                   "\n[[dependency]]\nid = \"base.mod\"\nversion = \"^1.0.0\"\n"));

    ModPackageManager manager(root);
    std::string error;
    check(manager.scan(&error), error.c_str());
    const ModPackage& metadata_package =
        manager.packages().at("base.mod").at("1.0.0");
    check(metadata_package.source_url == "https://example.com/project",
          "package source URL must be retained");
    check(metadata_package.author_links.size() == 1 &&
              metadata_package.author_links[0].name == "Test Author" &&
              metadata_package.author_links[0].url == "https://example.com/author",
          "package author links must be retained");
    write_deflated_package(root / "zip.psxmod");
    check(manager.install_archive(root / "zip.psxmod", nullptr, nullptr, &error),
          error.c_str());
    check(manager.packages().count("zip.mod") == 1,
          "deflated .psxmod must install");
    if (const char* external = std::getenv("PSXMOD_TEST_ARCHIVE");
        external && external[0]) {
        std::string installed_id, installed_version;
        check(manager.install_archive(external, &installed_id, &installed_version,
                                      &error),
              error.c_str());
        check(!installed_id.empty() && !installed_version.empty(),
              "external package must report installed identity");
    }
    check(manager.load_state(&error), error.c_str());
    check(manager.set_enabled("addon.mod", true, &error), error.c_str());
    ModResolution missing = manager.resolve("SLUS-TEST");
    check(!missing.ok, "missing dependency must fail resolution");
    check(manager.set_enabled("base.mod", true, &error), error.c_str());
    check(manager.set_option("base.mod", "difficulty", "hard", &error), error.c_str());
    check(!manager.set_option("base.mod", "difficulty", "impossible", &error),
          "invalid choice must be rejected");

    ModResolution resolved = manager.resolve("SLUS-TEST");
    check(resolved.ok, "valid dependency graph must resolve");
    check(resolved.ordered.size() == 2, "two packages should resolve");
    check(resolved.ordered.size() == 2 && resolved.ordered[0]->id == "base.mod",
          "dependency must precede dependent");
    check(resolved.writes.size() == 1, "selected declarative patch must resolve");
    check(resolved.writes.size() == 1 &&
              resolved.writes[0].location == 0x80001000ull &&
              resolved.writes[0].replacement[0] == 5,
          "resolved write must retain guest address and bytes");
    check(resolved.derived_discs.size() == 1 &&
              resolved.derived_discs[0].output_size == 123456,
          "selected derived-disc recipe must resolve");
    check(resolved.fingerprint.size() == 64, "plan fingerprint must be SHA-256 hex");
    const std::string fingerprint = resolved.fingerprint;

    check(manager.save_state(&error), error.c_str());
    ModPackageManager reload(root);
    check(reload.scan(&error), error.c_str());
    check(reload.load_state(&error), error.c_str());
    check(reload.resolve("SLUS-TEST").fingerprint == fingerprint,
          "saved state must resolve deterministically");
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "active package cannot be removed");
    check(reload.set_enabled("base.mod", false, &error), error.c_str());
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "enabled dependent must protect required version");
    check(reload.set_enabled("addon.mod", false, &error), error.c_str());
    check(reload.remove_version("base.mod", "1.0.0", &error), error.c_str());

    write_text(root / "packages/conflict.a/1.0.0/manifest.toml",
               "format_version = 1\n"
               "id = \"conflict.a\"\n"
               "version = \"1.0.0\"\n"
               "name = \"conflict.a\"\n"
               "resolver = \"declarative\"\n"
               "conflicts = [\"conflict.b\"]\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n");
    write_text(root / "packages/conflict.b/1.0.0/manifest.toml",
               manifest("conflict.b", "1.0.0"));
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", true, &error), error.c_str());
    check(reload.set_enabled("conflict.b", true, &error), error.c_str());
    check(reload.selections().at("conflict.a").enabled &&
              reload.selections().at("conflict.b").enabled,
          "enabling a package must not silently disable another package");
    check(!reload.resolve("SLUS-TEST").ok,
          "declared conflicts must fail resolution");

    write_text(root / "packages/matrix.mod/1.0.0/manifest.toml",
               manifest("matrix.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"title\"\n"
                   "label = \"Title\"\n"
                   "type = \"choice\"\n"
                   "default = \"mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"mega\"\n"
                   "label = \"Mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"rockman\"\n"
                   "label = \"Rockman\"\n"
                   "\n[[option]]\n"
                   "id = \"script\"\n"
                   "label = \"Script\"\n"
                   "type = \"choice\"\n"
                   "default = \"original\"\n"
                   "[[option.choice]]\n"
                   "value = \"original\"\n"
                   "label = \"Original\"\n"
                   "[[option.choice]]\n"
                   "value = \"retranslation\"\n"
                   "label = \"Retranslation\"\n"
                   "\n[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/matrix.xdelta3\"\n"
                   "patch_sha256 = \"2222222222222222222222222222222222222222222222222222222222222222\"\n"
                   "output_size = 222222\n"
                   "output_sha256 = \"3333333333333333333333333333333333333333333333333333333333333333\"\n"
                   "when = { title = \"rockman\", script = \"retranslation\" }\n"));
    write_text(root / "packages/matrix.mod/1.0.0/assets/matrix.xdelta3", "test");
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", false, &error), error.c_str());
    check(reload.set_enabled("conflict.b", false, &error), error.c_str());
    check(reload.set_enabled("matrix.mod", true, &error), error.c_str());
    check(reload.set_option("matrix.mod", "title", "rockman", &error), error.c_str());
    check(reload.set_option("matrix.mod", "script", "retranslation", &error), error.c_str());
    ModResolution matrix = reload.resolve("SLUS-TEST");
    check(matrix.ok && matrix.derived_discs.size() == 1 &&
              matrix.derived_discs[0].output_size == 222222,
          "multi-option derived-disc condition must match selected values");

    mod_clear_builtin_resolvers_for_tests();
    bool resolver_context_seen = false;
    check(mod_register_builtin_resolver(
              "context-test",
              [&](const ModPackage& package, const ModSelection& selection,
                  const ModBuiltinResolverContext& context,
                  std::vector<ModResolution::Write>& writes,
                  std::vector<std::string>& errors) {
                  (void)writes;
                  (void)errors;
                  resolver_context_seen =
                      package.id == "context.consumer" &&
                      selection.enabled &&
                      context.active_packages &&
                      context.selections &&
                      context.active_packages->count("context.provider") == 1 &&
                      context.active_packages->count("context.consumer") == 1 &&
                      context.selections->at("context.provider").enabled &&
                      context.selections->at("context.consumer").enabled;
                  return resolver_context_seen;
              }),
          "test resolver must register");
    write_text(root / "packages/context.provider/1.0.0/manifest.toml",
               manifest("context.provider", "1.0.0"));
    write_text(root / "packages/context.consumer/1.0.0/manifest.toml",
               "format_version = 1\n"
               "id = \"context.consumer\"\n"
               "version = \"1.0.0\"\n"
               "name = \"context.consumer\"\n"
               "resolver = \"builtin:context-test\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("matrix.mod", false, &error), error.c_str());
    check(reload.set_enabled("context.provider", true, &error), error.c_str());
    check(reload.set_enabled("context.consumer", true, &error), error.c_str());
    ModResolution context_resolution = reload.resolve("SLUS-TEST");
    check(context_resolution.ok && resolver_context_seen,
          "built-in resolver must receive active package selection context");
    check(reload.set_enabled("context.consumer", false, &error), error.c_str());
    check(reload.set_enabled("context.provider", false, &error), error.c_str());
    mod_clear_builtin_resolvers_for_tests();

    mod_clear_plugins_for_tests();
    check(mod_register_vblank_plugin(
              "test.vblank", +[]() {}),
          "test plugin must register");
    write_text(root / "packages/plugin.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"plugin.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Plugin Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"vblank\"\n"
               "name = \"VBlank Plugin\"\n"
               "[[plugin]]\n"
               "feature = \"vblank\"\n"
               "id = \"test.vblank\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", true, &error), error.c_str());
    ModResolution plugin_resolution = reload.resolve("SLUS-TEST");
    check(plugin_resolution.ok && plugin_resolution.plugins.size() == 1 &&
              plugin_resolution.plugins[0].id == "test.vblank" &&
              plugin_resolution.plugins[0].package_id == "plugin.mod" &&
              plugin_resolution.plugins[0].feature_id == "vblank",
          "enabled trusted plugin must resolve with feature ownership");
    const std::string plugin_fingerprint = plugin_resolution.fingerprint;
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", false, &error), error.c_str());
    ModResolution plugin_disabled = reload.resolve("SLUS-TEST");
    check(plugin_disabled.ok && plugin_disabled.plugins.empty() &&
              plugin_disabled.fingerprint != plugin_fingerprint,
          "disabling a plugin feature must remove its activation and change "
          "the fingerprint");
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", true, &error), error.c_str());
    mod_clear_plugins_for_tests();
    ModResolution plugin_unavailable = reload.resolve("SLUS-TEST");
    check(!plugin_unavailable.ok &&
              std::any_of(
                  plugin_unavailable.errors.begin(),
                  plugin_unavailable.errors.end(),
                  [](const std::string& item) {
                      return item.find(
                          "trusted plugin is unavailable: test.vblank") !=
                          std::string::npos;
                  }),
          "enabled plugin must fail closed when its trusted implementation "
          "is unavailable");
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", false, &error), error.c_str());

    /* Function-entry plugins register callbacks in mod_runtime but must still
     * appear in mod_plugin_registered so manifests can [[plugin]] them. */
    mod_clear_plugins_for_tests();
    check(mod_register_function_entry_plugin_id("test.fn-entry"),
          "function-entry plugin id must register for resolve");
    check(mod_plugin_registered("test.fn-entry"),
          "function-entry id must count as a trusted plugin");
    write_text(root / "packages/fnentry.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"fnentry.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Fn Entry Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"gate\"\n"
               "name = \"Gate Entry\"\n"
               "[[plugin]]\n"
               "feature = \"gate\"\n"
               "id = \"test.fn-entry\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_feature_enabled(
              "fnentry.mod", "gate", true, &error), error.c_str());
    ModResolution fnentry_resolution = reload.resolve("SLUS-TEST");
    check(fnentry_resolution.ok && fnentry_resolution.plugins.size() == 1 &&
              fnentry_resolution.plugins[0].id == "test.fn-entry",
          "function-entry plugin id must resolve from the package plan");
    check(reload.set_feature_enabled(
              "fnentry.mod", "gate", false, &error), error.c_str());
    mod_clear_plugins_for_tests();
    check(reload.set_feature_enabled(
              "fnentry.mod", "gate", true, &error), error.c_str());
    ModResolution fnentry_unavailable = reload.resolve("SLUS-TEST");
    check(!fnentry_unavailable.ok &&
              std::any_of(
                  fnentry_unavailable.errors.begin(),
                  fnentry_unavailable.errors.end(),
                  [](const std::string& item) {
                      return item.find(
                          "trusted plugin is unavailable: test.fn-entry") !=
                          std::string::npos;
                  }),
          "function-entry plugin must fail closed when unregistered");
    check(reload.set_feature_enabled(
              "fnentry.mod", "gate", false, &error), error.c_str());

    write_text(root / "packages/features.mod/1.0.0/manifest.toml",
               manifest("features.mod", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"title-screen\"\n"
                   "name = \"Title Screen\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"retranslation\"\n"
                   "name = \"Retranslation\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-collision\"\n"
                   "name = \"Title Collision\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-identical\"\n"
                   "name = \"Title Identical\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-partial-compatible\"\n"
                   "name = \"Title Partial Compatible\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-partial-conflict\"\n"
                   "name = \"Title Partial Conflict\"\n"
                   "\n[[option]]\n"
                   "feature = \"title-screen\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"choice\"\n"
                   "default = \"usa\"\n"
                   "[[option.choice]]\n"
                   "value = \"usa\"\n"
                   "label = \"Mega Man X6\"\n"
                   "[[option.choice]]\n"
                   "value = \"japan\"\n"
                   "label = \"Rockman X6\"\n"
                   "\n[[option]]\n"
                   "feature = \"retranslation\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"boolean\"\n"
                   "default = \"true\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-screen\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"
                   "when = { variant = \"japan\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"retranslation\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 23520\n"
                   "expected = \"03\"\n"
                   "replace = \"b3\"\n"
                   "when = { variant = \"true\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"title-collision\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"02\"\n"
                   "replace = \"ff\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-identical\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-partial-compatible\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"0209\"\n"
                   "replace = \"a2c9\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-partial-conflict\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"ff09\"\n"
                   "replace = \"a2c9\"\n"));
    check(reload.scan(&error), error.c_str());
    check(!reload.set_enabled("features.mod", true, &error),
          "feature-style package must not expose package enablement");
    check(reload.set_feature_option(
              "features.mod", "title-screen", "variant", "japan", &error),
          error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-screen", true, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "retranslation", true, &error), error.c_str());
    ModResolution features = reload.resolve("SLUS-TEST");
    check(features.ok && features.writes.size() == 2,
          "independently enabled features must compose their operations");
    check(features.ok && features.writes[0].feature_id == "title-screen" &&
              features.writes[1].feature_id == "retranslation",
          "resolved writes must retain feature ownership");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", true, &error), error.c_str());
    ModResolution collision = reload.resolve("SLUS-TEST");
    check(!collision.ok && collision.diagnostics.size() == 1,
          "overlapping feature writes must produce a structured diagnostic");
    check(!collision.diagnostics.empty() &&
              collision.diagnostics[0].feature_id == "title-collision" &&
              collision.diagnostics[0].other_feature_id == "title-screen" &&
              !collision.diagnostics[0].resource.empty(),
          "collision diagnostic must identify both features and the resource");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", false, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-identical", true, &error), error.c_str());
    ModResolution identical = reload.resolve("SLUS-TEST");
    check(identical.ok && identical.writes.size() == 2,
          "truly identical writes must coalesce deterministically");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-compatible", true, &error),
          error.c_str());
    ModResolution partial_compatible = reload.resolve("SLUS-TEST");
    check(partial_compatible.ok && partial_compatible.writes.size() == 3,
          "partially overlapping writes with matching expected and replacement "
          "bytes must compose");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-conflict", true, &error),
          error.c_str());
    ModResolution partial_conflict = reload.resolve("SLUS-TEST");
    check(!partial_conflict.ok && !partial_conflict.diagnostics.empty() &&
              partial_conflict.diagnostics[0].resource ==
                  "main_exe:0x80003001-0x80003002",
          "one differing expected byte in a partial overlap must identify "
          "the exact contested byte");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-conflict", false, &error),
          error.c_str());
    check(reload.save_state(&error), error.c_str());
    ModPackageManager feature_reload(root);
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.load_state(&error), error.c_str());
    check(feature_reload.feature_enabled("features.mod", "title-screen") &&
              feature_reload.feature_enabled("features.mod", "retranslation") &&
              !feature_reload.feature_enabled("features.mod", "title-collision"),
          "per-feature enabled state must survive save/reload");
    check(feature_reload.feature_option_value(
              "features.mod", "title-screen", "variant") == "japan",
          "feature-scoped option values must survive save/reload");
    check(feature_reload.resolve("SLUS-TEST").fingerprint ==
              partial_compatible.fingerprint,
          "feature state must resolve deterministically after reload");

    write_text(root / "packages/parametric.mod/1.0.0/manifest.toml",
               "format_version = 3\n"
               "id = \"parametric.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Parametric\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"numeric\"\n"
               "name = \"Numeric\"\n"
               "[[feature]]\n"
               "id = \"numeric-collision\"\n"
               "name = \"Numeric Collision\"\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"byte\"\n"
               "label = \"Byte\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 255\n"
               "step = 1\n"
               "default = 7\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"word\"\n"
               "label = \"Word\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 65534\n"
               "step = 1\n"
               "default = 4660\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"dword\"\n"
               "label = \"Dword\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 4294967295\n"
               "step = 1\n"
               "default = 305419896\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"split\"\n"
               "label = \"Split\"\n"
               "type = \"integer\"\n"
               "min = 200000\n"
               "max = 600000\n"
               "step = 1\n"
               "default = 425984\n"
               "[[option]]\n"
               "feature = \"numeric-collision\"\n"
               "id = \"byte\"\n"
               "label = \"Byte\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 255\n"
               "default = 8\n"
               "[[constraint]]\n"
               "feature = \"numeric\"\n"
               "kind = \"ordered_integer\"\n"
               "direction = \"nondecreasing\"\n"
               "options = [\"byte\", \"word\", \"dword\"]\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500032\n"
               "expected = \"00\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500033\n"
               "expected = \"07\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500034\n"
               "expected = \"0000\"\n"
               "replace_from = { option = \"word\", encoding = \"u16le\", addend = 1 }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500036\n"
               "expected = \"00000000\"\n"
               "replace_from = { option = \"dword\", encoding = \"u32le\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500040\n"
               "expected = \"0000aabb\"\n"
               "replace_from = { option = \"word\", encoding = \"u16le\", offset = 0 }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500048\n"
               "expected = \"0600013c00802134\"\n"
               "replace_from = { option = \"split\", encoding = \"mips_lui_ori_u32\", omit_when_default = true }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500056\n"
               "expected = \"0400013c00202134\"\n"
               "replace_from = { option = \"split\", encoding = \"mips_lui_ori_u32\", omit_when_default = true }\n"
               "[[patch]]\n"
               "feature = \"numeric-collision\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500032\n"
               "expected = \"00\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n");
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error), error.c_str());
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "+7", &error),
          "integer options must reject a leading plus");
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "07", &error),
          "integer options must reject noncanonical leading zeroes");
    ModResolution parametric = feature_reload.resolve("SLUS-TEST");
    const auto numeric_write = [&](uint64_t location)
        -> const ModResolution::Write* {
        const auto found = std::find_if(
            parametric.writes.begin(), parametric.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "parametric.mod" &&
                       write.location == location;
            });
        return found == parametric.writes.end() ? nullptr : &*found;
    };
    const ModResolution::Write* byte_write = numeric_write(0x80004000ull);
    const ModResolution::Write* noop_write = numeric_write(0x80004001ull);
    const ModResolution::Write* word_write = numeric_write(0x80004002ull);
    const ModResolution::Write* dword_write = numeric_write(0x80004004ull);
    const ModResolution::Write* guarded_word_write =
        numeric_write(0x80004008ull);
    check(parametric.ok && byte_write &&
              byte_write->replacement == std::vector<uint8_t>({7}),
          "u8 replace_from must encode the selected value");
    check(!noop_write,
          "replace_from equal to the stock guard must elide the no-op write");
    check(word_write &&
              word_write->replacement == std::vector<uint8_t>({0x35, 0x12}),
          "u16le replace_from must apply addend and encode little-endian");
    check(dword_write &&
              dword_write->replacement ==
                  std::vector<uint8_t>({0x78, 0x56, 0x34, 0x12}),
          "u32le replace_from must encode little-endian");
    check(guarded_word_write &&
              guarded_word_write->replacement ==
                  std::vector<uint8_t>({0x34, 0x12, 0xaa, 0xbb}),
          "replace_from must preserve guarded bytes outside its value field");
    check(!numeric_write(0x80004010ull) &&
              !numeric_write(0x80004018ull),
          "omit_when_default must suppress every split-immediate site");
    const std::string parametric_fingerprint = parametric.fingerprint;
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "9", &error),
          error.c_str());
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "5", &error),
          "enabled ordered integer features must reject inverted values");
    ModResolution changed_parametric = feature_reload.resolve("SLUS-TEST");
    check(changed_parametric.ok &&
              changed_parametric.fingerprint != parametric_fingerprint,
          "changing a generated integer must change the plan fingerprint");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "200000", &error),
          error.c_str());
    ModResolution split_parametric = feature_reload.resolve("SLUS-TEST");
    const auto split_write = [&](uint64_t location)
        -> const ModResolution::Write* {
        const auto found = std::find_if(
            split_parametric.writes.begin(), split_parametric.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "parametric.mod" &&
                       write.location == location;
            });
        return found == split_parametric.writes.end() ? nullptr : &*found;
    };
    check(split_write(0x80004010ull) &&
              split_write(0x80004010ull)->replacement ==
                  std::vector<uint8_t>({
                      0x03, 0x00, 0x01, 0x3c,
                      0x40, 0x0d, 0x21, 0x34}) &&
              split_write(0x80004018ull) &&
              split_write(0x80004018ull)->replacement ==
                  std::vector<uint8_t>({
                      0x03, 0x00, 0x01, 0x3c,
                      0x40, 0x0d, 0x21, 0x34}),
          "typed MIPS split encodings must update every guarded pair");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "270336", &error),
          error.c_str());
    ModResolution partial_stock_split =
        feature_reload.resolve("SLUS-TEST");
    check(std::count_if(
              partial_stock_split.writes.begin(),
              partial_stock_split.writes.end(),
              [](const ModResolution::Write& write) {
                  return write.package_id == "parametric.mod" &&
                         (write.location == 0x80004010ull ||
                          write.location == 0x80004018ull);
              }) == 2,
          "a nondefault split value must retain ownership of a pair whose "
          "replacement happens to equal stock");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "425984", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric-collision", true, &error),
          error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "different generated values at one guarded byte must collide");
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric-collision", false, &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", false, &error), error.c_str());
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "0", &error),
          "disabled features may retain an invalid draft");
    check(!feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error),
          "an invalid ordered integer draft must block feature enablement");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "4660", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error), error.c_str());
    check(feature_reload.save_state(&error), error.c_str());
    ModPackageManager parametric_reload(root);
    check(parametric_reload.scan(&error), error.c_str());
    check(parametric_reload.load_state(&error), error.c_str());
    check(parametric_reload.feature_option_value(
              "parametric.mod", "numeric", "byte") == "9" &&
              parametric_reload.resolve("SLUS-TEST").fingerprint ==
                  changed_parametric.fingerprint,
          "generated integer state and fingerprint must survive reload");

    write_text(root / "packages/requires.mod/1.0.0/manifest.toml",
               "format_version = 4\n"
               "id = \"requires.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Requires\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"prereq\"\n"
               "name = \"Prerequisite\"\n"
               "[[feature]]\n"
               "id = \"dependent\"\n"
               "name = \"Dependent\"\n"
               "[[feature]]\n"
               "id = \"optioned-dependent\"\n"
               "name = \"Optioned Dependent\"\n"
               "[[option]]\n"
               "feature = \"prereq\"\n"
               "id = \"availability\"\n"
               "label = \"Available in\"\n"
               "type = \"choice\"\n"
               "default = \"main\"\n"
               "[[option.choice]]\n"
               "value = \"main\"\n"
               "label = \"Main Stages\"\n"
               "[[option.choice]]\n"
               "value = \"everywhere\"\n"
               "label = \"Everywhere\"\n"
               "[[constraint]]\n"
               "feature = \"dependent\"\n"
               "kind = \"requires_feature\"\n"
               "requires_feature = \"prereq\"\n"
               "[[constraint]]\n"
               "feature = \"optioned-dependent\"\n"
               "kind = \"requires_feature\"\n"
               "requires_feature = \"prereq\"\n"
               "requires_option = \"availability\"\n"
               "requires_value = \"everywhere\"\n");
    check(parametric_reload.scan(&error), error.c_str());
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "dependent", true, &error), error.c_str());
    check(parametric_reload.feature_enabled("requires.mod", "prereq") &&
              parametric_reload.feature_enabled("requires.mod", "dependent"),
          "enabling a dependent feature must auto-enable its prerequisite");
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "optioned-dependent", true, &error),
          error.c_str());
    check(parametric_reload.feature_enabled(
              "requires.mod", "optioned-dependent") &&
              parametric_reload.feature_option_value(
                  "requires.mod", "prereq", "availability") == "everywhere",
          "enabling an optioned dependent must auto-select the required "
          "prerequisite value");
    check(parametric_reload.set_feature_option(
              "requires.mod", "prereq", "availability", "main", &error),
          error.c_str());
    check(parametric_reload.feature_enabled("requires.mod", "prereq") &&
              parametric_reload.feature_enabled("requires.mod", "dependent") &&
              !parametric_reload.feature_enabled(
                  "requires.mod", "optioned-dependent"),
          "weakening a prerequisite option must disable invalid dependents");
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "prereq", false, &error), error.c_str());
    check(!parametric_reload.feature_enabled("requires.mod", "prereq") &&
              !parametric_reload.feature_enabled("requires.mod", "dependent"),
          "disabling a prerequisite must disable downstream dependents");

    const auto reject_parametric_manifest =
        [&](const std::string& name, const std::string& body) {
            const fs::path path = root / (name + ".toml");
            write_text(path, body);
            ModPackage rejected;
            return !ModPackageManager::read_manifest(path, rejected, &error);
        };
    const std::string dynamic_prelude =
        "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
        "[[target]]\ngame_id=\"SLUS-TEST\"\n"
        "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
        "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
        "type=\"integer\"\nmin=0\nmax=255\ndefault=1\n";
    check(reject_parametric_manifest(
              "dynamic-v1",
              "format_version=1\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\"}\n"),
          "format 1 manifests must reject replace_from");
    check(reject_parametric_manifest(
              "dynamic-both",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\nreplace=\"01\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\"}\n"),
          "a patch must reject simultaneous replace and replace_from");
    check(reject_parametric_manifest(
              "dynamic-width",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0000\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",offset=2}\n"),
          "replace_from value must stay inside the expected guard");
    check(reject_parametric_manifest(
              "dynamic-overflow",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",addend=1}\n"),
          "the full option range plus addend must fit its encoding");
    check(reject_parametric_manifest(
              "dynamic-unknown",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",shift=1}\n"),
          "replace_from must reject unknown transform fields");
    check(reject_parametric_manifest(
              "dynamic-mips-v2",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0600013c00802134\"\n"
                  "replace_from={option=\"value\","
                  "encoding=\"mips_lui_ori_u32\"}\n"),
          "typed MIPS pairs must require package format 3");
    check(reject_parametric_manifest(
              "dynamic-mips-unlinked",
              "format_version=3\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0600013c00802234\"\n"
                  "replace_from={option=\"value\","
                  "encoding=\"mips_lui_ori_u32\"}\n"),
          "typed MIPS pairs must reject unlinked registers");
    check(reject_parametric_manifest(
              "constraint-inverted-default",
              "format_version=3\n"
              "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-TEST\"\n"
              "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
              "[[option]]\nfeature=\"bad\"\nid=\"low\"\nlabel=\"Low\"\n"
              "type=\"integer\"\nmin=0\nmax=10\ndefault=8\n"
              "[[option]]\nfeature=\"bad\"\nid=\"high\"\nlabel=\"High\"\n"
              "type=\"integer\"\nmin=0\nmax=10\ndefault=2\n"
              "[[constraint]]\nfeature=\"bad\"\n"
              "kind=\"ordered_integer\"\ndirection=\"nondecreasing\"\n"
              "options=[\"low\",\"high\"]\n"),
          "ordered integer defaults must satisfy their constraint");
    check(reject_parametric_manifest(
              "dynamic-step-default",
              "format_version=2\n"
              "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-TEST\"\n"
              "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
              "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
              "type=\"integer\"\nmin=0\nmax=10\nstep=2\ndefault=3\n"),
          "integer defaults must align to their declared step");

    write_text(root / "packages/sparse.mod/1.0.0/manifest.toml",
               "format_version = 4\n"
               "id = \"sparse.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Sparse Fields\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"timing\"\n"
               "name = \"Timing\"\n"
               "[[feature]]\n"
               "id = \"cancellable\"\n"
               "name = \"Cancellable\"\n"
               "[[feature]]\n"
               "id = \"collision\"\n"
               "name = \"Collision\"\n"
               "[[feature]]\n"
               "id = \"guard-mismatch\"\n"
               "name = \"Guard Mismatch\"\n"
               "[[feature]]\n"
               "id = \"predicates\"\n"
               "name = \"Predicates\"\n"
               "[[option]]\n"
               "feature = \"timing\"\n"
               "id = \"frames\"\n"
               "label = \"Frames\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 99\n"
               "default = 2\n"
               "[[option]]\n"
               "feature = \"predicates\"\n"
               "id = \"value\"\n"
               "label = \"Value\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 10\n"
               "default = 5\n"
               "[[patch]]\n"
               "feature = \"timing\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, option = \"frames\", encoding = \"u8\" }]\n"
               "when_integer = { option = \"frames\", op = \"gt\", value = 0 }\n"
               "[[patch]]\n"
               "feature = \"timing\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, replace = \"01\" }, "
               "{ offset = 2, replace = \"00\" }]\n"
               "when_integer = { option = \"frames\", op = \"eq\", value = 0 }\n"
               "[[patch]]\n"
               "feature = \"cancellable\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 1, replace = \"42\" }]\n"
               "[[patch]]\n"
               "feature = \"collision\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, replace = \"09\" }]\n"
               "[[patch]]\n"
               "feature = \"guard-mismatch\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"03000132\"\n"
               "fields = [{ offset = 3, replace = \"33\" }]\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508480\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"eq\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508481\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"ne\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508482\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"lt\", value = 6 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508483\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"le\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508484\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"gt\", value = 4 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508485\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"ge\", value = 5 }\n");
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "timing", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "cancellable", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "predicates", true, &error), error.c_str());
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "5", &error),
          error.c_str());
    ModResolution sparse_positive = feature_reload.resolve("SLUS-TEST");
    const auto sparse_writes_at = [&](const ModResolution& plan,
                                      uint64_t location) {
        return std::count_if(
            plan.writes.begin(), plan.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "sparse.mod" &&
                       write.location == location;
            });
    };
    check(sparse_positive.ok &&
              sparse_writes_at(sparse_positive, 0x80006000ull) == 2,
          "adjacent sparse fields in one guarded record must compose");
    const auto timing_write = std::find_if(
        sparse_positive.writes.begin(), sparse_positive.writes.end(),
        [](const ModResolution::Write& write) {
            return write.package_id == "sparse.mod" &&
                   write.feature_id == "timing" &&
                   write.location == 0x80006000ull;
        });
    check(timing_write != sparse_positive.writes.end() &&
              timing_write->expected ==
                  std::vector<uint8_t>({2, 0, 1, 0x32}) &&
              timing_write->replacement.empty() &&
              timing_write->fields.size() == 1 &&
              timing_write->fields[0].offset == 0 &&
              timing_write->fields[0].replacement ==
                  std::vector<uint8_t>({5}),
          "sparse resolution must retain the complete guard but own only "
          "declared fields");
    check(std::count_if(
              sparse_positive.writes.begin(),
              sparse_positive.writes.end(),
              [](const ModResolution::Write& write) {
                  return write.package_id == "sparse.mod" &&
                         write.feature_id == "predicates";
              }) == 5,
          "eq/ne/lt/le/gt/ge predicates must resolve with typed integer "
          "semantics");
    const std::string sparse_positive_fingerprint =
        sparse_positive.fingerprint;
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "0", &error),
          error.c_str());
    ModResolution sparse_zero = feature_reload.resolve("SLUS-TEST");
    const auto zero_timing = std::find_if(
        sparse_zero.writes.begin(), sparse_zero.writes.end(),
        [](const ModResolution::Write& write) {
            return write.package_id == "sparse.mod" &&
                   write.feature_id == "timing";
        });
    check(sparse_zero.ok && zero_timing != sparse_zero.writes.end() &&
              zero_timing->fields.size() == 2 &&
              zero_timing->fields[0].offset == 0 &&
              zero_timing->fields[0].replacement ==
                  std::vector<uint8_t>({1}) &&
              zero_timing->fields[1].offset == 2 &&
              zero_timing->fields[1].replacement ==
                  std::vector<uint8_t>({0}) &&
              sparse_zero.fingerprint != sparse_positive_fingerprint,
          "zero and nonzero conditional sparse plans must own their exact "
          "distinct fields and fingerprints");
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "2", &error),
          error.c_str());
    ModResolution sparse_stock = feature_reload.resolve("SLUS-TEST");
    check(sparse_stock.ok &&
              sparse_writes_at(sparse_stock, 0x80006000ull) == 1,
          "stock-equal sparse fields must elide only their own no-op while "
          "an adjacent feature remains active");
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "5", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "collision", true, &error), error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "different sparse replacements for one owned byte must collide");
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "collision", false, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "guard-mismatch", true, &error), error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "overlapping complete guards with incompatible expected bytes "
          "must fail before runtime");
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "guard-mismatch", false, &error),
          error.c_str());

    const std::string sparse_prelude =
        "id=\"bad.sparse\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
        "[[target]]\ngame_id=\"SLUS-TEST\"\n"
        "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
        "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
        "type=\"integer\"\nmin=0\nmax=10\nstep=2\ndefault=2\n";
    const std::string sparse_patch =
        "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
        "address=2147487744\nexpected=\"00000000\"\n";
    check(reject_parametric_manifest(
              "sparse-v3",
              "format_version=3\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"),
          "sparse fields must require format 4");
    check(reject_parametric_manifest(
              "sparse-empty",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[]\n"),
          "sparse fields must not be empty");
    check(reject_parametric_manifest(
              "sparse-both",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "replace=\"01000000\"\n"
                  "fields=[{offset=0,replace=\"01\"}]\n"),
          "sparse fields must be mutually exclusive with full replace");
    check(reject_parametric_manifest(
              "sparse-overlap",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"0102\"},"
                  "{offset=1,replace=\"03\"}]\n"),
          "sparse fields must reject overlapping owned ranges");
    check(reject_parametric_manifest(
              "sparse-bounds",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=4,replace=\"01\"}]\n"),
          "sparse fields must stay inside the complete guard");
    check(reject_parametric_manifest(
              "sparse-mixed",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\",option=\"value\","
                  "encoding=\"u8\"}]\n"),
          "one sparse field must not mix literal and dynamic forms");
    check(reject_parametric_manifest(
              "sparse-overflow",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,option=\"value\",encoding=\"u8\","
                  "addend=250}]\n"),
          "sparse dynamic field ranges plus addends must fit encoding");
    check(reject_parametric_manifest(
              "sparse-predicate-op",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"between\",value=2}\n"),
          "typed integer predicates must reject unknown operations");
    check(reject_parametric_manifest(
              "sparse-predicate-feature",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"missing\",op=\"eq\",value=2}\n"),
          "typed integer predicates must reference same-feature integers");
    check(reject_parametric_manifest(
              "sparse-predicate-bounds",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"gt\",value=11}\n"),
          "typed integer predicate constants must stay in option bounds");
    check(reject_parametric_manifest(
              "sparse-predicate-step",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"eq\",value=3}\n"),
          "typed equality predicates must use selectable values");

    const std::vector<uint8_t> overlay_a = {1, 2, 3, 4};
    const std::vector<uint8_t> overlay_b = {8, 9};
    const std::vector<uint8_t> overlay_c = {3, 4, 7};
    const std::string overlay_disc_hash(64, '4');
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/a.bin", overlay_a);
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/b.bin", overlay_b);
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/c.bin", overlay_c);
    write_text(root / "packages/overlay.mod/1.0.0/manifest.toml",
               manifest("overlay.mod", "1.0.0",
                   "disc_sha256 = \"" + overlay_disc_hash + "\"\n"
                   "[[feature]]\n"
                   "id = \"asset-a\"\n"
                   "name = \"Asset A\"\n"
                   "[[feature]]\n"
                   "id = \"asset-b\"\n"
                   "name = \"Asset B\"\n"
                   "[[feature]]\n"
                   "id = \"asset-c\"\n"
                   "name = \"Asset C\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-a\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 100\n"
                   "file = \"assets/a.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_a) + "\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-b\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 102\n"
                   "file = \"assets/b.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_b) + "\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-c\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 102\n"
                   "file = \"assets/c.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_c) + "\"\n"));
    check(feature_reload.scan(&error), error.c_str());
    const ModPackage* overlay_package =
        feature_reload.selected_package("overlay.mod");
    check(overlay_package && overlay_package->overlays.size() == 3 &&
              overlay_package->overlays[0].size == overlay_a.size(),
          "manifest scan must verify and retain overlay metadata");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-a", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-c", true, &error), error.c_str());
    ModResolution overlay_compatible =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(overlay_compatible.ok && overlay_compatible.overlays.size() == 2,
          "partially overlapping file overlays with matching payload bytes "
          "must compose");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", true, &error), error.c_str());
    ModResolution overlay_collision =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(!overlay_collision.ok &&
              overlay_collision.diagnostics.size() == 1 &&
              overlay_collision.diagnostics[0].feature_id == "asset-b" &&
              overlay_collision.diagnostics[0].other_feature_id == "asset-a",
          "overlapping file overlays must identify both owning features");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", false, &error), error.c_str());
    ModResolution one_overlay =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(one_overlay.ok && one_overlay.overlays.size() == 2 &&
              one_overlay.overlays[0].payload == overlay_a &&
              one_overlay.overlays[1].payload == overlay_c,
          "compatible enabled overlay payloads must remain in the plan");

    write_text(root / "feature-derived.toml",
               manifest("bad.derived", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"bad\"\n"
                   "name = \"Bad\"\n"
                   "[[derived_disc]]\n"
                   "patch = \"bad.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 1\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"));
    ModPackage feature_derived;
    check(!ModPackageManager::read_manifest(
              root / "feature-derived.toml", feature_derived, &error),
          "feature-style packages must reject derived-disc operations");

    ModPackage invalid;
    write_text(root / "bad.toml",
               "format_version=1\nid=\"../bad\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
               "[[target]]\ngame_id=\"SLUS-TEST\"\n");
    check(!ModPackageManager::read_manifest(root / "bad.toml", invalid, &error),
          "unsafe package id must be rejected");

    fs::remove_all(root, ec);
    if (failures) {
        std::cerr << failures << " mod package test(s) failed\n";
        return 1;
    }
    std::cout << "mod package tests passed\n";
    return 0;
}
