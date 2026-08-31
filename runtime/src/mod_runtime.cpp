#include "mod_runtime.h"

#include "disc_path.h"
#include "func_override.h"
#include "iso_reader.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "gpu.h"
#include "psx_sha256.h"

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" uint8_t psx_read_byte(uint32_t addr);
extern "C" void psx_write_byte(uint32_t addr, uint8_t value);
extern "C" uint16_t psx_read_half(uint32_t addr);
extern "C" void psx_write_half(uint32_t addr, uint16_t value);
extern "C" uint32_t psx_read_word(uint32_t addr);
extern "C" void psx_write_word(uint32_t addr, uint32_t value);
extern "C" uint32_t psx_mod_memory_alloc(uint32_t size, uint32_t alignment);
extern "C" uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t size,
                                                  uint32_t alignment);
extern "C" int psx_ws_x_margin(void);
extern "C" void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len);
extern "C" int fntrace_is_game_started(void);

namespace PSXRecompV4 {
namespace {

struct RuntimeMods {
    ModPackageManager manager;
    ModResolution plan;
    ModResolution validation;
    std::map<uint32_t, std::vector<size_t>> raw_disc_index;
    std::map<uint32_t, std::vector<size_t>> user_disc_index;
    std::map<uint32_t, std::vector<size_t>> raw_overlay_index;
    std::map<uint32_t, std::vector<size_t>> user_overlay_index;
    std::string game_id;
    std::string error;
    std::string exe_sha256;
    std::string disc_sha256;
    std::filesystem::path disc_path;
    std::filesystem::path effective_disc_path;
    uint32_t entry_phys = 0;
    bool initialized = false;
    bool main_applied = false;
    bool disc_enabled = false;
    bool disc_guard_failed = false;
    const ModResolution::Plugin* current_plugin = nullptr;
};

RuntimeMods& state() {
    static RuntimeMods value;
    return value;
}

struct FunctionEntryPlugin {
    std::string id;
    uint32_t address = 0;
    PSXModFunctionEntryCallback callback = nullptr;
};

std::vector<FunctionEntryPlugin>& function_entry_plugins() {
    static std::vector<FunctionEntryPlugin> value;
    return value;
}

/* Function overrides queue here at constructor time and are ARMED into the
 * func_override tier only for plugins the resolved package plan selects —
 * the same gating as vblank/activation callbacks. `id` is the full
 * registered name (may carry a ":label" suffix, kept for diagnostics);
 * `plugin` is the manifest-facing part gating matches on. */
struct FunctionOverridePlugin {
    std::string id;
    std::string plugin;
    uint32_t address = 0;
    PSXModFunctionOverrideFn fn = nullptr;
    uint32_t guard[FO_MAX_GUARD_WORDS] = {0, 0, 0, 0};
    int n_guard = 0;
    int32_t credit = 0;
    bool armed = false;
};

std::vector<FunctionOverridePlugin>& function_override_plugins() {
    static std::vector<FunctionOverridePlugin> value;
    return value;
}

const ModPackage* selected_package(const std::string& id) {
    return state().manager.selected_package(id);
}

bool package_has_enabled_feature(const ModPackage& package) {
    return std::any_of(
        package.features.begin(), package.features.end(),
        [&](const ModFeature& feature) {
            return state().manager.feature_enabled(package.id, feature.id);
        });
}

std::string selected_value(const ModPackage& package, const ModOption& option) {
    const auto selection = state().manager.selections().find(package.id);
    if (selection != state().manager.selections().end()) {
        const auto value = selection->second.values.find(option.id);
        if (value != selection->second.values.end()) return value->second;
    }
    return option.default_value;
}

/* Resolve the manifest's disabled_by link against the CURRENT selection: the
 * named boolean sibling being true makes this option inert. Both the launcher
 * (greys the control) and psx_mod_option_value (returns the default instead of
 * a stale value) go through this, so the UI and the plugins can never disagree
 * about whether a control counts. */
bool option_is_disabled(const ModPackage& package, const ModOption& option) {
    if (option.disabled_by.empty()) return false;
    for (const ModOption& other : package.options) {
        if (other.feature_id != option.feature_id ||
            other.id != option.disabled_by)
            continue;
        return selected_value(package, other) == "true";
    }
    return false;
}

void build_disc_index(RuntimeMods& s) {
    s.raw_disc_index.clear();
    s.user_disc_index.clear();
    s.raw_overlay_index.clear();
    s.user_overlay_index.clear();
    for (size_t i = 0; i < s.plan.writes.size(); ++i) {
        const ModResolution::Write& write = s.plan.writes[i];
        if (write.target == ModPatchTarget::DiscRaw)
            s.raw_disc_index[(uint32_t)(write.location / 2352)].push_back(i);
        else if (write.target == ModPatchTarget::DiscUser)
            s.user_disc_index[(uint32_t)(write.location / 2048)].push_back(i);
    }
    for (size_t i = 0; i < s.plan.overlays.size(); ++i) {
        const ModResolution::Overlay& overlay = s.plan.overlays[i];
        const uint64_t sector_size =
            overlay.target == ModPatchTarget::DiscRaw ? 2352 : 2048;
        auto& index = overlay.target == ModPatchTarget::DiscRaw
            ? s.raw_overlay_index : s.user_overlay_index;
        const uint64_t first = overlay.location / sector_size;
        const uint64_t last =
            (overlay.location + overlay.payload.size() - 1) / sector_size;
        for (uint64_t lba = first; lba <= last; ++lba)
            index[(uint32_t)lba].push_back(i);
    }
}

void set_error(const std::string& error) {
    state().error = error;
}

void apply_main_write(const ModResolution::Write& write) {
    if (write.fields.empty()) {
        for (size_t i = 0; i < write.replacement.size(); ++i)
            psx_write_byte((uint32_t)write.location + (uint32_t)i,
                           write.replacement[i]);
        dirty_ram_mark_executable_range(
            (uint32_t)write.location & 0x1FFFFFFFu,
            (uint32_t)write.replacement.size());
        return;
    }
    for (const ModResolution::Write::Field& field : write.fields) {
        for (size_t i = 0; i < field.replacement.size(); ++i)
            psx_write_byte(
                (uint32_t)write.location +
                    (uint32_t)field.offset + (uint32_t)i,
                field.replacement[i]);
        dirty_ram_mark_executable_range(
            ((uint32_t)write.location +
             (uint32_t)field.offset) & 0x1FFFFFFFu,
            (uint32_t)field.replacement.size());
    }
}

bool restored_main_matches_plan(const RuntimeMods& s, uint32_t& failed_at) {
    std::map<uint32_t, uint8_t> desired;
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        if (write.fields.empty()) {
            for (size_t i = 0; i < write.replacement.size(); ++i)
                desired[(uint32_t)write.location + (uint32_t)i] =
                    write.replacement[i];
        } else {
            for (const ModResolution::Write::Field& field : write.fields) {
                for (size_t i = 0; i < field.replacement.size(); ++i)
                    desired[
                        (uint32_t)write.location +
                        (uint32_t)field.offset + (uint32_t)i] =
                        field.replacement[i];
            }
        }
    }

    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        for (size_t i = 0; i < write.expected.size(); ++i) {
            const uint32_t address =
                (uint32_t)write.location + (uint32_t)i;
            const uint8_t observed = psx_read_byte(address);
            if (observed == write.expected[i]) continue;
            const auto replacement = desired.find(address);
            if (replacement != desired.end() &&
                observed == replacement->second)
                continue;
            failed_at = address;
            return false;
        }
    }
    return true;
}

bool sha256_file(const std::filesystem::path& path, std::string& out,
                 std::string* error) {
    out.clear();
    if (path.empty()) return true;
    const DiscPathResolution resolved = resolve_disc_path(path);
    const std::filesystem::path input = resolved.data;
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    std::string extension = resolved.mount.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".chd") {
        PS1::ISOReader disc;
        if (!disc.Open(resolved.mount.string())) {
            if (error) *error =
                "cannot decode image fingerprint: " + resolved.mount.string();
            return false;
        }
        std::array<uint8_t, 2352> sector{};
        for (uint32_t lba = 0; lba < disc.GetSectorCount(); ++lba) {
            if (!disc.ReadRawSector(lba, sector.data())) {
                if (error) *error =
                    "cannot finish decoding image fingerprint: " +
                    resolved.mount.string();
                return false;
            }
            psx_sha256_update(&hash, sector.data(), sector.size());
        }
        uint8_t digest[32];
        psx_sha256_final(&hash, digest);
        std::ostringstream text;
        for (uint8_t byte : digest)
            text << std::hex << std::setw(2) << std::setfill('0')
                 << (unsigned)byte;
        out = text.str();
        return true;
    }

    std::array<uint8_t, 1024 * 1024> buffer{};
    std::ifstream file(input, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot fingerprint image: " + input.string();
        return false;
    }
    while (file) {
        file.read((char*)buffer.data(), (std::streamsize)buffer.size());
        const std::streamsize got = file.gcount();
        if (got > 0) psx_sha256_update(&hash, buffer.data(), (size_t)got);
    }
    if (!file.eof()) {
        if (error) *error =
            "cannot finish fingerprinting image: " + input.string();
        return false;
    }
    uint8_t digest[32];
    psx_sha256_final(&hash, digest);
    std::ostringstream text;
    for (uint8_t byte : digest)
        text << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    out = text.str();
    return true;
}

std::filesystem::path raw_image_path(const std::filesystem::path& path,
                                     std::string* error) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension != ".cue") return path;
    std::ifstream cue(path);
    if (!cue) {
        if (error) *error = "cannot open disc CUE: " + path.string();
        return {};
    }
    std::string line;
    while (std::getline(cue, line)) {
        size_t at = line.find_first_not_of(" \t");
        if (at == std::string::npos || line.size() - at < 4) continue;
        std::string keyword = line.substr(at, 4);
        std::transform(keyword.begin(), keyword.end(), keyword.begin(),
            [](unsigned char c) { return (char)std::toupper(c); });
        if (keyword != "FILE") continue;
        at = line.find_first_not_of(" \t", at + 4);
        if (at == std::string::npos) continue;
        std::string name;
        if (line[at] == '"') {
            const size_t end = line.find('"', at + 1);
            if (end == std::string::npos) continue;
            name = line.substr(at + 1, end - at - 1);
        } else {
            const size_t end = line.find_first_of(" \t", at);
            name = line.substr(at, end - at);
        }
        return (path.parent_path() / name).lexically_normal();
    }
    if (error) *error = "disc CUE has no source file: " + path.string();
    return {};
}

bool sha256_disc_range(const std::filesystem::path& image,
                       ModPatchTarget target, uint64_t location, size_t size,
                       std::string& out, std::string* error) {
    std::string extension = image.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".chd") {
        PS1::ISOReader disc;
        if (!disc.Open(image.string())) {
            if (error) *error = "cannot open stock CHD range: " + image.string();
            return false;
        }
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        std::array<uint8_t, 2352> sector{};
        size_t remaining = size;
        uint64_t at = location;
        const uint64_t sector_size =
            target == ModPatchTarget::DiscRaw ? 2352u : 2048u;
        while (remaining != 0) {
            const uint64_t lba64 = at / sector_size;
            if (lba64 >= disc.GetSectorCount()) {
                if (error) *error = "overlay expected range exceeds stock CHD";
                return false;
            }
            const size_t within = (size_t)(at % sector_size);
            const bool read_ok =
                target == ModPatchTarget::DiscRaw
                    ? disc.ReadRawSector((uint32_t)lba64, sector.data())
                    : disc.ReadSector((uint32_t)lba64, sector.data());
            if (!read_ok) {
                if (error) *error = "cannot decode stock CHD overlay range";
                return false;
            }
            const size_t chunk =
                std::min(remaining, (size_t)sector_size - within);
            psx_sha256_update(&hash, sector.data() + within, chunk);
            at += chunk;
            remaining -= chunk;
        }
        uint8_t digest[32];
        psx_sha256_final(&hash, digest);
        std::ostringstream text;
        for (uint8_t byte : digest)
            text << std::hex << std::setw(2) << std::setfill('0')
                 << (unsigned)byte;
        out = text.str();
        return true;
    }

    const std::filesystem::path source = raw_image_path(image, error);
    if (source.empty()) return false;
    std::ifstream file(source, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open stock image range: " + source.string();
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff file_size = file.tellg();
    if (file_size < 0) {
        if (error) *error = "cannot size stock image: " + source.string();
        return false;
    }
    psx_sha256_ctx hash;
    psx_sha256_init(&hash);
    std::array<uint8_t, 2048> bytes{};
    size_t remaining = size;
    uint64_t at = location;
    const bool raw_source = file_size > 0 &&
        ((uint64_t)file_size % 2352u) == 0;
    while (remaining != 0) {
        uint64_t physical = at;
        size_t chunk = remaining;
        if (target == ModPatchTarget::DiscUser && raw_source) {
            const uint64_t lba = at / 2048u;
            const size_t within = (size_t)(at % 2048u);
            physical = lba * 2352u + 24u + within;
            chunk = std::min(chunk, 2048u - within);
        }
        chunk = std::min(chunk, bytes.size());
        if (physical > (uint64_t)file_size ||
            chunk > (uint64_t)file_size - physical) {
            if (error) *error = "overlay expected range exceeds stock image";
            return false;
        }
        file.clear();
        file.seekg((std::streamoff)physical);
        if (!file.read((char*)bytes.data(), (std::streamsize)chunk)) {
            if (error) *error = "cannot read stock image overlay range";
            return false;
        }
        psx_sha256_update(&hash, bytes.data(), chunk);
        at += chunk;
        remaining -= chunk;
    }
    uint8_t digest[32];
    psx_sha256_final(&hash, digest);
    std::ostringstream text;
    for (uint8_t byte : digest)
        text << std::hex << std::setw(2) << std::setfill('0') << (unsigned)byte;
    out = text.str();
    return true;
}

#if defined(_WIN32)
std::wstring quote_windows_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(c);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
#endif

bool run_xdelta_decode(const std::filesystem::path& executable,
                       const std::filesystem::path& source,
                       const std::filesystem::path& patch,
                       const std::filesystem::path& output,
                       std::string* error) {
#if defined(_WIN32)
    std::wstring command =
        quote_windows_argument(executable.wstring()) + L" -f -n -d -s " +
        quote_windows_argument(source.wstring()) + L" " +
        quote_windows_argument(patch.wstring()) + L" " +
        quote_windows_argument(output.wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.wstring().c_str(), mutable_command.data(),
                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        if (error) *error = "cannot start trusted xdelta3 decoder (Windows error " +
            std::to_string((unsigned long)GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        if (error) *error =
            "trusted xdelta3 decoder failed with exit code " + std::to_string(exit_code);
        return false;
    }
    return true;
#else
    const pid_t child = fork();
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), "-f", "-n", "-d", "-s",
              source.c_str(), patch.c_str(), output.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (child < 0) {
        if (error) *error = "cannot start trusted xdelta3 decoder";
        return false;
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        if (error) *error = "trusted xdelta3 decoder failed";
        return false;
    }
    return true;
#endif
}

bool valid_cached_disc(const std::filesystem::path& path,
                       const ModResolution::DerivedDisc& derived) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) &&
           std::filesystem::file_size(path, ec) == derived.output_size && !ec;
}

bool materialize_derived_disc(RuntimeMods& s, const ModResolution& plan,
                              std::filesystem::path& out, std::string* error) {
    out.clear();
    if (plan.derived_discs.empty()) return true;
    const ModResolution::DerivedDisc& derived = plan.derived_discs.front();
    std::string digest;
    if (!sha256_file(derived.patch, digest, error) ||
        digest != derived.patch_sha256) {
        if (error && error->empty())
            *error = derived.package_id + ": derived-disc patch checksum failed";
        else if (error && digest != derived.patch_sha256)
            *error = derived.package_id + ": derived-disc patch checksum failed";
        return false;
    }
    const std::filesystem::path cache_root = s.manager.root() / "cache";
    const std::filesystem::path cached = cache_root / (plan.fingerprint + ".bin");
    if (valid_cached_disc(cached, derived)) {
        out = cached;
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_root, ec);
    if (ec) {
        if (error) *error = "cannot create derived-disc cache: " + ec.message();
        return false;
    }
    const char* override_tool = std::getenv("PSXRECOMP_XDELTA3");
    const std::filesystem::path decoder =
        override_tool && override_tool[0]
            ? std::filesystem::path(override_tool)
#if defined(_WIN32)
            : s.manager.root().parent_path() / "xdelta3.exe";
#else
            : s.manager.root().parent_path() / "xdelta3";
#endif
    if (!std::filesystem::is_regular_file(decoder, ec)) {
        if (error) *error =
            "this mod needs the trusted xdelta3 decoder, but it is missing: " +
            decoder.string();
        return false;
    }
    const std::filesystem::path source = raw_image_path(s.disc_path, error);
    if (source.empty()) return false;
#if defined(_WIN32)
    const unsigned long process_id = GetCurrentProcessId();
#else
    const unsigned long process_id = (unsigned long)getpid();
#endif
    const std::filesystem::path temporary =
        cache_root / (plan.fingerprint + ".tmp." + std::to_string(process_id));
    std::filesystem::remove(temporary, ec);
    std::fprintf(stdout, "psxrecomp: building derived disc for %s...\n",
                 derived.package_id.c_str());
    if (!run_xdelta_decode(decoder, source, derived.patch, temporary, error)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    if (!valid_cached_disc(temporary, derived)) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = derived.package_id +
            ": derived disc has the wrong output size";
        return false;
    }
    if (!sha256_file(temporary, digest, error) || digest != derived.output_sha256) {
        std::filesystem::remove(temporary, ec);
        if (error && digest != derived.output_sha256)
            *error = derived.package_id + ": derived disc checksum failed";
        return false;
    }
    std::filesystem::rename(temporary, cached, ec);
    if (ec) {
        std::filesystem::remove(cached, ec);
        ec.clear();
        std::filesystem::rename(temporary, cached, ec);
    }
    if (ec) {
        std::filesystem::remove(temporary, ec);
        if (error) *error = "cannot publish derived-disc cache: " + ec.message();
        return false;
    }
    std::fprintf(stdout, "psxrecomp: cached derived disc %s\n", cached.string().c_str());
    out = cached;
    return true;
}

#if defined(RECOMP_LAUNCHER)
void copy_text(char* out, size_t capacity, const std::string& value) {
    if (!out || capacity == 0) return;
    std::snprintf(out, capacity, "%s", value.c_str());
}

int provider_package_count(void*) {
    return (int)state().manager.packages().size();
}

int provider_package_get(void*, int index, RecompLauncherCModPackage* out) {
    if (!out || index < 0) return 0;
    const auto& packages = state().manager.packages();
    if ((size_t)index >= packages.size()) return 0;
    auto item = packages.begin();
    std::advance(item, index);
    const ModPackage* package = selected_package(item->first);
    if (!package) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), package->id);
    copy_text(out->version, sizeof(out->version), package->version);
    copy_text(out->name, sizeof(out->name), package->name);
    copy_text(out->author, sizeof(out->author), package->author);
    out->author_link_count = std::min(
        (int)package->author_links.size(), RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX);
    for (int i = 0; i < out->author_link_count; ++i) {
        copy_text(out->author_links[i].name, sizeof(out->author_links[i].name),
                  package->author_links[(size_t)i].name);
        copy_text(out->author_links[i].url, sizeof(out->author_links[i].url),
                  package->author_links[(size_t)i].url);
    }
    copy_text(out->description, sizeof(out->description), package->description);
    copy_text(out->license, sizeof(out->license), package->license);
    copy_text(out->source_name, sizeof(out->source_name), package->source_name);
    copy_text(out->source_url, sizeof(out->source_url), package->source_url);
    out->enabled = package_has_enabled_feature(*package);
    out->option_count = (int)package->options.size();
    out->removable = !out->enabled;
    return 1;
}

int provider_option_get(void*, const char* package_id, int index,
                        RecompLauncherCModOption* out) {
    if (!package_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package || (size_t)index >= package->options.size()) return 0;
    const ModOption& option = package->options[(size_t)index];
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), option.id);
    copy_text(out->label, sizeof(out->label), option.label);
    copy_text(out->description, sizeof(out->description), option.description);
    copy_text(out->group, sizeof(out->group), option.group);
    copy_text(out->value, sizeof(out->value), selected_value(*package, option));
    copy_text(out->default_value, sizeof(out->default_value), option.default_value);
    out->type = option.type == ModOptionType::Boolean ? RECOMP_MOD_OPTION_BOOLEAN :
                option.type == ModOptionType::Choice ? RECOMP_MOD_OPTION_CHOICE :
                                                       RECOMP_MOD_OPTION_INTEGER;
    out->min_value = option.min_value;
    out->max_value = option.max_value;
    out->step = option.step;
    out->choice_count = (int)option.choices.size();
    out->disabled = option_is_disabled(*package, option) ? 1 : 0;
    return 1;
}

int provider_choice_get(void*, const char* package_id, const char* option_id,
                        int index, RecompLauncherCModChoice* out) {
    if (!package_id || !option_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    const auto option = std::find_if(package->options.begin(), package->options.end(),
        [&](const ModOption& value) { return value.id == option_id; });
    if (option == package->options.end() || (size_t)index >= option->choices.size()) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->value, sizeof(out->value), option->choices[(size_t)index].value);
    copy_text(out->label, sizeof(out->label), option->choices[(size_t)index].label);
    return 1;
}

template <typename Callback>
int mutate(Callback callback);

bool provider_feature_at(int index, const ModPackage*& package,
                         const ModFeature*& feature) {
    if (index < 0) return false;
    for (const auto& [package_id, versions] : state().manager.packages()) {
        (void)versions;
        const ModPackage* selected = selected_package(package_id);
        if (!selected) continue;
        for (const ModFeature& candidate : selected->features) {
            if (index-- == 0) {
                package = selected;
                feature = &candidate;
                return true;
            }
        }
    }
    return false;
}

std::vector<const ModOption*> provider_feature_options(
    const ModPackage& package, const std::string& feature_id) {
    std::vector<const ModOption*> out;
    for (const ModOption& option : package.options)
        if (option.feature_id == feature_id) out.push_back(&option);
    return out;
}

bool diagnostic_matches(const ModResolution::Diagnostic& diagnostic,
                        const std::string& package_id,
                        const std::string& feature_id) {
    return (diagnostic.package_id == package_id &&
            diagnostic.feature_id == feature_id) ||
           (diagnostic.other_package_id == package_id &&
            diagnostic.other_feature_id == feature_id);
}

int provider_feature_count(void*) {
    int count = 0;
    for (const auto& [package_id, versions] : state().manager.packages()) {
        (void)versions;
        const ModPackage* package = selected_package(package_id);
        if (package) count += (int)package->features.size();
    }
    return count;
}

int provider_feature_get(void*, int index, RecompLauncherCModFeature* out) {
    if (!out) return 0;
    const ModPackage* package = nullptr;
    const ModFeature* feature = nullptr;
    if (!provider_feature_at(index, package, feature)) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), feature->id);
    copy_text(out->package_id, sizeof(out->package_id), package->id);
    copy_text(out->package_version, sizeof(out->package_version), package->version);
    copy_text(out->package_name, sizeof(out->package_name), package->name);
    copy_text(out->name, sizeof(out->name), feature->name);
    copy_text(out->author, sizeof(out->author),
              feature->author.empty() ? package->author : feature->author);
    out->author_link_count = std::min(
        (int)package->author_links.size(), RECOMP_LAUNCHER_MOD_AUTHOR_LINK_MAX);
    for (int i = 0; i < out->author_link_count; ++i) {
        copy_text(out->author_links[i].name, sizeof(out->author_links[i].name),
                  package->author_links[(size_t)i].name);
        copy_text(out->author_links[i].url, sizeof(out->author_links[i].url),
                  package->author_links[(size_t)i].url);
    }
    copy_text(out->description, sizeof(out->description), feature->description);
    copy_text(out->source_name, sizeof(out->source_name), package->source_name);
    copy_text(out->source_url, sizeof(out->source_url), package->source_url);
    copy_text(out->group, sizeof(out->group), feature->group);
    out->hidden = feature->hidden ? 1 : 0;
    out->enabled =
        state().manager.feature_enabled(package->id, feature->id) ? 1 : 0;
    out->option_count =
        (int)provider_feature_options(*package, feature->id).size();
    for (const ModResolution::Diagnostic& diagnostic :
         state().validation.diagnostics) {
        if (!diagnostic_matches(diagnostic, package->id, feature->id)) continue;
        out->has_error = 1;
        copy_text(out->status, sizeof(out->status), diagnostic.message);
        break;
    }
    return 1;
}

int provider_feature_option_get(void*, const char* package_id,
                                const char* feature_id, int index,
                                RecompLauncherCModOption* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    const auto options = provider_feature_options(*package, feature_id);
    if ((size_t)index >= options.size()) return 0;
    const ModOption& option = *options[(size_t)index];
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, sizeof(out->id), option.id);
    copy_text(out->label, sizeof(out->label), option.label);
    copy_text(out->description, sizeof(out->description), option.description);
    copy_text(out->group, sizeof(out->group), option.group);
    copy_text(out->value, sizeof(out->value),
              state().manager.feature_option_value(
                  package_id, feature_id, option.id));
    copy_text(out->default_value, sizeof(out->default_value),
              option.default_value);
    out->type = option.type == ModOptionType::Boolean
        ? RECOMP_MOD_OPTION_BOOLEAN
        : option.type == ModOptionType::Choice
            ? RECOMP_MOD_OPTION_CHOICE : RECOMP_MOD_OPTION_INTEGER;
    out->min_value = option.min_value;
    out->max_value = option.max_value;
    out->step = option.step;
    out->choice_count = (int)option.choices.size();
    out->disabled = option_is_disabled(*package, option) ? 1 : 0;
    return 1;
}

int provider_feature_choice_get(void*, const char* package_id,
                                const char* feature_id,
                                const char* option_id, int index,
                                RecompLauncherCModChoice* out) {
    if (!package_id || !feature_id || !option_id || !out || index < 0)
        return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    const auto option = std::find_if(
        package->options.begin(), package->options.end(),
        [&](const ModOption& value) {
            return value.feature_id == feature_id && value.id == option_id;
        });
    if (option == package->options.end() ||
        (size_t)index >= option->choices.size()) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->value, sizeof(out->value),
              option->choices[(size_t)index].value);
    copy_text(out->label, sizeof(out->label),
              option->choices[(size_t)index].label);
    return 1;
}

int provider_feature_enable(void*, const char* package_id,
                            const char* feature_id, int enabled) {
    if (!package_id || !feature_id) return 0;
    return mutate([&](std::string& error) {
        const ModFeature* feature =
            state().manager.selected_feature(package_id, feature_id);
        if (feature && feature->legacy)
            return state().manager.set_enabled(
                package_id, enabled != 0, &error);
        return state().manager.set_feature_enabled(
            package_id, feature_id, enabled != 0, &error);
    });
}

int provider_feature_set_option(void*, const char* package_id,
                                const char* feature_id,
                                const char* option_id,
                                const char* value) {
    if (!package_id || !feature_id || !option_id || !value) return 0;
    return mutate([&](std::string& error) {
        const ModFeature* feature =
            state().manager.selected_feature(package_id, feature_id);
        if (feature && feature->legacy)
            return state().manager.set_option(
                package_id, option_id, value, &error);
        return state().manager.set_feature_option(
            package_id, feature_id, option_id, value, &error);
    });
}

int provider_feature_resource_count(void*, const char* package_id,
                                    const char* feature_id) {
    if (!package_id || !feature_id) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    return (int)std::count_if(
        package->resources.begin(), package->resources.end(),
        [&](const ModResource& resource) {
            return resource.feature_id == feature_id;
        });
}

int provider_feature_resource_get(void*, const char* package_id,
                                  const char* feature_id, int index,
                                  RecompLauncherCModResource* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    const ModPackage* package = selected_package(package_id);
    if (!package) return 0;
    for (const ModResource& resource : package->resources) {
        if (resource.feature_id != feature_id) continue;
        if (index-- != 0) continue;
        const std::filesystem::path path =
            state().manager.feature_resource_path(
                package_id, feature_id, resource.id);
        std::error_code ec;
        const bool directory =
            resource.format == "directory" || resource.format == "folder";
        const bool verified = !path.empty() &&
            (directory ? std::filesystem::is_directory(path, ec)
                       : std::filesystem::is_regular_file(path, ec));
        std::memset(out, 0, sizeof(*out));
        copy_text(out->id, sizeof(out->id), resource.id);
        copy_text(out->label, sizeof(out->label), resource.label);
        copy_text(out->description, sizeof(out->description),
                  resource.description);
        copy_text(out->path, sizeof(out->path), path.string());
        copy_text(out->status, sizeof(out->status),
                  path.empty() ? "Not selected" :
                      (verified ? "Selected" : "Selected path is missing"));
        copy_text(out->file_patterns, sizeof(out->file_patterns),
                  resource.file_patterns);
        copy_text(out->file_description, sizeof(out->file_description),
                  resource.file_description);
        out->required = resource.required ? 1 : 0;
        out->verified = verified ? 1 : 0;
        copy_text(out->format, sizeof(out->format), resource.format);
        return 1;
    }
    return 0;
}

int provider_feature_resource_set_path(void*, const char* package_id,
                                       const char* feature_id,
                                       const char* resource_id,
                                       const char* path) {
    if (!package_id || !feature_id || !resource_id || !path) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_feature_resource_path(
            package_id, feature_id, resource_id,
            std::filesystem::path(path), &error);
    });
}

int provider_diagnostic_count(void*, const char* package_id,
                              const char* feature_id) {
    if (!package_id || !feature_id) return 0;
    return (int)std::count_if(
        state().validation.diagnostics.begin(),
        state().validation.diagnostics.end(),
        [&](const ModResolution::Diagnostic& diagnostic) {
            return diagnostic_matches(diagnostic, package_id, feature_id);
        });
}

int provider_diagnostic_get(void*, const char* package_id,
                            const char* feature_id, int index,
                            RecompLauncherCModDiagnostic* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    for (const ModResolution::Diagnostic& diagnostic :
         state().validation.diagnostics) {
        if (!diagnostic_matches(diagnostic, package_id, feature_id)) continue;
        if (index-- != 0) continue;
        std::memset(out, 0, sizeof(*out));
        out->severity = 2;
        copy_text(out->resource, sizeof(out->resource), diagnostic.resource);
        copy_text(out->message, sizeof(out->message), diagnostic.message);
        const bool primary = diagnostic.package_id == package_id &&
                             diagnostic.feature_id == feature_id;
        copy_text(out->related_package_id, sizeof(out->related_package_id),
                  primary ? diagnostic.other_package_id :
                            diagnostic.package_id);
        copy_text(out->related_feature_id, sizeof(out->related_feature_id),
                  primary ? diagnostic.other_feature_id :
                            diagnostic.feature_id);
        return 1;
    }
    return 0;
}

int provider_version_count(void*, const char* package_id) {
    if (!package_id) return 0;
    const auto package = state().manager.packages().find(package_id);
    return package == state().manager.packages().end() ? 0 : (int)package->second.size();
}

int provider_version_get(void*, const char* package_id, int index,
                         RecompLauncherCModVersion* out) {
    if (!package_id || !out || index < 0) return 0;
    const auto package = state().manager.packages().find(package_id);
    if (package == state().manager.packages().end() ||
        (size_t)index >= package->second.size()) return 0;
    auto version = package->second.begin();
    std::advance(version, index);
    std::memset(out, 0, sizeof(*out));
    copy_text(out->version, sizeof(out->version), version->first);
    const ModPackage* selected = selected_package(package_id);
    out->selected = selected && selected->version == version->first;
    out->removable = !out->selected ||
                     !selected || !package_has_enabled_feature(*selected);
    return 1;
}

template <typename Callback>
int mutate(Callback callback) {
    std::string error;
    if (!callback(error)) {
        set_error(error);
        return 0;
    }
    if (!state().disc_path.empty())
        state().validation = state().manager.resolve(
            state().game_id, state().exe_sha256, state().disc_sha256);
    else
        state().validation = {};
    state().error.clear();
    return 1;
}

int provider_install(void*, const char* path) {
    if (!path) return 0;
    return mutate([&](std::string& error) {
        std::string id, version;
        if (!state().manager.install_archive(path, &id, &version, &error)) return false;
        if (!state().manager.scan(&error)) return false;
        return state().manager.select_version(id, version, &error);
    });
}

int provider_remove(void*, const char* id, const char* version) {
    if (!id || !version) return 0;
    return mutate([&](std::string& error) {
        return state().manager.remove_version(id, version, &error);
    });
}

int provider_enable(void*, const char* id, int enabled) {
    if (!id) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_enabled(id, enabled != 0, &error);
    });
}

int provider_select(void*, const char* id, const char* version) {
    if (!id || !version) return 0;
    return mutate([&](std::string& error) {
        return state().manager.select_version(id, version, &error);
    });
}

int provider_set_option(void*, const char* id, const char* option, const char* value) {
    if (!id || !option || !value) return 0;
    return mutate([&](std::string& error) {
        return state().manager.set_option(id, option, value, &error);
    });
}

int provider_commit(void*, const char* image_path) {
    std::string error;
    if (!mod_runtime_commit(image_path ? std::filesystem::path(image_path) :
                                      std::filesystem::path(), &error)) {
        set_error(error);
        return 0;
    }
    state().error.clear();
    return 1;
}

int provider_commit_netplay(void*, const char* image_path) {
    (void)image_path;
    std::string error;
    if (!mod_runtime_clear_for_netplay(&error)) {
        set_error(error);
        return 0;
    }
    state().error.clear();
    return 1;
}

const char* provider_error(void*) {
    return state().error.c_str();
}

RecompLauncherCModProvider provider = {
    nullptr,
    provider_package_count,
    provider_package_get,
    provider_option_get,
    provider_choice_get,
    provider_version_count,
    provider_version_get,
    provider_install,
    provider_remove,
    provider_enable,
    provider_select,
    provider_set_option,
    provider_commit,
    provider_error,
    provider_feature_count,
    provider_feature_get,
    provider_feature_option_get,
    provider_feature_choice_get,
    provider_feature_enable,
    provider_feature_set_option,
    provider_diagnostic_count,
    provider_diagnostic_get,
    nullptr, /* archive_extension — PSX defaults */
    nullptr, /* archive_description */
    provider_commit_netplay,
    provider_feature_resource_count,
    provider_feature_resource_get,
    provider_feature_resource_set_path,
};
#endif

} // namespace

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path,
                            std::string* error) {
    RuntimeMods& s = state();
    s.manager.set_root({});
    s.plan = {};
    s.validation = {};
    s.raw_disc_index.clear();
    s.user_disc_index.clear();
    s.raw_overlay_index.clear();
    s.user_overlay_index.clear();
    s.game_id.clear();
    s.error.clear();
    s.exe_sha256.clear();
    s.disc_sha256.clear();
    s.disc_path.clear();
    s.effective_disc_path.clear();
    s.entry_phys = 0;
    s.initialized = false;
    s.main_applied = false;
    s.disc_enabled = false;
    s.disc_guard_failed = false;
    s.manager.set_root(root);
    s.game_id = game_id;
    s.entry_phys = game_entry_pc & 0x1FFFFFFFu;
    if (!s.manager.scan(&s.error) || !s.manager.load_state(&s.error)) {
        if (error) *error = s.error;
        return false;
    }
    if (!sha256_file(exe_path, s.exe_sha256, &s.error)) {
        /* Release installs commonly do not carry a loose PS-X EXE; game-id and
         * expected-byte guards remain available in that case. */
        s.exe_sha256.clear();
        s.error.clear();
    }
    s.initialized = true;
    return true;
}

bool mod_runtime_clear_for_netplay(std::string* error) {
    RuntimeMods& s = state();
    if (!s.initialized) {
        if (error) error->clear();
        return true;
    }
    s.plan = {};
    s.validation = {};
    s.raw_disc_index.clear();
    s.user_disc_index.clear();
    s.raw_overlay_index.clear();
    s.user_overlay_index.clear();
    s.effective_disc_path.clear();
    s.main_applied = false;
    s.disc_enabled = false;
    s.disc_guard_failed = false;
    s.error.clear();
    /* Clearing s.plan is enough for activation/vblank callbacks — they only
     * run while something iterates the plan. Function overrides are armed
     * into func_override.c's own table with the dispatcher hook installed,
     * so they survive a cleared plan and keep firing unless explicitly
     * disarmed. That matters most on the rematch path (main.cpp jumps to
     * session_reboot, which is PAST mod_runtime_activate_plugins and
     * func_override_install), where a modded session entering netplay would
     * otherwise print the vanilla banner and still run its overrides —
     * diverging from a peer without the mod. Also re-arm-able: dropping the
     * armed flag lets a later plan register the same override again. */
    const int disarmed = func_override_reset_package_armed();
    if (disarmed > 0)
        for (FunctionOverridePlugin& pending : function_override_plugins())
            pending.armed = false;
    if (error) error->clear();
    std::fprintf(stdout, "psxrecomp: mods cleared for netplay (vanilla session)\n");
    return true;
}

bool mod_runtime_commit(const std::filesystem::path& disc_path, std::string* error) {
    RuntimeMods& s = state();
    if (!s.initialized) return true;
    if (disc_path != s.disc_path) {
        std::string hash_error;
        std::string digest;
        if (!sha256_file(disc_path, digest, &hash_error)) digest.clear();
        s.disc_path = disc_path;
        s.disc_sha256 = std::move(digest);
    }
    ModResolution plan =
        s.manager.resolve(s.game_id, s.exe_sha256, s.disc_sha256);
    s.validation = plan;
    if (!plan.ok) {
        s.error.clear();
        for (const std::string& item : plan.errors) {
            if (!s.error.empty()) s.error += "\n";
            s.error += item;
        }
        if (error) *error = s.error;
        return false;
    }
    for (const ModResolution::Overlay& overlay : plan.overlays) {
        if (overlay.expected_sha256.empty()) continue;
        std::string actual;
        if (!sha256_disc_range(
                s.disc_path, overlay.target, overlay.location,
                overlay.payload.size(), actual, &s.error) ||
            actual != overlay.expected_sha256) {
            if (s.error.empty())
                s.error = overlay.package_id + "/" + overlay.feature_id +
                    ": stock overlay range checksum failed";
            if (error) *error = s.error;
            return false;
        }
    }
    std::filesystem::path effective_disc;
    if (!materialize_derived_disc(s, plan, effective_disc, &s.error)) {
        if (error) *error = s.error;
        return false;
    }
    if (!s.manager.save_state(&s.error)) {
        if (error) *error = s.error;
        return false;
    }
    s.plan = std::move(plan);
    build_disc_index(s);
    s.effective_disc_path = std::move(effective_disc);
    s.main_applied = false;
    s.error.clear();
    return true;
}

const std::string& mod_runtime_fingerprint() {
    return state().plan.fingerprint;
}

const std::filesystem::path& mod_runtime_effective_disc_path() {
    return state().effective_disc_path;
}

#if defined(RECOMP_LAUNCHER)
const RecompLauncherCModProvider* mod_runtime_launcher_provider() {
    return &provider;
}
#endif

} // namespace PSXRecompV4

extern "C" void mod_runtime_on_dispatch(uint32_t target) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || s.main_applied ||
        (target & 0x1FFFFFFFu) != s.entry_phys) return;

    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        for (size_t i = 0; i < write.expected.size(); ++i) {
            if (psx_read_byte((uint32_t)write.location + (uint32_t)i) !=
                write.expected[i]) {
                std::fprintf(stderr,
                    "psxrecomp: mod plan %s rejected at 0x%08X "
                    "(expected-byte guard failed; booting unmodified)\n",
                    s.plan.fingerprint.c_str(),
                    (unsigned)((uint32_t)write.location + (uint32_t)i));
                s.main_applied = true;
                return;
            }
        }
    }
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        apply_main_write(write);
    }
    s.main_applied = true;
    if (!s.plan.writes.empty())
        std::fprintf(stdout, "psxrecomp: applied mod plan %s\n",
                     s.plan.fingerprint.c_str());
}

extern "C" void mod_runtime_on_savestate_loaded(void) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok) return;

    if (!s.main_applied) {
        uint32_t failed_at = 0;
        if (!restored_main_matches_plan(s, failed_at)) {
            std::fprintf(stderr,
                "psxrecomp: mod plan %s rejected after savestate restore at "
                "0x%08X (expected-byte guard failed)\n",
                s.plan.fingerprint.c_str(), (unsigned)failed_at);
            return;
        }
    }

    bool applied = false;
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        apply_main_write(write);
        applied = true;
    }
    s.main_applied = true;
    if (applied)
        std::fprintf(stdout,
            "psxrecomp: reapplied mod plan %s after savestate restore\n",
            s.plan.fingerprint.c_str());
}

extern "C" void mod_runtime_enable_disc_patches(void) {
    PSXRecompV4::state().disc_enabled = true;
}

extern "C" void mod_runtime_activate_plugins(void) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok) return;
    for (const ModResolution::Plugin& plugin : s.plan.plugins) {
        s.current_plugin = &plugin;
        mod_invoke_activation_plugin(plugin.id);
        s.current_plugin = nullptr;
    }
    /* Arm the package-gated function overrides for plan-selected plugin
     * ids, then (re)install the dispatcher hook. func_override_add refuses
     * duplicate addresses; a refusal here means two active plugins claim
     * one function, which the resolver should have prevented — the armed
     * flag stays false and the `func_override` TCP command shows the gap. */
    for (FunctionOverridePlugin& pending : function_override_plugins()) {
        if (pending.armed) continue;
        const bool selected = std::any_of(
            s.plan.plugins.begin(), s.plan.plugins.end(),
            [&](const ModResolution::Plugin& plugin) {
                return plugin.id == pending.plugin;
            });
        if (!selected) continue;
        const int rc = func_override_add_package(
            pending.id.c_str(), pending.address, pending.fn,
            pending.n_guard ? pending.guard : nullptr, pending.n_guard,
            pending.credit);
        pending.armed = (rc == FO_OK);
    }
    func_override_install();
}

extern "C" void mod_runtime_on_vblank(void) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok) return;
    for (const ModResolution::Plugin& plugin : s.plan.plugins) {
        s.current_plugin = &plugin;
        mod_invoke_vblank_plugin(plugin.id);
        s.current_plugin = nullptr;
    }
}

extern "C" int psx_mod_game_started(void) {
    return fntrace_is_game_started();
}

extern "C" int psx_mod_option_value(const char* package_id,
                                    const char* feature_id,
                                    const char* option_id,
                                    char* out, uint32_t out_size) {
    using namespace PSXRecompV4;
    if (out && out_size) out[0] = '\0';
    if (!package_id || !feature_id || !option_id || !out || out_size == 0)
        return 0;
    RuntimeMods& s = state();
    /* Activation runs after the final plan commit, so a committed plan is the
     * precondition for a meaningful answer. Without one there is no selection
     * to read and the caller must fall back to its own default rather than
     * treat an empty string as a value. */
    if (!s.initialized || !s.plan.ok) return 0;
    const std::string value = s.manager.feature_option_value(
        package_id, feature_id, option_id);
    if (value.empty()) return 0;
    if (value.size() + 1 > (size_t)out_size) return 0;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return 1;
}

extern "C" int psx_mod_current_resource_path(const char* resource_id,
                                             char* out, uint32_t out_size) {
    using namespace PSXRecompV4;
    if (out && out_size) out[0] = '\0';
    if (!resource_id || !resource_id[0] || !out || out_size == 0)
        return 0;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok || !s.current_plugin) return 0;
    for (const ModResolution::Resource& resource : s.plan.resources) {
        if (resource.package_id != s.current_plugin->package_id ||
            resource.feature_id != s.current_plugin->feature_id ||
            resource.id != resource_id)
            continue;
        const std::string text = resource.path.string();
        if (text.empty() || text.size() + 1 > (size_t)out_size) return 0;
        std::memcpy(out, text.c_str(), text.size() + 1);
        return 1;
    }
    return 0;
}

extern "C" uint8_t psx_mod_read_byte(uint32_t address) {
    return psx_read_byte(address);
}

extern "C" void psx_mod_write_byte(uint32_t address, uint8_t value) {
    psx_write_byte(address, value);
}

extern "C" uint16_t psx_mod_read_half(uint32_t address) {
    return psx_read_half(address);
}

extern "C" void psx_mod_write_half(uint32_t address, uint16_t value) {
    psx_write_half(address, value);
}

extern "C" uint32_t psx_mod_read_word(uint32_t address) {
    return psx_read_word(address);
}

extern "C" void psx_mod_write_word(uint32_t address, uint32_t value) {
    psx_write_word(address, value);
}

extern "C" void psx_mod_write_code_word(uint32_t address, uint32_t value) {
    psx_write_word(address, value);
    dirty_ram_mark_executable_range(address & 0x1FFFFFFFu, 4u);
}

extern "C" uint32_t psx_mod_alloc_guest_memory(uint32_t size,
                                                uint32_t alignment) {
    return psx_mod_memory_alloc(size, alignment);
}

extern "C" uint32_t psx_mod_alloc_gpu_dma_memory(uint32_t size,
                                                  uint32_t alignment) {
    return psx_mod_gpu_dma_memory_alloc(size, alignment);
}

extern "C" int32_t psx_mod_widescreen_x_margin(void) {
    return (int32_t)psx_ws_x_margin();
}

/*
 * The presenter's own view of the scanned-out picture. Plugins that draw
 * overlay primitives need the real edge, and the visible width depends on the
 * GP1(06h) horizontal range, which GPUSTAT does not carry -- so a plugin
 * cannot derive this itself. Zero means "not established yet"; the header
 * tells callers to skip drawing rather than guess.
 */
extern "C" uint32_t psx_mod_display_width(void) {
    GpuDisplayInfo info;
    std::memset(&info, 0, sizeof(info));
    gpu_get_display_info(&info);
    return info.width;
}

extern "C" uint32_t psx_mod_display_height(void) {
    GpuDisplayInfo info;
    std::memset(&info, 0, sizeof(info));
    gpu_get_display_info(&info);
    return info.height;
}

extern "C" int psx_mod_register_function_entry_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback) {
    using namespace PSXRecompV4;
    if (!id || !*id || !address || !callback) return 0;
    auto& plugins = function_entry_plugins();
    const auto duplicate = std::find_if(
        plugins.begin(), plugins.end(), [&](const FunctionEntryPlugin& item) {
            return item.id == id && item.address == address;
        });
    if (duplicate != plugins.end()) return 0;
    plugins.push_back(FunctionEntryPlugin{id, address, callback});
    return 1;
}

extern "C" int psx_mod_register_function_override(
    const char* id, uint32_t address, PSXModFunctionOverrideFn fn,
    const uint32_t* expected_words, int n_words, int32_t credit) {
    using namespace PSXRecompV4;
    if (!id || !*id || !address || !fn) return 0;
    if (n_words < 0 || n_words > FO_MAX_GUARD_WORDS) return 0;
    if (n_words > 0 && !expected_words) return 0;
    if (credit < FO_CREDIT_SELF) return 0;
    /* An optional ":label" suffix names this override in diagnostics (the
     * `func_override` TCP command) without multiplying manifest plugin ids —
     * gating and resolver availability use only the part before the ':'.
     * "pkg.feature:aim" and "pkg.feature:fire" are two overrides under the
     * one manifest plugin "pkg.feature". */
    const char* colon = strchr(id, ':');
    const std::string plugin_id = colon ? std::string(id, colon - id)
                                        : std::string(id);
    if (plugin_id.empty() || (colon && !colon[1])) return 0;
    auto& plugins = function_override_plugins();
    const auto duplicate = std::find_if(
        plugins.begin(), plugins.end(),
        [&](const FunctionOverridePlugin& item) {
            return item.id == id && item.address == address;
        });
    if (duplicate != plugins.end()) return 0;
    FunctionOverridePlugin plugin;
    plugin.id = id;
    plugin.plugin = plugin_id;
    plugin.address = address;
    plugin.fn = fn;
    for (int i = 0; i < n_words; ++i) plugin.guard[i] = expected_words[i];
    plugin.n_guard = n_words;
    plugin.credit = credit;
    plugins.push_back(plugin);
    /* Mark the plugin id available to the package resolver so a manifest can
     * gate an override-only plugin (multiple overrides may share one). */
    mod_register_function_override_marker(plugin_id);
    return 1;
}

extern "C" void psx_mod_function_entry(CPUState* cpu, uint32_t address) {
    using namespace PSXRecompV4;
    if (!cpu) return;
    for (const FunctionEntryPlugin& plugin : function_entry_plugins()) {
        if (plugin.address == address) plugin.callback(cpu, address);
    }
}

extern "C" void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                               uint8_t* bytes, uint32_t size) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.disc_enabled || s.disc_guard_failed ||
        !bytes || size == 0) return;
    /* Raw Mode2 Form1 reads are also the source of the 2048-byte logical
     * stream consumed by the emulated CD controller. Apply raw claims to the
     * complete sector, then user-data claims to its payload window. Form2/XA
     * and CDDA sectors deliberately do not receive disc_user overlays. */
    const bool has_mode2_form1_user_data =
        raw_sector && size >= 2072 && bytes[15] == 2 &&
        (bytes[18] & 0x20u) == 0;
    const ModPatchTarget target =
        raw_sector ? ModPatchTarget::DiscRaw : ModPatchTarget::DiscUser;
    const uint64_t base = (uint64_t)lba * size;
    const uint64_t end = base + size;
    const auto& index = raw_sector ? s.raw_disc_index : s.user_disc_index;
    const auto sector = index.find(lba);
    const auto& overlay_index =
        raw_sector ? s.raw_overlay_index : s.user_overlay_index;
    const auto overlay_sector = overlay_index.find(lba);
    if (sector == index.end() && overlay_sector == overlay_index.end()) {
        if (has_mode2_form1_user_data)
            mod_runtime_patch_disc_sector(lba, 0, bytes + 24, 2048);
        return;
    }
    if (sector != index.end()) {
        for (size_t write_index : sector->second) {
            const ModResolution::Write& write = s.plan.writes[write_index];
            if (write.target != target || write.location < base ||
                write.location + write.expected.size() > end) continue;
            const size_t offset = (size_t)(write.location - base);
            if (std::memcmp(bytes + offset, write.expected.data(),
                            write.expected.size()) != 0) {
                std::fprintf(stderr,
                    "psxrecomp: disc mod plan %s rejected at LBA %u+%zu "
                    "(expected-byte guard failed; disc overlay disabled)\n",
                    s.plan.fingerprint.c_str(), lba, offset);
                s.disc_guard_failed = true;
                return;
            }
        }
        for (size_t write_index : sector->second) {
            const ModResolution::Write& write = s.plan.writes[write_index];
            if (write.target != target || write.location < base ||
                write.location + write.expected.size() > end) continue;
            const size_t offset = (size_t)(write.location - base);
            if (write.fields.empty()) {
                std::memcpy(bytes + offset, write.replacement.data(),
                            write.replacement.size());
            } else {
                for (const ModResolution::Write::Field& field :
                     write.fields)
                    std::memcpy(
                        bytes + offset +
                            static_cast<size_t>(field.offset),
                        field.replacement.data(),
                        field.replacement.size());
            }
        }
    }
    if (overlay_sector != overlay_index.end()) {
        for (size_t overlay_index_value : overlay_sector->second) {
            const ModResolution::Overlay& overlay =
                s.plan.overlays[overlay_index_value];
            const uint64_t overlay_end =
                overlay.location + overlay.payload.size();
            const uint64_t copy_begin = std::max(base, overlay.location);
            const uint64_t copy_end = std::min(end, overlay_end);
            if (copy_begin >= copy_end) continue;
            const size_t destination = (size_t)(copy_begin - base);
            const size_t source = (size_t)(copy_begin - overlay.location);
            const size_t count = (size_t)(copy_end - copy_begin);
            std::memcpy(bytes + destination,
                        overlay.payload.data() + source, count);
        }
    }
    if (has_mode2_form1_user_data && !s.disc_guard_failed)
        mod_runtime_patch_disc_sector(lba, 0, bytes + 24, 2048);
}
