#!/usr/bin/env python3
"""Exercise `psxmod validate` against real manifests.

The point of the tool is that an author finds out their manifest is wrong from
a shell, instead of by packing it, installing it, and watching the mod silently
fail to appear. So this drives the built binary rather than inspecting source:
a validator that compiles but reports the wrong verdict is exactly the failure
it exists to prevent.

Usage: test_psxmod_validate.py <path-to-psxmod>
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def run(psxmod, *args):
    proc = subprocess.run([str(psxmod), "validate", *args],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


GOOD = """format_version = 6
id = "test.good"
version = "1.0.0"
name = "Good"
[[target]]
game_id = "SLUS-00000"
[[feature]]
id = "thing"
name = "Thing"
[[feature]]
id = "rough"
name = "Rough"
channel = "experimental"
"""

BAD_CHANNEL = """format_version = 6
id = "test.bad"
version = "1.0.0"
name = "Bad"
[[target]]
game_id = "SLUS-00000"
[[feature]]
id = "thing"
name = "Thing"
channel = "beta"
"""

DEFAULT_ON = """format_version = 6
id = "test.defaulton"
version = "1.0.0"
name = "Default On"
[[target]]
game_id = "SLUS-00000"
[[feature]]
id = "thing"
name = "Thing"
default_enabled = true
"""

LEGACY = """format_version = 1
id = "test.legacy"
version = "1.0.0"
name = "Legacy"
[[target]]
game_id = "SLUS-00000"
"""


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_psxmod_validate.py <path-to-psxmod>", file=sys.stderr)
        return 2
    psxmod = Path(sys.argv[1])
    if not psxmod.is_file():
        print(f"psxmod binary not found: {psxmod}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)

        def package(name, text):
            path = tmpdir / name / "manifest.toml"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            return path.parent

        good = package("good", GOOD)
        bad = package("bad", BAD_CHANNEL)
        default_on = package("default-on", DEFAULT_ON)
        legacy = package("legacy", LEGACY)

        code, out = run(psxmod, str(good))
        check(code == 0, f"a valid manifest must pass (exit {code}): {out}")
        check("1 ok" in out, f"a valid manifest must be counted ok: {out}")
        check("experimental" in out,
              f"the summary must report each feature's channel: {out}")

        # The parser's own verdict, surfaced. This is the whole point: the
        # author sees the reason, not silence.
        code, out = run(psxmod, str(bad))
        check(code == 1, f"an invalid manifest must fail (exit {code}): {out}")
        check("channel must be stable, experimental or developer" in out,
              f"the parser's reason must be reported verbatim: {out}")
        check(out.count(str(bad / "manifest.toml")) == 1,
              f"the failing path must be printed once, not twice: {out}")

        # Warnings are advisory by default and fatal under --strict, so a CI
        # gate can be stricter than an author's inner loop.
        code, out = run(psxmod, str(default_on))
        check(code == 0, f"a warning alone must not fail (exit {code}): {out}")
        check("WARN" in out and "default_enabled" in out,
              f"default_enabled must warn: {out}")
        code, out = run(psxmod, "--strict", str(default_on))
        check(code == 1, f"--strict must fail on a warning (exit {code}): {out}")

        # A package-style manifest gets a synthesised "legacy" feature, which
        # is the dialect being retired -- say so rather than quietly accepting.
        code, out = run(psxmod, str(legacy))
        check(code == 0, f"a legacy manifest still loads (exit {code}): {out}")
        check("legacy" in out and "WARN" in out,
              f"a package-style manifest must warn about the legacy shape: {out}")

        # A whole catalog at once, which is how a release would be checked.
        code, out = run(psxmod, "--quiet", str(tmpdir))
        check(code == 1, f"a catalog containing a bad package must fail: {out}")
        check("test.good" not in out,
              f"--quiet must not print packages that are merely ok: {out}")

        code, out = run(psxmod, str(tmpdir / "nope"))
        check(code == 2, f"a missing path must be a usage error, got {code}: {out}")

    # The framework's own catalog must pass the tool it ships.
    builtin = ROOT / "mods/builtin/packages"
    if builtin.is_dir():
        code, out = run(psxmod, "--strict", str(builtin))
        check(code == 0,
              f"the built-in mod catalog must validate cleanly: {out}")

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"{len(failures)} psxmod validate test(s) failed")
        return 1
    print("psxmod validate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
