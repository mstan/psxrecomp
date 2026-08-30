#!/usr/bin/env python3
"""Verify CD-ROM/SPU trace storage is absent from PSX_NO_DEBUG_TOOLS objects."""

import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

LEAN_DEFINE = "PSX_NO_DEBUG_TOOLS=1"
MIB = 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def simple_function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{([^{{}}]*)\}}",
                      source)
    require(match is not None, f"missing simple function definition: {name}")
    return " ".join(match.group(1).split())


def is_msvc(compiler: Path) -> bool:
    return compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")


def compile_and_preprocess(compiler: Path, source: Path, include_dir: Path,
                           lean: bool, output: Path) -> str:
    define = [f"/D{LEAN_DEFINE}"] if lean else []
    if is_msvc(compiler):
        common = [str(compiler), "/nologo", "/std:c11", "/Od", *define,
                  f"/I{include_dir}", f"/I{source.parent}"]
        subprocess.run(
            [*common, "/c", str(source), f"/Fo{output}"], check=True)
        result = subprocess.run(
            [*common, "/EP", str(source)], check=True,
            stdout=subprocess.PIPE, text=True, encoding="utf-8",
            errors="replace")
    else:
        define = [f"-D{LEAN_DEFINE}"] if lean else []
        common = [str(compiler), "-std=c99", "-O0", *define,
                  f"-I{include_dir}", f"-I{source.parent}"]
        subprocess.run(
            [*common, "-c", str(source), "-o", str(output)], check=True)
        result = subprocess.run(
            [*common, "-E", "-P", str(source)], check=True,
            stdout=subprocess.PIPE, text=True, encoding="utf-8",
            errors="replace")
    return " ".join(result.stdout.split())


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        end = len(data)
    return data[offset:end].decode("ascii", errors="replace")


def normalize_symbol(name: str) -> str:
    # 32-bit COFF and Mach-O C symbols carry one leading underscore.
    return name.removeprefix("_")


def coff_info(data: bytes) -> tuple[set[str], int] | None:
    if len(data) < 20:
        return None
    machine, section_count = struct.unpack_from("<HH", data, 0)
    if machine not in (0x014C, 0x01C0, 0x01C4, 0x8664, 0xAA64):
        return None
    symbol_offset, symbol_count = struct.unpack_from("<II", data, 8)
    optional_size = struct.unpack_from("<H", data, 16)[0]
    section_offset = 20 + optional_size
    require(section_offset + section_count * 40 <= len(data),
            "truncated COFF section table")

    bss = 0
    for index in range(section_count):
        off = section_offset + index * 40
        name = data[off:off + 8].split(b"\0", 1)[0]
        size = struct.unpack_from("<I", data, off + 16)[0]
        if name == b".bss" or name.startswith(b".bss$"):
            bss += size

    symbols: set[str] = set()
    entry_size = 18
    string_offset = symbol_offset + symbol_count * entry_size
    require(symbol_offset == 0 or string_offset <= len(data),
            "truncated COFF symbol table")
    index = 0
    while symbol_offset and index < symbol_count:
        off = symbol_offset + index * entry_size
        raw_name = data[off:off + 8]
        if raw_name[:4] == b"\0\0\0\0":
            string_index = struct.unpack_from("<I", raw_name, 4)[0]
            name = c_string(data, string_offset + string_index)
        else:
            name = raw_name.split(b"\0", 1)[0].decode("ascii", errors="replace")
        symbols.add(normalize_symbol(name))
        index += 1 + data[off + 17]
    return symbols, bss


def elf_info(data: bytes) -> tuple[set[str], int] | None:
    if not data.startswith(b"\x7fELF"):
        return None
    elf_class, encoding = data[4], data[5]
    require(encoding in (1, 2), "unsupported ELF byte order")
    endian = "<" if encoding == 1 else ">"
    if elf_class == 2:
        section_offset = struct.unpack_from(endian + "Q", data, 0x28)[0]
        entry_size, section_count = struct.unpack_from(endian + "HH", data, 0x3A)
        section_fmt = endian + "IIQQQQIIQQ"
        symbol_entry_size = 24
    elif elf_class == 1:
        section_offset = struct.unpack_from(endian + "I", data, 0x20)[0]
        entry_size, section_count = struct.unpack_from(endian + "HH", data, 0x2E)
        section_fmt = endian + "IIIIIIIIII"
        symbol_entry_size = 16
    else:
        raise AssertionError("unsupported ELF class")
    require(section_offset + entry_size * section_count <= len(data),
            "truncated ELF section table")
    sections = [struct.unpack_from(section_fmt, data, section_offset + i * entry_size)
                for i in range(section_count)]
    bss = sum(section[5] for section in sections if section[1] == 8)  # SHT_NOBITS
    symbols: set[str] = set()
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        sym_offset, sym_size, string_index = section[4], section[5], section[6]
        string_section = sections[string_index]
        strings = data[string_section[4]:string_section[4] + string_section[5]]
        stride = section[9] or symbol_entry_size
        for off in range(sym_offset, sym_offset + sym_size, stride):
            name_index = struct.unpack_from(endian + "I", data, off)[0]
            if name_index:
                symbols.add(normalize_symbol(c_string(strings, name_index)))
    return symbols, bss


def macho_info(data: bytes) -> tuple[set[str], int] | None:
    if len(data) < 28:
        return None
    magic = data[:4]
    formats = {
        b"\xcf\xfa\xed\xfe": ("<", True), b"\xfe\xed\xfa\xcf": (">", True),
        b"\xce\xfa\xed\xfe": ("<", False), b"\xfe\xed\xfa\xce": (">", False),
    }
    if magic not in formats:
        return None
    endian, is_64 = formats[magic]
    command_count = struct.unpack_from(endian + "I", data, 16)[0]
    cursor = 32 if is_64 else 28
    symbols: set[str] = set()
    bss = 0
    symbol_command = None
    for _ in range(command_count):
        command, command_size = struct.unpack_from(endian + "II", data, cursor)
        require(command_size >= 8 and cursor + command_size <= len(data),
                "truncated Mach-O load command")
        if command == (0x19 if is_64 else 0x1):  # LC_SEGMENT[_64]
            section_count = struct.unpack_from(
                endian + "I", data, cursor + (64 if is_64 else 48))[0]
            section_cursor = cursor + (72 if is_64 else 56)
            section_size = 80 if is_64 else 68
            for index in range(section_count):
                off = section_cursor + index * section_size
                size = struct.unpack_from(endian + ("Q" if is_64 else "I"),
                                          data, off + (40 if is_64 else 36))[0]
                flags = struct.unpack_from(endian + "I", data,
                                           off + (64 if is_64 else 56))[0]
                if flags & 0xFF in (0x1, 0xC, 0x12):
                    bss += size
        elif command == 0x2:  # LC_SYMTAB
            symbol_command = struct.unpack_from(endian + "IIII", data, cursor + 8)
        cursor += command_size
    if symbol_command:
        symoff, nsyms, stroff, strsize = symbol_command
        strings = data[stroff:stroff + strsize]
        stride = 16 if is_64 else 12
        for index in range(nsyms):
            name_index = struct.unpack_from(endian + "I", data,
                                            symoff + index * stride)[0]
            if name_index:
                symbols.add(normalize_symbol(c_string(strings, name_index)))
    return symbols, bss


def object_info(path: Path) -> tuple[set[str], int]:
    data = path.read_bytes()
    info = coff_info(data) or elf_info(data) or macho_info(data)
    require(info is not None, f"unsupported object format: {path}")
    return info


def check_symbols(label: str, symbols: set[str], expected: set[str],
                  forbidden: set[str]) -> None:
    missing = expected - symbols
    present = forbidden & symbols
    require(not missing, f"{label}: missing symbols: {sorted(missing)}")
    require(not present, f"{label}: lean object retained symbols: {sorted(present)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    include_dir = runtime / "include"
    cd_source = runtime / "src/cdrom.c"
    spu_source = runtime / "src/spu.c"
    cd_text = cd_source.read_text(encoding="utf-8")
    spu_text = spu_source.read_text(encoding="utf-8")
    require("#define trace_cdrom(kind, addr, val, width) ((void)0)" in cd_text,
            "CD lean recording stub is missing")
    require("#define record_sector_history" in cd_text,
            "CD sector-history lean recording stub is missing")
    require("#define spu_event_record(kind, voice, addr) ((void)0)" in spu_text,
            "SPU lean recording stub is missing")
    require("if (out_entries) *out_entries = NULL;" in cd_text,
            "CD lean getters no longer null non-null output pointers")

    with tempfile.TemporaryDirectory(prefix="cd-spu-trace-") as temp_name:
        temp = Path(temp_name)
        objects: dict[tuple[str, str], Path] = {}
        preprocessed: dict[tuple[str, str], str] = {}
        for unit, source in (("cd", cd_source), ("spu", spu_source)):
            for mode, lean in (("debug", False), ("lean", True)):
                suffix = ".obj" if is_msvc(args.compiler) else ".o"
                obj = temp / f"{unit}-{mode}{suffix}"
                objects[(unit, mode)] = obj
                preprocessed[(unit, mode)] = compile_and_preprocess(
                    args.compiler, source, include_dir, lean, obj)

        require("static CDROMTraceEntry cdrom_trace[" in preprocessed[("cd", "debug")],
                "debug CD trace storage was compiled out")
        require("static CDROMTraceEntry cdrom_trace[" not in preprocessed[("cd", "lean")],
                "lean CD trace storage survived preprocessing")
        require("static SpuEvent s_events[" in preprocessed[("spu", "debug")],
                "debug SPU event storage was compiled out")
        require("static SpuEvent s_events[" not in preprocessed[("spu", "lean")],
                "lean SPU event storage survived preprocessing")

        lean_cd = preprocessed[("cd", "lean")]
        for name in ("cdrom_debug_get_trace",
                     "cdrom_debug_get_command_history",
                     "cdrom_debug_get_sector_history"):
            body = simple_function_body(lean_cd, name)
            require("if (out_entries) *out_entries =" in body and "return 0;" in body,
                    f"{name} is not a zero/NULL lean stub")
        for name in ("cdrom_debug_clear_trace",
                     "cdrom_debug_clear_command_history",
                     "cdrom_debug_clear_sector_history"):
            require(simple_function_body(lean_cd, name) == "",
                    f"{name} is not an empty lean stub")

        lean_spu = preprocessed[("spu", "lean")]
        require(simple_function_body(lean_spu, "spu_event_total") == "return 0;",
                "spu_event_total is not a zero lean stub")
        get_body = simple_function_body(lean_spu, "spu_event_get")
        require("(void)out;" in get_body and "(void)max_count;" in get_body
                and get_body.endswith("return 0;"),
                "spu_event_get is not a zero lean stub")
        require(simple_function_body(lean_spu, "spu_event_reset") == "",
                "spu_event_reset is not an empty lean stub")

        info = {key: object_info(path) for key, path in objects.items()}
        public_cd = {
            "cdrom_debug_get_trace", "cdrom_debug_clear_trace",
            "cdrom_debug_get_command_history", "cdrom_debug_clear_command_history",
            "cdrom_debug_get_sector_history", "cdrom_debug_clear_sector_history",
        }
        private_cd = {"cdrom_trace", "command_history", "sector_history"}
        public_spu = {"spu_event_total", "spu_event_get", "spu_event_reset"}
        private_spu = {"s_events", "spu_event_record"}
        check_symbols("debug CD", info[("cd", "debug")][0],
                      public_cd | private_cd, set())
        check_symbols("lean CD", info[("cd", "lean")][0], public_cd, private_cd)
        check_symbols("debug SPU", info[("spu", "debug")][0],
                      public_spu | private_spu, set())
        check_symbols("lean SPU", info[("spu", "lean")][0], public_spu, private_spu)

        cd_saved = info[("cd", "debug")][1] - info[("cd", "lean")][1]
        spu_saved = info[("spu", "debug")][1] - info[("spu", "lean")][1]
        require(cd_saved >= 4 * MIB,
                f"CD lean object saved only {cd_saved} B of uninitialized storage")
        require(spu_saved >= 30 * MIB,
                f"SPU lean object saved only {spu_saved} B of uninitialized storage")

    print(f"PASS: CD/SPU lean trace contract (CD -{cd_saved} B, SPU -{spu_saved} B)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
