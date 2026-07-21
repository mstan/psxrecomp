// psxrecomp — self-contained developer front-end.
//
// Disc image + BIOS in; generated C project + build scripts out.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fmt/format.h"
#include "iso_reader.h"
#include "ps1_exe_parser.h"

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path disc;
    fs::path bios;
    fs::path output;
    std::string name;
};

void usage(const char* program) {
    fmt::print(
        "Usage:\n"
        "  {} build --disc <game.cue|bin|iso> --bios <SCPH1001.BIN> "
        "--output <directory> [--name <title>]\n\n"
        "The output contains generated game/BIOS C, game.toml, CMakeLists.txt,\n"
        "and build scripts. No compiler toolchain is bundled.\n",
        program);
}

std::string toml_string(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\') out += '/';
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
    size_t first = 0;
    while (first < value.size() && std::isspace((unsigned char)value[first])) first++;
    return value.substr(first);
}

std::string parse_boot_path(const std::string& cnf) {
    std::string lower = cnf;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    size_t pos = lower.find("cdrom:");
    if (pos == std::string::npos) pos = lower.find("cdrom0:");
    if (pos == std::string::npos) return {};
    pos = lower.find(':', pos) + 1;
    while (pos < cnf.size() && (cnf[pos] == '\\' || cnf[pos] == '/')) pos++;
    std::string path;
    while (pos < cnf.size()) {
        char c = cnf[pos++];
        if (c == ';' || c == '\r' || c == '\n' || c == '\0' ||
            std::isspace((unsigned char)c)) break;
        path += c;
    }
    return path;
}

std::string serial_from_boot(const std::string& boot) {
    std::string base = fs::path(boot).filename().string();
    std::string clean;
    for (char c : base) {
        if (std::isalnum((unsigned char)c)) clean += (char)std::toupper((unsigned char)c);
    }
    size_t split = 0;
    while (split < clean.size() && std::isalpha((unsigned char)clean[split])) split++;
    if (split && split < clean.size()) clean.insert(split, "-");
    return clean.empty() ? "PSX-GAME" : clean;
}

std::string safe_stem(std::string value) {
    std::string out;
    for (char c : value) {
        if (std::isalnum((unsigned char)c)) out += c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "PSXGameRecomp" : out;
}

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << contents;
    if (!out) throw std::runtime_error("write failed for " + path.string());
}

void write_bytes(const fs::path& path, const std::vector<uint8_t>& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write((const char*)contents.data(), (std::streamsize)contents.size());
    if (!out) throw std::runtime_error("write failed for " + path.string());
}

fs::path executable_dir(const char* argv0) {
    std::error_code ec;
    fs::path path = fs::absolute(argv0, ec);
    if (ec) path = argv0;
    path = fs::weakly_canonical(path, ec);
    return (ec ? fs::path(argv0) : path).parent_path();
}

fs::path find_helper(const fs::path& exe_dir, const std::string& name) {
#ifdef _WIN32
    const std::string filename = name + ".exe";
#else
    const std::string filename = name;
#endif
    for (const fs::path& candidate : {
             exe_dir / "libexec" / filename,
             exe_dir / filename,
         }) {
        if (fs::is_regular_file(candidate)) return fs::absolute(candidate);
    }
    throw std::runtime_error("missing packaged helper: " + filename);
}

fs::path find_framework(const fs::path& exe_dir) {
    for (const fs::path& candidate : {
             exe_dir / "framework",
             exe_dir.parent_path().parent_path(),
         }) {
        if (fs::is_regular_file(candidate / "runtime" / "runtime.cmake"))
            return fs::absolute(candidate);
    }
    throw std::runtime_error("packaged framework sources are missing");
}

fs::path find_bios_seeds(const fs::path& exe_dir) {
    for (const fs::path& candidate : {
             exe_dir / "share" / "phase2_ghidra_seeds.json",
             exe_dir.parent_path() / "seeds" / "phase2_ghidra_seeds.json",
         }) {
        if (fs::is_regular_file(candidate)) return fs::absolute(candidate);
    }
    throw std::runtime_error("packaged BIOS seed data is missing");
}

int run_process(const std::vector<std::string>& args) {
    std::vector<char*> raw;
    raw.reserve(args.size() + 1);
    for (const std::string& arg : args) raw.push_back(const_cast<char*>(arg.c_str()));
    raw.push_back(nullptr);
#ifdef _WIN32
    intptr_t result = _spawnv(_P_WAIT, args[0].c_str(), raw.data());
    return result < 0 ? -1 : (int)result;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(args[0].c_str(), raw.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

class CurrentPathGuard {
public:
    explicit CurrentPathGuard(const fs::path& path) : original_(fs::current_path()) {
        fs::current_path(path);
    }
    ~CurrentPathGuard() {
        std::error_code ignored;
        fs::current_path(original_, ignored);
    }

    CurrentPathGuard(const CurrentPathGuard&) = delete;
    CurrentPathGuard& operator=(const CurrentPathGuard&) = delete;

private:
    fs::path original_;
};

void copy_framework(const fs::path& source, const fs::path& destination) {
    fs::create_directories(destination);
    for (const auto& entry : fs::directory_iterator(source)) {
        fs::copy(entry.path(), destination / entry.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(flag) + " needs a value");
            return argv[++i];
        };
        if (arg == "--disc") options.disc = value("--disc");
        else if (arg == "--bios") options.bios = value("--bios");
        else if (arg == "--output" || arg == "-o") options.output = value("--output");
        else if (arg == "--name") options.name = value("--name");
        else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (options.disc.empty() || options.bios.empty() || options.output.empty())
        throw std::runtime_error("--disc, --bios, and --output are required");
    options.disc = fs::absolute(options.disc);
    options.bios = fs::absolute(options.bios);
    options.output = fs::absolute(options.output);
    return options;
}

int build_project(const Options& options, const fs::path& exe_dir) {
    if (!fs::is_regular_file(options.disc))
        throw std::runtime_error("disc image not found: " + options.disc.string());
    if (!fs::is_regular_file(options.bios))
        throw std::runtime_error("BIOS not found: " + options.bios.string());
    if (fs::file_size(options.bios) != 512 * 1024)
        throw std::runtime_error("BIOS must be an uncompressed 512 KiB PlayStation BIOS dump");
    if (fs::exists(options.output)) {
        if (!fs::is_directory(options.output))
            throw std::runtime_error("output path is not a directory: " + options.output.string());
        if (!fs::is_empty(options.output))
            throw std::runtime_error("output directory is not empty: " + options.output.string());
    }

    PS1::ISOReader disc;
    if (!disc.Open(options.disc.string()))
        throw std::runtime_error("could not open the disc image or its CUE tracks");
    size_t cnf_size = disc.GetFileSize("SYSTEM.CNF");
    if (!cnf_size) throw std::runtime_error("SYSTEM.CNF was not found on the disc");
    std::vector<uint8_t> cnf_bytes(cnf_size + 1, 0);
    size_t cnf_read = disc.ReadFile("SYSTEM.CNF", cnf_bytes.data(), cnf_size);
    std::string boot = parse_boot_path(std::string((char*)cnf_bytes.data(), cnf_read));
    if (boot.empty()) throw std::runtime_error("SYSTEM.CNF does not contain a BOOT executable");
    size_t exe_size = disc.GetFileSize(boot);
    if (!exe_size) throw std::runtime_error("boot executable was not found on disc: " + boot);
    std::vector<uint8_t> exe_bytes(exe_size);
    if (disc.ReadFile(boot, exe_bytes.data(), exe_bytes.size()) != exe_bytes.size())
        throw std::runtime_error("failed to read the complete boot executable");
    if (exe_bytes.size() < 2048 || std::string((char*)exe_bytes.data(), 8) != "PS-X EXE")
        throw std::runtime_error("the disc BOOT file is not a PS-X EXE");

    fs::create_directories(options.output);
    // Mark the generated folder as its own project before config_loader walks
    // upward looking for a project root. This matters when the output folder
    // happens to live inside another checkout.
    write_file(options.output / ".gitignore",
        "build/\ninput/\ngenerated/\nbios-generated/\n*.mcr\nsaves/\n");
    const std::string boot_file = fs::path(boot).filename().string();
    const fs::path local_exe = options.output / "input" / boot_file;
    write_bytes(local_exe, exe_bytes);

    std::string parse_error;
    auto parsed = PSXRecomp::PS1ExeParser::parse_file(local_exe, parse_error);
    if (!parsed) throw std::runtime_error("failed to parse boot executable: " + parse_error);
    const auto& image = *parsed;
    const std::string serial = serial_from_boot(boot);
    const std::string title = options.name.empty()
                                  ? trim(disc.GetVolumeID())
                                  : options.name;
    const std::string game_name = title.empty() ? serial : title;
    const std::string project_name = safe_stem(game_name) + "Recomp";

    std::set<uint32_t> seeds = {image.entry_point()};
    for (uint32_t address = image.load_address(); address + 4 <= image.end_address(); address += 4) {
        auto word = image.read_word(address);
        if (!word || ((*word >> 26) & 0x3F) != 0x03) continue;
        uint32_t target = (address & 0xF0000000u) | ((*word & 0x03FFFFFFu) << 2);
        if (target >= image.load_address() && target < image.end_address()) seeds.insert(target);
    }
    std::string seed_text = fmt::format("# Auto-generated JAL targets for {}\n", serial);
    for (uint32_t seed : seeds) seed_text += fmt::format("0x{:08X}\n", seed);
    write_file(options.output / "seeds" / "functions.txt", seed_text);

    uint32_t text_size = image.code_size();
    if (text_size & 0xFFF) text_size = (text_size + 0xFFF) & ~0xFFFu;
    const uint32_t stack = image.header.stack_base ? image.header.stack_base : 0x801FFFF0u;
    const std::string config = fmt::format(
        "[game]\n"
        "name = \"{}\"\n"
        "id = \"{}\"\n"
        "exe = \"input/{}\"\n"
        "disc = \"{}\"\n"
        "load_address = \"0x{:08X}\"\n"
        "entry_pc = \"0x{:08X}\"\n"
        "text_size = \"0x{:X}\"\n"
        "stack_base = \"0x{:08X}\"\n\n"
        "[recompiler]\n"
        "seeds = \"seeds/functions.txt\"\n"
        "strict = true\n"
        "out_dir = \"generated\"\n\n"
        "[runtime]\n"
        "window_title = \"{} Recompiled\"\n"
        "bios_hle = false\n"
        "overlay_cache = true\n\n"
        "[video]\n"
        "renderer = \"opengl\"\n",
        toml_string(game_name), serial, toml_string(boot_file),
        toml_string(options.disc.generic_string()), image.load_address(),
        image.entry_point(), text_size, stack, toml_string(game_name));
    write_file(options.output / "game.toml", config);

    const fs::path game_tool = find_helper(exe_dir, "psxrecomp-game");
    const fs::path bios_tool = find_helper(exe_dir, "psxrecomp-bios");
    fmt::print("[1/4] Recompiling game executable...\n");
    {
        CurrentPathGuard project_directory(options.output);
        if (run_process({game_tool.string(), "--config", "game.toml"}))
            throw std::runtime_error("game recompilation failed");
    }
    fmt::print("[2/4] Recompiling BIOS...\n");
    fs::create_directories(options.output / "bios-generated");
    if (run_process({bios_tool.string(), options.bios.string(),
                     (options.output / "bios-generated").string(), "--emit-full",
                     find_bios_seeds(exe_dir).string()}))
        throw std::runtime_error("BIOS recompilation failed");

    fmt::print("[3/4] Copying build framework...\n");
    copy_framework(find_framework(exe_dir), options.output / "psxrecomp");

    const std::string cmake = fmt::format(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project({} C CXX)\n"
        "set(CMAKE_C_STANDARD 99)\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(PSXRECOMP_ROOT \"${{CMAKE_CURRENT_SOURCE_DIR}}/psxrecomp\")\n"
        "set(PSX_LAUNCHER OFF CACHE BOOL \"\" FORCE)\n"
        "include(\"${{PSXRECOMP_ROOT}}/runtime/runtime.cmake\")\n"
        "file(GLOB GAME_FULL CONFIGURE_DEPENDS\n"
        "  \"${{CMAKE_CURRENT_SOURCE_DIR}}/generated/*_full.c\"\n"
        "  \"${{CMAKE_CURRENT_SOURCE_DIR}}/generated/*_full_*.c\")\n"
        "file(GLOB GAME_DISPATCH \"${{CMAKE_CURRENT_SOURCE_DIR}}/generated/*_dispatch.c\")\n"
        "psxrecomp_add_runtime_target(psx-runtime\n"
        "  GAME_GENERATED_FULL_C \"${{GAME_FULL}}\"\n"
        "  GAME_GENERATED_DISPATCH_C \"${{GAME_DISPATCH}}\"\n"
        "  BIOS_GENERATED_FULL_C \"${{CMAKE_CURRENT_SOURCE_DIR}}/bios-generated/SCPH1001_full.c\"\n"
        "  BIOS_GENERATED_DISPATCH_C \"${{CMAKE_CURRENT_SOURCE_DIR}}/bios-generated/SCPH1001_dispatch.c\"\n"
        "  WINDOW_TITLE \"{} Recompiled\"\n"
        "  DEFAULT_GAME_CONFIG_PATH \"game.toml\"\n"
        ")\n",
        project_name, game_name);
    write_file(options.output / "CMakeLists.txt", cmake);

    write_file(options.output / "build.ps1",
        "$ErrorActionPreference = 'Stop'\n"
        "$Root = Split-Path -Parent $MyInvocation.MyCommand.Path\n"
        "cmake -S $Root -B (Join-Path $Root 'build') -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_LAUNCHER=OFF\n"
        "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n"
        "cmake --build (Join-Path $Root 'build') --config Release --parallel\n"
        "exit $LASTEXITCODE\n");
    write_file(options.output / "build.sh",
        "#!/usr/bin/env sh\n"
        "set -eu\n"
        "ROOT=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
        "cmake -S \"$ROOT\" -B \"$ROOT/build\" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_LAUNCHER=OFF\n"
        "cmake --build \"$ROOT/build\" --config Release --parallel\n");
    write_file(options.output / ".gitignore",
        "build/\ninput/\ngenerated/\nbios-generated/\n*.mcr\nsaves/\n");
    write_file(options.output / "README.md", fmt::format(
        "# {}\n\n"
        "Generated locally by PSXRecomp from your own disc and BIOS.\n\n"
        "## Build\n\n"
        "Install CMake, Ninja, a C/C++ compiler, and SDL2 development files.\n"
        "Then run `sh build.sh` on macOS/Linux or `.\\build.ps1` in PowerShell.\n\n"
        "The executable is written under `build/`. Keep your original disc image\n"
        "at the path stored in `game.toml`, or update that path before running.\n\n"
        "The `input/`, `generated/`, and `bios-generated/` folders contain data\n"
        "derived from copyrighted files you supplied. Do not redistribute them.\n",
        game_name));
    fmt::print("[4/4] Wrote project files.\n\nReady: {}\nBuild with: {}\n",
               options.output.string(),
#ifdef _WIN32
               (options.output / "build.ps1").string()
#else
               (options.output / "build.sh").string()
#endif
    );
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
            usage(argv[0]);
            return argc < 2 ? 2 : 0;
        }
        if (std::string(argv[1]) != "build") {
            usage(argv[0]);
            throw std::runtime_error("unknown command: " + std::string(argv[1]));
        }
        return build_project(parse_options(argc, argv), executable_dir(argv[0]));
    } catch (const std::exception& error) {
        fmt::print(stderr, "psxrecomp: error: {}\n", error.what());
        return 1;
    }
}
