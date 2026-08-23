#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace PSXRecompV4 {

enum class ModOptionType {
    Boolean,
    Choice,
    Integer,
};

struct ModChoice {
    std::string value;
    std::string label;
};

struct ModOption {
    std::string feature_id;
    std::string id;
    std::string label;
    std::string description;
    std::string group;
    ModOptionType type = ModOptionType::Boolean;
    std::string default_value;
    int64_t min_value = 0;
    int64_t max_value = 0;
    int64_t step = 1;
    std::vector<ModChoice> choices;
    /* Optional id of a BOOLEAN option in the same feature that overrides this
     * one. While that option is true this control is inert: the launcher greys
     * it out and plugins ignore its value. Models "tick Instant and the speed
     * box stops mattering" without inventing a second widget type. */
    std::string disabled_by;
};

struct ModFeature {
    std::string id;
    std::string name;
    std::string author;
    std::string description;
    std::string group = "General";
    bool default_enabled = false;
    bool legacy = false;
};

enum class ModConstraintKind {
    OrderedInteger,
    RequiresFeature,
};

enum class ModConstraintDirection {
    Nondecreasing,
    Nonincreasing,
};

struct ModConstraint {
    std::string feature_id;
    ModConstraintKind kind = ModConstraintKind::OrderedInteger;
    ModConstraintDirection direction =
        ModConstraintDirection::Nondecreasing;
    std::vector<std::string> options;
    std::string required_feature_id;
    std::string required_option_id;
    std::string required_value;
};

struct ModRequirement {
    std::string id;
    std::string version;
};

struct ModTarget {
    std::string game_id;
    std::string exe_sha256;
    std::string disc_sha256;
};

enum class ModPatchTarget {
    MainExe,
    DiscRaw,
    DiscUser,
};

enum class ModValueEncoding {
    U8,
    U16LE,
    U32LE,
    MipsLuiOriU32,
};

enum class ModIntegerPredicateOp {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

struct ModIntegerPredicate {
    bool present = false;
    std::string option;
    ModIntegerPredicateOp op = ModIntegerPredicateOp::Equal;
    int64_t value = 0;
};

struct ModPatchField {
    uint64_t offset = 0;
    std::vector<uint8_t> replacement;
    std::string replace_from_option;
    ModValueEncoding replace_encoding = ModValueEncoding::U8;
    int64_t replace_addend = 0;
};

struct ModPatch {
    std::string feature_id;
    ModPatchTarget target = ModPatchTarget::MainExe;
    uint64_t location = 0; /* guest address or canonical disc-stream byte offset */
    std::vector<uint8_t> expected;
    std::vector<uint8_t> replacement;
    std::string replace_from_option;
    ModValueEncoding replace_encoding = ModValueEncoding::U8;
    uint64_t replace_offset = 0;
    int64_t replace_addend = 0;
    bool replace_omit_when_default = false;
    std::string when_option;
    std::string when_value;
    std::map<std::string, std::string> when;
    ModIntegerPredicate when_integer;
    std::vector<ModPatchField> fields;
    int64_t order = 0;
};

struct ModOverlay {
    std::string feature_id;
    ModPatchTarget target = ModPatchTarget::DiscRaw;
    uint64_t location = 0;
    std::filesystem::path file;
    std::string sha256;
    std::string expected_sha256;
    uint64_t size = 0;
    std::map<std::string, std::string> when;
    struct FeaturePredicate {
        bool present = false;
        std::string package_id;
        std::string feature_id;
        bool enabled = true;
    } when_feature;
    int64_t order = 0;
};

struct ModPlugin {
    std::string feature_id;
    std::string id;
    std::map<std::string, std::string> when;
    int64_t order = 0;
};

struct ModDerivedDisc {
    std::string kind = "vcdiff";
    std::filesystem::path patch;
    std::string patch_sha256;
    uint64_t output_size = 0;
    std::string output_sha256;
    std::string when_option;
    std::string when_value;
    std::map<std::string, std::string> when;
};

struct ModAuthorLink {
    std::string name;
    std::string url;
};

struct ModPackage {
    uint32_t format_version = 0;
    std::string id;
    std::string version;
    std::string name;
    std::string author;
    std::vector<ModAuthorLink> author_links;
    std::string description;
    std::string license;
    std::string source_name;
    std::string source_url;
    std::string resolver = "declarative";
    std::string save_compatibility = "shared";
    std::filesystem::path root;
    std::vector<ModTarget> targets;
    std::vector<ModRequirement> dependencies;
    std::vector<std::string> conflicts;
    std::vector<ModFeature> features;
    std::vector<ModOption> options;
    std::vector<ModConstraint> constraints;
    std::vector<ModPatch> patches;
    std::vector<ModOverlay> overlays;
    std::vector<ModPlugin> plugins;
    std::vector<ModDerivedDisc> derived_discs;
};

struct ModFeatureSelection {
    bool enabled = false;
    bool has_enabled = false;
    std::map<std::string, std::string> values;
};

struct ModSelection {
    /* v1 migration state. Feature-style manifests do not use these fields. */
    bool enabled = false;
    std::string version;
    std::map<std::string, std::string> values;
    std::map<std::string, ModFeatureSelection> features;
};

struct ModResolution {
    bool ok = false;
    std::string fingerprint;
    std::vector<const ModPackage*> ordered;
    struct Write {
        struct Field {
            uint64_t offset = 0;
            std::vector<uint8_t> replacement;
        };
        ModPatchTarget target = ModPatchTarget::MainExe;
        uint64_t location = 0;
        std::vector<uint8_t> expected;
        std::vector<uint8_t> replacement;
        std::vector<Field> fields;
        std::string package_id;
        std::string feature_id;
    };
    std::vector<Write> writes;
    struct Overlay {
        ModPatchTarget target = ModPatchTarget::DiscRaw;
        uint64_t location = 0;
        std::vector<uint8_t> payload;
        std::string payload_sha256;
        std::string expected_sha256;
        std::string package_id;
        std::string feature_id;
    };
    std::vector<Overlay> overlays;
    struct DerivedDisc {
        std::string kind;
        std::filesystem::path patch;
        std::string patch_sha256;
        uint64_t output_size = 0;
        std::string output_sha256;
        std::string package_id;
    };
    std::vector<DerivedDisc> derived_discs;
    struct Plugin {
        std::string id;
        std::string package_id;
        std::string feature_id;
    };
    std::vector<Plugin> plugins;
    struct Diagnostic {
        std::string message;
        std::string resource;
        std::string package_id;
        std::string feature_id;
        std::string other_package_id;
        std::string other_feature_id;
    };
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> errors;
};

struct ModBuiltinResolverContext {
    const std::map<std::string, const ModPackage*>* active_packages = nullptr;
    const std::map<std::string, ModSelection>* selections = nullptr;
};

using ModBuiltinResolver = std::function<bool(
    const ModPackage& package,
    const ModSelection& selection,
    const ModBuiltinResolverContext& context,
    std::vector<ModResolution::Write>& writes,
    std::vector<std::string>& errors)>;

class ModPackageManager {
public:
    explicit ModPackageManager(std::filesystem::path mods_root = {});

    void set_root(std::filesystem::path mods_root);
    const std::filesystem::path& root() const { return root_; }

    bool scan(std::string* error = nullptr);
    bool load_state(std::string* error = nullptr);
    bool save_state(std::string* error = nullptr) const;

    bool install_archive(const std::filesystem::path& archive,
                         std::string* installed_id = nullptr,
                         std::string* installed_version = nullptr,
                         std::string* error = nullptr);
    bool install_archive_bytes(const std::uint8_t* data, size_t size,
                               std::string* installed_id = nullptr,
                               std::string* installed_version = nullptr,
                               std::string* error = nullptr);
    bool export_archive(const std::string& id, const std::string& version,
                        std::vector<std::uint8_t>& out,
                        std::string* error = nullptr) const;
    bool remove_version(const std::string& id, const std::string& version,
                        std::string* error = nullptr);

    bool set_enabled(const std::string& id, bool enabled, std::string* error = nullptr);
    bool select_version(const std::string& id, const std::string& version,
                        std::string* error = nullptr);
    bool set_option(const std::string& id, const std::string& option,
                    const std::string& value, std::string* error = nullptr);
    bool set_feature_enabled(const std::string& package_id,
                             const std::string& feature_id, bool enabled,
                             std::string* error = nullptr);
    bool set_feature_option(const std::string& package_id,
                            const std::string& feature_id,
                            const std::string& option_id,
                            const std::string& value,
                            std::string* error = nullptr);

    const std::map<std::string, std::map<std::string, ModPackage>>& packages() const {
        return packages_;
    }
    const std::map<std::string, ModSelection>& selections() const { return selections_; }
    const ModPackage* selected_package(const std::string& id) const;
    const ModFeature* selected_feature(const std::string& package_id,
                                       const std::string& feature_id) const;
    bool feature_enabled(const std::string& package_id,
                         const std::string& feature_id) const;
    std::string feature_option_value(const std::string& package_id,
                                     const std::string& feature_id,
                                     const std::string& option_id) const;

    ModResolution resolve(const std::string& game_id,
                          const std::string& exe_sha256 = {},
                          const std::string& disc_sha256 = {}) const;

    static bool read_manifest(const std::filesystem::path& path, ModPackage& out,
                              std::string* error = nullptr);

private:
    std::filesystem::path root_;
    std::map<std::string, std::map<std::string, ModPackage>> packages_;
    std::map<std::string, ModSelection> selections_;
};

bool mod_register_builtin_resolver(const std::string& id, ModBuiltinResolver resolver);
void mod_clear_builtin_resolvers_for_tests();
bool mod_register_activation_plugin(const std::string& id, void (*callback)(void));
bool mod_register_vblank_plugin(const std::string& id, void (*callback)(void));
/* Mark a function-entry plugin id as trusted for package resolve. Callbacks
 * remain in mod_runtime; same id may bind multiple guest addresses. */
bool mod_register_function_entry_plugin_id(const std::string& id);
bool mod_plugin_registered(const std::string& id);
void mod_invoke_activation_plugin(const std::string& id);
void mod_invoke_vblank_plugin(const std::string& id);
void mod_clear_plugins_for_tests();

} // namespace PSXRecompV4
