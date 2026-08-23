#include "mod_runtime.h"

#include "disc_path.h"
#include "iso_reader.h"
#include "mod_packages.h"
#include "mod_plugins.h"
#include "psx_lobby_client.h"
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
    /* PER-MACHINE. Dual-console runs two independent guests that each boot the
     * disc and each reach the EXE entry, so a single process-global one-shot
     * meant only whichever console got there FIRST received the mod plan --
     * the other silently booted stock, i.e. the two consoles ran different
     * games and could never link. Index by psx_dual_machine_live() (-1 =>
     * single console => slot 0). */
    bool main_applied[2] = { false, false };
    bool disc_enabled = false;
    bool disc_guard_failed = false;
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

/* True when every planned MainExe replacement byte is already live in RAM.
 * Used after savestate restore so we do not re-psx_write + dirty executable
 * pages for a checkpoint that was saved with the plan already applied
 * (re-dirty + overlay invalidate soft-locks enhanced 8 MB titles). */
bool restored_main_has_replacements(const RuntimeMods& s) {
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
    if (desired.empty()) return true;
    for (const auto& kv : desired) {
        if (psx_read_byte(kv.first) != kv.second)
            return false;
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
    if (!mod_runtime_commit_for_netplay(image_path ? std::filesystem::path(image_path) :
                                                    std::filesystem::path(), &error)) {
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
    s.main_applied[0] = s.main_applied[1] = false;
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
    s.main_applied[0] = s.main_applied[1] = false;
    s.disc_enabled = false;
    s.disc_guard_failed = false;
    s.error.clear();
    if (error) error->clear();
    std::fprintf(stdout, "psxrecomp: mods cleared for netplay (vanilla session)\n");
    return true;
}

/* One CSV element is "<feature>" or "<feature>=<opt>~<val>[+<opt>~<val>]".
 * Split the name from its option tail. */
static void feat_token_split(const std::string& token, std::string& name,
                             std::string& opts) {
    const size_t eq = token.find('=');
    if (eq == std::string::npos) {
        name = token;
        opts.clear();
    } else {
        name = token.substr(0, eq);
        opts = token.substr(eq + 1);
    }
}

static bool feat_csv_wants(const char* csv, const std::string& id) {
    if (!csv || !csv[0]) return true;
    const char* p = csv;
    while (*p) {
        const char* start = p;
        while (*p && *p != ',') ++p;
        std::string name, opts;
        feat_token_split(std::string(start, p), name, opts);
        if (name == id) return true;
        if (*p == ',') ++p;
    }
    return false;
}

/* Apply the option values the host published for one feature. */
static void feat_csv_apply_options(ModPackageManager& mgr,
                                   const std::string& package_id,
                                   const std::string& feature_id,
                                   const char* csv) {
    if (!csv || !csv[0]) return;
    const char* p = csv;
    while (*p) {
        const char* start = p;
        while (*p && *p != ',') ++p;
        std::string name, opts;
        feat_token_split(std::string(start, p), name, opts);
        if (*p == ',') ++p;
        if (name != feature_id || opts.empty()) continue;
        size_t pos = 0;
        while (pos < opts.size()) {
            size_t end = opts.find('+', pos);
            if (end == std::string::npos) end = opts.size();
            const std::string kv = opts.substr(pos, end - pos);
            pos = end + 1;
            const size_t tilde = kv.find('~');
            if (tilde == std::string::npos) continue;
            const std::string key = kv.substr(0, tilde);
            const std::string val = kv.substr(tilde + 1);
            std::string err;
            if (!mgr.set_feature_option(package_id, feature_id, key, val, &err)) {
                std::fprintf(stderr,
                             "psxrecomp: session plan option %s.%s=%s rejected: "
                             "%s\n", feature_id.c_str(), key.c_str(),
                             val.c_str(), err.c_str());
            }
        }
        return;
    }
}

static bool apply_host_mod_plan(const PsxLobbyMatchCaps& caps, std::string* error) {
    RuntimeMods& s = state();
    if (!s.initialized) return true;
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (const auto& [package_id, versions] : s.manager.packages()) {
            (void)versions;
            const ModPackage* package = s.manager.selected_package(package_id);
            if (!package) continue;
            int required = -1;
            for (int i = 0; i < caps.mod_count; ++i) {
                if (package_id == caps.mods[i].id) {
                    required = i;
                    break;
                }
            }
            const bool legacy = package->features.size() == 1 &&
                                package->features.front().legacy;
            if (legacy) {
                const bool want = required >= 0;
                std::string err;
                if (!s.manager.set_enabled(package_id, want, &err) && want) {
                    if (error) *error = err;
                    return false;
                }
                continue;
            }
            for (const ModFeature& feature : package->features) {
                const bool want = required >= 0 &&
                    feat_csv_wants(caps.mods[required].feats, feature.id);
                const bool have = s.manager.feature_enabled(package_id, feature.id);
                if (want == have) continue;
                std::string err;
                if (!s.manager.set_feature_enabled(package_id, feature.id, want, &err)) {
                    if (want) {
                        if (error) *error = err;
                        return false;
                    }
                    continue;
                }
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (int i = 0; i < caps.mod_count; ++i) {
        const PsxLobbyModPkg& pkg = caps.mods[i];
        if (!pkg.id[0] || !pkg.ver[0]) continue;
        std::string err;
        if (!s.manager.select_version(pkg.id, pkg.ver, &err)) {
            if (error) *error = err.empty() ? (std::string(pkg.id) + " is not installed") : err;
            return false;
        }
        const ModPackage* package = s.manager.selected_package(pkg.id);
        if (!package) {
            if (error) *error = std::string(pkg.id) + " is not installed";
            return false;
        }
        const bool legacy = package->features.size() == 1 &&
                            package->features.front().legacy;
        if (legacy) {
            if (!s.manager.set_enabled(pkg.id, true, &err)) {
                if (error) *error = err;
                return false;
            }
            continue;
        }
        for (const ModFeature& feature : package->features) {
            const bool want = feat_csv_wants(pkg.feats, feature.id);
            if (!s.manager.set_feature_enabled(pkg.id, feature.id, want, &err) && want) {
                if (error) *error = err;
                return false;
            }
            if (want)
                feat_csv_apply_options(s.manager, pkg.id, feature.id, pkg.feats);
        }
    }
    return true;
}

/* LAN / Direct-IP session plan (see mod_runtime.h). Process-global because
 * it is set from the netplay datagram pump and read at launch commit. */
static std::string& session_plan_spec() {
    static std::string spec;
    return spec;
}

void mod_runtime_set_session_plan_spec(const std::string& spec) {
    session_plan_spec() = spec;
}

static std::string& session_plan_fp() {
    static std::string fp;
    return fp;
}

void mod_runtime_set_session_plan_fp(const std::string& fp) {
    session_plan_fp() = fp;
}

const std::string& mod_runtime_session_plan_fp() {
    return session_plan_fp();
}

const std::string& mod_runtime_session_plan_spec() {
    return session_plan_spec();
}

bool mod_runtime_commit_for_netplay(const std::filesystem::path& disc_path,
                                    std::string* error) {
    if (!psx_lobby_in_lobby()) {
        /* No lobby server in the picture (LAN / Direct-IP): the host's plan
         * arrived in the session datagrams instead. Applying it here is what
         * makes LAN sessions modded rather than silently vanilla. */
        if (!session_plan_spec().empty()) {
            if (!mod_runtime_apply_link_spec(session_plan_spec(), disc_path,
                                             error))
                return false;
            return mod_runtime_verify_session_plan_fp(error);
        }
        return mod_runtime_clear_for_netplay(error);
    }
    const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();
    if (!caps || !caps->valid || caps->mod_count <= 0)
        return mod_runtime_clear_for_netplay(error);
    if (!apply_host_mod_plan(*caps, error)) return false;
    if (!mod_runtime_commit(disc_path, error, false)) return false;
    return mod_runtime_verify_session_plan_fp(error);
}

/* Post-apply guard: this peer's resolution of the session plan must match the
 * host's. Divergence here is silent by nature (both sides "have the mods"),
 * so refuse the launch and name the two fingerprints. */
bool mod_runtime_verify_session_plan_fp(std::string* error) {
    const std::string& want = session_plan_fp();
    if (want.empty()) return true;               /* host published nothing */
    const std::string got = mod_runtime_plan_fingerprint_portable();
    if (got.empty() || got == want) return true;
    if (error) {
        *error = "this machine resolves the session's mods differently from "
                 "the host (host " + want.substr(0, 16) + "…, here " +
                 got.substr(0, 16) + "…) — check that both have the same mod "
                 "versions and option values";
    }
    return false;
}

/* ===== PSX-Link pair mod propagation (see mod_runtime.h) ================ */

/* Spec delimiters: ';' entries, '@' id/ver, ':' ver/feats, ',' features,
 * '=' feature/options, '+' between options, '~' key/value. A token holding
 * any of them (or whitespace) would corrupt the grammar, so it is dropped
 * rather than silently mis-parsed on the far side. */
static bool spec_token_safe(const std::string& t) {
    if (t.empty()) return false;
    return t.find_first_of(";@:,=+~\n\r ") == std::string::npos;
}

std::string mod_runtime_link_spec_from_session() {
    RuntimeMods& s = state();
    std::string spec;
    if (!s.initialized) return spec;
    /* Walk packages in the manager's stable (std::map) order so repeated
     * calls and both processes produce byte-identical specs. The enabled
     * test mirrors ae_np_fill_required_mods' provider walk, so a link
     * follower applies exactly what the lobby would have published. */
    for (const auto& [package_id, versions] : s.manager.packages()) {
        (void)versions;
        const ModPackage* package = s.manager.selected_package(package_id);
        if (!package) continue;
        std::string feats;
        for (const ModFeature& feature : package->features) {
            if (!s.manager.feature_enabled(package_id, feature.id)) continue;
            if (!feats.empty()) feats += ',';
            feats += feature.id;
            /* Option values ride with their feature: two peers running the
             * same feature with different option values resolve different
             * bytes, which is invisible to an id/version-only plan and only
             * shows up as a mid-race desync. */
            std::string opts;
            for (const ModOption& option : package->options) {
                if (option.feature_id != feature.id) continue;
                const std::string value =
                    s.manager.feature_option_value(package_id, feature.id,
                                                   option.id);
                if (value.empty()) continue;
                if (!spec_token_safe(option.id) || !spec_token_safe(value))
                    continue;      /* never emit a token that breaks parsing */
                opts += opts.empty() ? '=' : '+';
                opts += option.id;
                opts += '~';
                opts += value;
            }
            feats += opts;
        }
        if (feats.empty()) continue;   /* nothing enabled in this package */
        if (!spec.empty()) spec += ';';
        spec += package_id;
        spec += '@';
        spec += package->version;
        spec += ':';
        spec += feats;
    }
    return spec;
}

bool mod_runtime_apply_link_spec(const std::string& spec,
                                 const std::filesystem::path& disc_path,
                                 std::string* error) {
    if (spec.empty())
        return mod_runtime_clear_for_netplay(error);

    /* Reuse the lobby application path verbatim: the spec is just a caps
     * mod list by another transport, so the follower runs the exact same
     * enable/version resolution the lobby peers run. */
    PsxLobbyMatchCaps caps{};
    caps.valid = 1;
    size_t pos = 0;
    while (pos < spec.size() && caps.mod_count < PSX_LOBBY_MAX_MODS) {
        size_t end = spec.find(';', pos);
        if (end == std::string::npos) end = spec.size();
        std::string entry = spec.substr(pos, end - pos);
        pos = end + 1;
        if (entry.empty()) continue;
        const size_t at = entry.find('@');
        if (at == std::string::npos) {
            if (error) *error = "malformed link mod spec entry: " + entry;
            return false;
        }
        std::string id = entry.substr(0, at);
        std::string rest = entry.substr(at + 1);
        std::string ver = rest;
        std::string feats;
        const size_t colon = rest.find(':');
        if (colon != std::string::npos) {
            ver = rest.substr(0, colon);
            feats = rest.substr(colon + 1);
        }
        PsxLobbyModPkg& pkg = caps.mods[caps.mod_count++];
        std::snprintf(pkg.id, sizeof pkg.id, "%s", id.c_str());
        std::snprintf(pkg.ver, sizeof pkg.ver, "%s", ver.c_str());
        std::snprintf(pkg.name, sizeof pkg.name, "%s", id.c_str());
        std::snprintf(pkg.feats, sizeof pkg.feats, "%s", feats.c_str());
    }
    if (!apply_host_mod_plan(caps, error)) return false;
    return mod_runtime_commit(disc_path, error, false);
}

std::string mod_runtime_plan_fingerprint_portable() {
    RuntimeMods& s = state();
    if (!s.initialized) return std::string();
    /* Empty disc sha on purpose: peers legitimately hold different dumps of
     * the same title, and disc identity is already matched by the lobby's own
     * disc fingerprint. What must agree here is the MOD plan. */
    const ModResolution r = s.manager.resolve(s.game_id, s.exe_sha256,
                                              std::string());
    return r.fingerprint;
}

uint32_t mod_runtime_link_spec_hash(const std::string& spec) {
    /* FNV-1a: identity only (pair cfg cross-check), never persisted. */
    uint32_t h = 2166136261u;
    for (unsigned char c : spec) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

bool mod_runtime_export_package(const std::string& id, const std::string& version,
                                std::vector<uint8_t>& out, std::string* sha256_hex,
                                std::string* error) {
    if (!state().manager.export_archive(id, version, out, error)) return false;
    if (sha256_hex) {
        uint8_t digest[32];
        psx_sha256_compute(out.data(), out.size(), digest);
        sha256_hex->assign(64, '0');
        static const char* kHex = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            (*sha256_hex)[(size_t)i * 2] = kHex[digest[i] >> 4];
            (*sha256_hex)[(size_t)i * 2 + 1] = kHex[digest[i] & 0xf];
        }
    }
    return true;
}

bool mod_runtime_install_bytes(const uint8_t* data, size_t size, std::string* error) {
    std::string err;
    if (!state().manager.install_archive_bytes(data, size, nullptr, nullptr, &err)) {
        if (err.find("already installed") != std::string::npos) {
            if (error) error->clear();
            return true;
        }
        if (error) *error = err;
        return false;
    }
    return true;
}

bool mod_runtime_commit(const std::filesystem::path& disc_path, std::string* error) {
    return mod_runtime_commit(disc_path, error, true);
}

bool mod_runtime_commit(const std::filesystem::path& disc_path, std::string* error,
                        bool persist) {
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
    if (persist && !s.manager.save_state(&s.error)) {
        if (error) *error = s.error;
        return false;
    }
    s.plan = std::move(plan);
    build_disc_index(s);
    s.effective_disc_path = std::move(effective_disc);
    s.main_applied[0] = s.main_applied[1] = false;
    s.error.clear();
    return true;
}

const std::string& mod_runtime_fingerprint() {
    return state().plan.fingerprint;
}

extern "C" const char* psx_mod_runtime_fingerprint_cstr(void) {
    return state().plan.fingerprint.c_str();
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

extern "C" int psx_dual_machine_live(void);

/* Which console is asking. -1 (single console) folds to slot 0. */
static int mod_runtime_machine_slot() {
    return (psx_dual_machine_live() == 1) ? 1 : 0;
}

extern "C" void mod_runtime_on_dispatch(uint32_t target) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    const int slot = mod_runtime_machine_slot();
    if (!s.initialized || s.main_applied[slot] ||
        (target & 0x1FFFFFFFu) != s.entry_phys) return;

    /* Disc overlays can rewrite the boot EXE while the BIOS LoadExe path
     * reads it, so by entry RAM may already hold plan replacements. Accept
     * stock expected OR the planned replacement (same rule as savestate
     * reapply); reject only when live bytes match neither. */
    uint32_t failed_at = 0;
    if (!restored_main_matches_plan(s, failed_at)) {
        std::fprintf(stderr,
            "psxrecomp: mod plan %s rejected at 0x%08X "
            "(expected-byte guard failed; booting unmodified)\n",
            s.plan.fingerprint.c_str(), (unsigned)failed_at);
        s.main_applied[slot] = true;
        return;
    }
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        apply_main_write(write);
    }
    s.main_applied[slot] = true;
    if (!s.plan.writes.empty())
        std::fprintf(stdout, "psxrecomp: applied mod plan %s\n",
                     s.plan.fingerprint.c_str());
}

extern "C" void mod_runtime_on_savestate_loaded(void) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok) return;

    /* Always validate: stock expected OR planned replacement. Reject only
     * when live bytes match neither (corrupt / foreign checkpoint). */
    uint32_t failed_at = 0;
    if (!restored_main_matches_plan(s, failed_at)) {
        std::fprintf(stderr,
            "psxrecomp: mod plan %s rejected after savestate restore at "
            "0x%08X (expected-byte guard failed)\n",
            s.plan.fingerprint.c_str(), (unsigned)failed_at);
        return;
    }

    /* Checkpoint saved under this plan already carries replacements in RAM.
     * Re-psx_write + dirty_ram_mark_executable_range after overlay invalidate
     * soft-locks enhanced 8 MB sessions; leave bytes alone. */
    if (restored_main_has_replacements(s)) {
        s.main_applied[mod_runtime_machine_slot()] = true;
        return;
    }

    bool applied = false;
    for (const ModResolution::Write& write : s.plan.writes) {
        if (write.target != ModPatchTarget::MainExe) continue;
        apply_main_write(write);
        applied = true;
    }
    s.main_applied[mod_runtime_machine_slot()] = true;
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
    for (const ModResolution::Plugin& plugin : s.plan.plugins)
        mod_invoke_activation_plugin(plugin.id);
}

extern "C" void mod_runtime_on_vblank(void) {
    using namespace PSXRecompV4;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok) return;
    for (const ModResolution::Plugin& plugin : s.plan.plugins)
        mod_invoke_vblank_plugin(plugin.id);
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

extern "C" int psx_mod_register_function_entry_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback) {
    using namespace PSXRecompV4;
    if (!id || !*id || !address || !callback) return 0;
    if (!mod_register_function_entry_plugin_id(id)) return 0;
    auto& plugins = function_entry_plugins();
    const auto duplicate = std::find_if(
        plugins.begin(), plugins.end(), [&](const FunctionEntryPlugin& item) {
            return item.id == id && item.address == address;
        });
    if (duplicate != plugins.end()) return 0;
    plugins.push_back(FunctionEntryPlugin{id, address, callback});
    return 1;
}

extern "C" void psx_mod_function_entry(CPUState* cpu, uint32_t address) {
    using namespace PSXRecompV4;
    if (!cpu) return;
    RuntimeMods& s = state();
    if (!s.initialized || !s.plan.ok) return;
    for (const FunctionEntryPlugin& plugin : function_entry_plugins()) {
        if (plugin.address != address) continue;
        bool enabled = false;
        for (const ModResolution::Plugin& planned : s.plan.plugins) {
            if (planned.id == plugin.id) {
                enabled = true;
                break;
            }
        }
        if (!enabled) continue;
        plugin.callback(cpu, address);
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
