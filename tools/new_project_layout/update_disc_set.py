#!/usr/bin/env python3
"""Point an EXISTING game.toml at a multi-disc set, without rewriting the file.

Why this exists
---------------
`probe_disc.py --write-game-toml` renders a COMPLETE game.toml. That is right
when scaffolding a new project and wrong for every run after it: a live project
has hand-tuned [video], [controller], [widescreen], [netplay] and per-title
comments that a regeneration would silently discard.

The setup wizard needs the other half of that job -- take the disc images the
player located and update only the keys that describe the set:

    [game]     disc / discs, disc_serials
    [netplay]  required_disc_fps

Everything else in the file is preserved byte-for-byte, comments included.

This is the step that makes the standalone wizard reach the same end state as
the RetComM path, which runs probe_disc.py per disc and verify_disc_set.py over
the results. The runtime's hot-swap roster is built from [game] discs -- not
from disc.cfg, which is only the mounted-image cache -- so without this the
wizard can record every disc a player owns and none of them reach the roster.

Refuses to write when verify_disc_set.py rejects the set: a set that is not one
program is a mistake to surface, not a config to record.

Usage:
  python3 update_disc_set.py --game-toml game.toml disc1.cue disc2.cue disc3.cue
  python3 update_disc_set.py --game-toml game.toml --check disc1.cue ...
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from dataclasses import asdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import probe_disc  # noqa: E402


def toml_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def render_array(key: str, values: list[str], indent: str = "    ") -> list[str]:
    if not values:
        return []
    out = [f"{key} = ["]
    out += [f'{indent}"{toml_escape(v)}",' for v in values]
    out.append("]")
    return out


class TomlSurgeon:
    """Line-oriented replacement of single keys and inline arrays.

    Deliberately not a TOML round-trip: every parser that can rewrite this file
    also reflows it, and the comments in a game.toml carry reasoning that
    reflowing destroys. Only the exact key lines are touched.
    """

    def __init__(self, text: str):
        self.lines = text.splitlines()

    def _section_bounds(self, section: str) -> tuple[int, int]:
        """[start, end) line range of a section body, or (-1, -1)."""
        start = -1
        for i, ln in enumerate(self.lines):
            s = ln.strip()
            if start < 0:
                if s == f"[{section}]":
                    start = i + 1
            elif s.startswith("[") and not s.startswith("[["):
                return start, i
        return (start, len(self.lines)) if start >= 0 else (-1, -1)

    def _find_key(self, key: str, lo: int, hi: int) -> int:
        for i in range(lo, min(hi, len(self.lines))):
            s = self.lines[i].lstrip()
            if s.startswith(key):
                rest = s[len(key):].lstrip()
                if rest.startswith("="):
                    return i
        return -1

    def _block_end(self, start: int) -> int:
        """End (exclusive) of the value at `start`, spanning a [ ... ] array."""
        if "[" in self.lines[start] and "]" not in self.lines[start].split("=", 1)[1]:
            for j in range(start + 1, len(self.lines)):
                if self.lines[j].strip().startswith("]"):
                    return j + 1
        return start + 1

    def current_values(self, section: str, key: str) -> list[str] | None:
        """The quoted strings a key currently holds, scalar or array.

        None when the key is absent. Used to skip a rewrite whose result would
        be semantically identical -- a project's game.toml is hand-formatted,
        and reflowing an array we agree with turns every run of this tool into
        a diff for no reason.
        """
        lo, hi = self._section_bounds(section)
        if lo < 0:
            return None
        at = self._find_key(key, lo, hi)
        if at < 0:
            return None
        blob = "\n".join(self.lines[at:self._block_end(at)])
        blob = blob.split("=", 1)[1] if "=" in blob else blob
        out, i = [], 0
        while i < len(blob):
            if blob[i] == "#":                       # trailing comment
                while i < len(blob) and blob[i] != "\n":
                    i += 1
                continue
            if blob[i] != '"':
                i += 1
                continue
            j = i + 1
            buf = []
            while j < len(blob) and blob[j] != '"':
                if blob[j] == "\\" and j + 1 < len(blob):
                    j += 1
                buf.append(blob[j])
                j += 1
            out.append("".join(buf))
            i = j + 1
        return out

    def replace(self, section: str, key: str, new_lines: list[str],
                *, anchor_after: str | None = None) -> bool:
        lo, hi = self._section_bounds(section)
        if lo < 0:
            return False
        at = self._find_key(key, lo, hi)
        if at >= 0:
            self.lines[at:self._block_end(at)] = new_lines
            return True
        # Not present: insert after an anchor key when given, else at the top.
        ins = lo
        if anchor_after:
            a = self._find_key(anchor_after, lo, hi)
            if a >= 0:
                ins = self._block_end(a)
        self.lines[ins:ins] = new_lines
        return True

    def drop(self, section: str, key: str) -> bool:
        lo, hi = self._section_bounds(section)
        if lo < 0:
            return False
        at = self._find_key(key, lo, hi)
        if at < 0:
            return False
        del self.lines[at:self._block_end(at)]
        return True

    def text(self) -> str:
        return "\n".join(self.lines) + "\n"


def run_verify(probe_jsons: list[Path], out_json: Path) -> tuple[bool, str]:
    tool = HERE / "verify_disc_set.py"
    if not tool.is_file():
        return False, f"missing {tool}"
    cmd = [sys.executable, str(tool), *[str(p) for p in probe_jsons],
           "--json-out", str(out_json)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0, (r.stdout or "") + (r.stderr or "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cues", nargs="+", help="disc images in disc order (disc 1 first)")
    ap.add_argument("--game-toml", required=True, help="the game.toml to update in place")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the file would change; write nothing")
    ap.add_argument("--artifact-dir", default="",
                    help="keep disc_probe*.json / disc_set.json here. Omitted, "
                         "they are written to a temp dir and discarded -- the "
                         "default must not dirty the project.")
    args = ap.parse_args()

    game_toml = Path(args.game_toml).expanduser()
    if not game_toml.is_file():
        print(f"error: missing {game_toml}", file=sys.stderr)
        return 1
    # Probe dumps are inputs to verify_disc_set, not project content. Writing
    # them beside game.toml by default modified three TRACKED files in the game
    # repo on every Generate (disc_probe*.json are committed provenance records
    # of the original scaffold), so they go to a scratch dir unless asked for.
    tmp: tempfile.TemporaryDirectory | None = None
    if args.artifact_dir:
        art = Path(args.artifact_dir).expanduser()
        art.mkdir(parents=True, exist_ok=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="disc_set_")
        art = Path(tmp.name)

    cues = [Path(c).expanduser() for c in args.cues]
    for c in cues:
        if not c.is_file():
            print(f"error: missing disc image {c}", file=sys.stderr)
            return 1

    # Probe each disc, writing the same per-disc artifacts the RetComM path
    # leaves behind (disc_probe.json, disc_probe.2.json, ...).
    probes, probe_jsons = [], []
    for i, cue in enumerate(cues, start=1):
        try:
            p = probe_disc.probe(cue)
        except Exception as e:  # noqa: BLE001 - report, do not half-write config
            print(f"error: probing {cue.name}: {e}", file=sys.stderr)
            return 1
        payload = asdict(p)
        # Same sanitisation probe_disc.main() applies: seed_addrs is bulky and
        # boot_exe_bytes is raw bytes, which json.dumps cannot encode at all.
        payload.pop("seed_addrs", None)
        payload.pop("boot_exe_bytes", None)
        out = art / ("disc_probe.json" if i == 1 else f"disc_probe.{i}.json")
        out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        probes.append(p)
        probe_jsons.append(out)
        print(f"  disc {i}: {p.serial or '?'}  {cue.name}")

    if len(cues) > 1:
        ok, log = run_verify(probe_jsons, art / "disc_set.json")
        sys.stdout.write(log)
        if not ok:
            print("error: these images are not one buildable set — nothing written",
                  file=sys.stderr)
            return 1

    surgeon = TomlSurgeon(game_toml.read_text(encoding="utf-8"))

    def set_array(section: str, key: str, values: list[str],
                  anchor_after: str | None = None) -> None:
        if surgeon.current_values(section, key) == values:
            return          # already says this; leave the author's formatting
        surgeon.replace(section, key, render_array(key, values),
                        anchor_after=anchor_after)

    paths = [str(c) for c in cues]
    serials = [p.serial or "" for p in probes]
    fps = [p.required_disc_fp or "" for p in probes]

    if len(paths) == 1:
        surgeon.drop("game", "discs")
        if surgeon.current_values("game", "disc") != paths:
            surgeon.replace("game", "disc",
                            [f'disc = "{toml_escape(paths[0])}"'],
                            anchor_after="exe")
        surgeon.drop("game", "disc_serials")
    else:
        # `disc` and `discs` must never coexist: two keys that can disagree
        # about what is mounted is exactly the multi-source bug this replaces.
        surgeon.drop("game", "disc")
        set_array("game", "discs", paths, anchor_after="exe")
        if any(serials):
            set_array("game", "disc_serials", serials, anchor_after="discs")
    if len(paths) > 1 and any(fps):
        # Only when the file already has a [netplay] section — adding one would
        # advertise a feature this title may not build with.
        lo, _ = surgeon._section_bounds("netplay")
        if lo >= 0:
            set_array("netplay", "required_disc_fps", fps)

    new_text = surgeon.text()
    if tmp is not None:
        tmp.cleanup()
    if args.check:
        if new_text != game_toml.read_text(encoding="utf-8"):
            print(f"out of date: {game_toml}", file=sys.stderr)
            return 1
        print(f"ok: {game_toml}")
        return 0

    game_toml.write_text(new_text, encoding="utf-8")
    print(f"updated {game_toml} for {len(paths)} disc(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
