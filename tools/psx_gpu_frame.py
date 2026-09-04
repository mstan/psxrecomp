#!/usr/bin/env python3
"""psx_gpu_frame.py -- capture and decode a frame's GP0 stream, with the guest
function that issued every primitive.

Why this exists
---------------
`gpu_frame_dump` on the runtime's TCP debug server already stamps every GP0
packet with the guest code that issued it (`func`, `pc`, `ra`) and its linked-
list OT rank -- see runtime/src/debug_server.c, handle_gpu_frame_dump(), and
GpuGp0RingEntry in runtime/include/gpu.h. Nothing consumed that attribution.
This module turns the raw ring into decoded primitives, so "which guest
function drew this, and was the semi-transparency bit set" has a mechanical
answer instead of a guess.

Provenance rule
---------------
Everything here is OBSERVED from a running game and is labelled as such. It is
never merged into a static claim. A `func` appearing in a frame dump is
evidence that code executed and issued a primitive -- it is not evidence about
the call graph, and the analyzer's static coverage gap stays exactly as wide as
it was.

Fidelity, stated up front
-------------------------
* Vertices are decoded exactly and the running GP0(E5) draw offset is applied,
  because that is what the rasteriser applies (gpu.c, gp0_exec_* ).
* The texpage latch follows hardware: a textured polygon's own tpage word
  overwrites draw-mode state (gpu.c set_tpage_from_poly), and later untextured
  prims and rectangles consume whatever it left behind. Getting this wrong
  mislabels exactly the semi-transparency mode you are usually chasing.
* Textures are NOT sampled. Texture pages, CLUTs and UVs are decoded and
  reported, but a textured primitive renders as its command colour. The goal is
  attribution and blend-state triage, not a second GPU implementation.
* The ring truncates each packet to GPU_GP0_RING_MAX_WORDS (12). A packet
  longer than that is decoded as far as it goes and marked `truncated`.
"""

from __future__ import annotations

import json
import socket
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

DUMP_KIND = "psx-gpu-frame"
DUMP_VERSION = 1

DEFAULT_NATIVE_PORT = 4370
DEFAULT_DUCKSTATION_PORT = 4371

# Semi-transparency modes, GPUSTAT bits 5-6. The names are the blend the
# hardware performs, because that is what you compare against the screenshot.
STP_MODES = {
    0: "0.5B+0.5F",
    1: "B+F",
    2: "B-F",
    3: "B+0.25F",
}


class DebugError(RuntimeError):
    pass


class DebugConn:
    """JSON client for the runtime (and DuckStation) debug server.

    ONE REQUEST PER CONNECTION. That is the server's actual contract, not a
    conservative choice: runtime/src/debug_server.c's io_thread_main() accepts,
    reads a single line, replies, and sock_close()s. A client that holds the
    socket open and sends a second command gets silence and then EOF, which
    looks exactly like a hung emulator. (`s_client` in that file is vestigial;
    nothing ever assigns it a live socket.) tools/debug_client.py's query()
    documents the same contract -- "connect, send, receive, close".

    So each command opens its own socket and reads to EOF. Replies can be
    megabytes (a full gpu_frame_dump), and the server's send is blocking with a
    starvation watchdog behind it, so the read drains in large chunks and never
    parses incrementally.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = DEFAULT_NATIVE_PORT,
                 timeout: float = 15.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._next_id = 1

    def __enter__(self) -> "DebugConn":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def connect(self) -> None:
        """Probe that a server is there. Not required before cmd()."""
        try:
            socket.create_connection((self.host, self.port),
                                     timeout=self.timeout).close()
        except OSError as e:
            raise DebugError(f"no debug server on {self.host}:{self.port} ({e})") from e

    def close(self) -> None:
        """No persistent socket to close; kept so callers can use `with`."""

    def cmd(self, name: str, **fields: Any) -> Dict[str, Any]:
        """Send one command, return the parsed reply. Raises on ok:false."""
        reply = self.raw(name, **fields)
        if not reply.get("ok", False):
            raise DebugError(f"{name}: {reply.get('error', reply.get('err', reply))}")
        return reply

    def raw(self, name: str, **fields: Any) -> Dict[str, Any]:
        """Like cmd() but returns ok:false replies instead of raising."""
        req = {"id": self._next_id, "cmd": name}
        self._next_id += 1
        req.update(fields)
        line = (json.dumps(req) + "\n").encode()

        try:
            sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as e:
            raise DebugError(f"{name}: no debug server on "
                             f"{self.host}:{self.port} ({e})") from e
        try:
            sock.settimeout(self.timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                sock.sendall(line)
            except OSError as e:
                raise DebugError(f"{name}: send failed ({e})") from e

            buf = bytearray()
            while True:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout as e:
                    raise DebugError(
                        f"{name}: timed out after {self.timeout}s "
                        f"({len(buf)} bytes received)") from e
                except OSError as e:
                    raise DebugError(f"{name}: recv failed ({e})") from e
                if not chunk:
                    break
                buf += chunk
                # The server ends every reply with a newline and then closes.
                # Stopping at the newline avoids waiting on the close for peers
                # (DuckStation) that keep the socket around.
                if buf.endswith(b"\n") or b"\n" in buf:
                    break
        finally:
            sock.close()

        if not buf:
            raise DebugError(f"{name}: server closed without replying")
        head = bytes(buf).split(b"\n", 1)[0]
        try:
            return json.loads(head.decode("utf-8", "replace"))
        except json.JSONDecodeError as e:
            raise DebugError(f"{name}: malformed reply ({e}); "
                             f"first 200 bytes: {head[:200]!r}") from e

    # -- convenience wrappers used by the CLIs ------------------------------

    def frame(self) -> int:
        return int(self.cmd("ping").get("frame", 0))

    def ring_span(self) -> Dict[str, Any]:
        """Which frames the GP0 ring can still be asked for.

        This is the replacement for pausing. The ring holds ~1M packets, which
        is several hundred frames, so the workflow is: let the game run, and
        capture the frame AFTER you have seen the bug -- along with the frames
        leading up to it. Reaching backwards beats freezing forwards.
        """
        r = self.cmd("gpu_ring_stats")
        return {
            "oldest": int(r.get("oldest_frame", 0)),
            "newest": int(r.get("newest_frame", 0)),
            "total": int(r.get("total", 0)),
            "capacity": int(r.get("capacity", 0)),
            "max_words": int(r.get("max_words", 12)),
        }

    # pause / continue / step / run_to_frame were REMOVED from psx-runtime --
    # runtime/src/debug_server.c registers them only as handlers that return an
    # error, because pause-step-read synthesizes a snapshot instead of reading
    # the history the runtime already records, and it once turned a dropped
    # client into an apparent freeze. They still work on the DuckStation oracle,
    # which is why these wrappers exist at all; against psx-runtime they raise
    # with the server's own migration message.

    def run_to_frame(self, frame: int) -> Dict[str, Any]:
        """DuckStation only. Raises against psx-runtime, which removed it."""
        return self.cmd("run_to_frame", frame=int(frame))

    def screenshot(self, path: str) -> Dict[str, Any]:
        """Write a PNG of the presented frame.

        The native server calls this `screenshot_file`; the DuckStation oracle
        patch calls the same operation `screenshot`. Try both so one caller
        works against either end.
        """
        r = self.raw("screenshot_file", path=path)
        if r.get("ok"):
            return r
        r2 = self.raw("screenshot", path=path)
        if r2.get("ok"):
            return r2
        raise DebugError(f"screenshot: {r2.get('error', r.get('error', 'failed'))}")


# ---------------------------------------------------------------------------
# GP0 decoding
# ---------------------------------------------------------------------------

def _sx11(v: int) -> int:
    """Sign-extend an 11-bit GPU coordinate."""
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


def parse_vertex(word: int) -> Tuple[int, int]:
    return _sx11(word & 0xFFFF), _sx11((word >> 16) & 0xFFFF)


def parse_color(word: int) -> Tuple[int, int, int]:
    return word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF


def poly_flags(op: int) -> Dict[str, bool]:
    return {
        "gouraud": bool(op & 0x10),
        "quad": bool(op & 0x08),
        "textured": bool(op & 0x04),
        "semi": bool(op & 0x02),
        "raw_tex": bool(op & 0x01),
    }


def poly_words(op: int) -> int:
    f = poly_flags(op)
    n = 4 if f["quad"] else 3
    return 1 + n * (2 if f["textured"] else 1) + (n - 1 if f["gouraud"] else 0)


def rect_words(op: int) -> int:
    variable = ((op >> 3) & 3) == 0
    return 1 + 1 + (1 if op & 0x04 else 0) + (1 if variable else 0)


RECT_SIZES = {1: (1, 1), 2: (8, 8), 3: (16, 16)}


def op_name(op: int) -> str:
    """Human name for a GP0 opcode. Primitive names spell out their flags,
    because the flags are the diagnosis."""
    if op == 0x00:
        return "nop"
    if op == 0x01:
        return "clear_cache"
    if op == 0x02:
        return "fill_rect"
    if op == 0x03:
        return "unknown_03"
    if 0x20 <= op <= 0x3F:
        f = poly_flags(op)
        s = "PolyG" if f["gouraud"] else "PolyF"
        if f["textured"]:
            s += "T"
        s += "4" if f["quad"] else "3"
        if f["semi"]:
            s += "+semi"
        if f["raw_tex"] and f["textured"]:
            s += "+raw"
        return s
    if 0x40 <= op <= 0x5F:
        s = "LineG" if op & 0x10 else "LineF"
        if op & 0x08:
            s = "Poly" + s
        if op & 0x02:
            s += "+semi"
        return s
    if 0x60 <= op <= 0x7F:
        size = (op >> 3) & 3
        s = {0: "Rect", 1: "Rect1", 2: "Rect8", 3: "Rect16"}[size]
        if op & 0x04:
            s = "Sprite" + s[4:]
        if op & 0x02:
            s += "+semi"
        if op & 0x01 and op & 0x04:
            s += "+raw"
        return s
    if 0x80 <= op <= 0x9F:
        return "copy_vram_vram"
    if 0xA0 <= op <= 0xBF:
        return "copy_cpu_vram"
    if 0xC0 <= op <= 0xDF:
        return "copy_vram_cpu"
    return {
        0xE1: "draw_mode(E1)",
        0xE2: "tex_window(E2)",
        0xE3: "draw_area_tl(E3)",
        0xE4: "draw_area_br(E4)",
        0xE5: "draw_offset(E5)",
        0xE6: "mask_bit(E6)",
    }.get(op, f"op_{op:02X}")


def op_kind(op: int) -> str:
    if 0x20 <= op <= 0x3F:
        return "poly"
    if 0x40 <= op <= 0x5F:
        return "line"
    if 0x60 <= op <= 0x7F:
        return "rect"
    if op == 0x02:
        return "fill"
    if 0x80 <= op <= 0xDF:
        return "copy"
    if 0xE0 <= op <= 0xEF:
        return "env"
    return "other"


class GpuState:
    """Draw-mode state carried between packets while walking a frame.

    This is the part that makes the decode faithful rather than plausible: the
    semi-transparency mode of an untextured polygon or a rectangle comes from
    whatever the last GP0(E1) -- or the last textured polygon's own tpage word
    -- left in draw-mode state, not from the packet itself.
    """

    def __init__(self) -> None:
        self.offset = (0, 0)
        self.area_tl = (0, 0)
        self.area_br = (1023, 511)
        self.stp = 0
        self.texpage_x = 0
        self.texpage_y = 0
        self.tex_colors = 0
        self.mask_set = False
        self.mask_check = False

    def set_tpage(self, tpage_word: int) -> None:
        self.texpage_x = tpage_word & 0xF
        self.texpage_y = (tpage_word >> 4) & 1
        self.stp = (tpage_word >> 5) & 3
        self.tex_colors = (tpage_word >> 7) & 3

    def apply_env(self, op: int, w0: int) -> None:
        if op == 0xE1:
            self.set_tpage(w0 & 0xFFFF)
        elif op == 0xE3:
            self.area_tl = (w0 & 0x3FF, (w0 >> 10) & 0x1FF)
        elif op == 0xE4:
            self.area_br = (w0 & 0x3FF, (w0 >> 10) & 0x1FF)
        elif op == 0xE5:
            self.offset = (_sx11(w0 & 0x7FF), _sx11((w0 >> 11) & 0x7FF))
        elif op == 0xE6:
            self.mask_set = bool(w0 & 1)
            self.mask_check = bool(w0 & 2)

    def snapshot(self) -> Dict[str, Any]:
        return {
            "offset": list(self.offset),
            "area": [self.area_tl[0], self.area_tl[1], self.area_br[0], self.area_br[1]],
            "stp": self.stp,
            "texpage": [self.texpage_x, self.texpage_y, self.tex_colors],
            "mask": [self.mask_set, self.mask_check],
        }


def _hexw(words: Sequence[Any]) -> List[int]:
    out = []
    for w in words:
        out.append(int(w, 16) if isinstance(w, str) else int(w))
    return out


def decode_entry(entry: Dict[str, Any], state: GpuState) -> Dict[str, Any]:
    """Decode one gpu_frame_dump ring entry against the running draw state.

    `state` is mutated: env commands and textured-poly tpage latches change it
    for everything that follows, exactly as on hardware.
    """
    op = int(entry["op"], 16) if isinstance(entry["op"], str) else int(entry["op"])
    words = _hexw(entry.get("w", []))
    n_words = int(entry.get("n", len(words)))
    kind = op_kind(op)

    prim: Dict[str, Any] = {
        "seq": int(entry.get("seq", 0)),
        "op": op,
        "op_name": op_name(op),
        "kind": kind,
        "n_words": n_words,
        "have_words": len(words),
        "truncated": n_words > len(words),
        "func": entry.get("func", "0x00000000"),
        "pc": entry.get("pc", "0x00000000"),
        "ra": entry.get("ra", "0x00000000"),
        "src": entry.get("src", "0x00000000"),
        "ot": int(entry.get("ot", 0xFFFF)),
        "words": [f"0x{w:08X}" for w in words],
        "verts": [],
        "colors": [],
        "uvs": [],
        "semi": False,
        "stp": state.stp,
        "textured": False,
        "raw_tex": False,
        "gouraud": False,
        "size": None,
        "clut": None,
        "tpage": None,
    }
    if not words:
        prim["state"] = state.snapshot()
        return prim

    if kind == "env":
        state.apply_env(op, words[0])
        prim["state"] = state.snapshot()
        prim["stp"] = state.stp
        return prim

    ox, oy = state.offset

    if kind == "poly":
        f = poly_flags(op)
        n = 4 if f["quad"] else 3
        prim.update(semi=f["semi"], textured=f["textured"],
                    raw_tex=f["raw_tex"] and f["textured"], gouraud=f["gouraud"])
        # Hardware latches the poly's own tpage word into draw-mode state, so
        # do it before reading stp -- and do it even if the packet is truncated
        # past the vertices, because the real GPU latched it too.
        tp_idx = 5 if f["gouraud"] else 4
        if f["textured"] and len(words) > tp_idx:
            tpage_word = (words[tp_idx] >> 16) & 0xFFFF
            state.set_tpage(tpage_word)
            prim["tpage"] = tpage_word
        prim["stp"] = state.stp
        i = 0
        color = parse_color(words[0])
        i = 1
        for v in range(n):
            if f["gouraud"] and v > 0:
                if i >= len(words):
                    break
                color = parse_color(words[i])
                i += 1
            if i >= len(words):
                break
            prim["verts"].append([parse_vertex(words[i])[0] + ox,
                                  parse_vertex(words[i])[1] + oy])
            prim["colors"].append(list(color))
            i += 1
            if f["textured"]:
                if i >= len(words):
                    break
                uvw = words[i]
                prim["uvs"].append([uvw & 0xFF, (uvw >> 8) & 0xFF])
                if v == 0:
                    prim["clut"] = (uvw >> 16) & 0xFFFF
                i += 1

    elif kind == "rect":
        variable = ((op >> 3) & 3) == 0
        prim.update(semi=bool(op & 0x02), textured=bool(op & 0x04),
                    raw_tex=bool(op & 0x01) and bool(op & 0x04))
        prim["stp"] = state.stp
        color = parse_color(words[0])
        if len(words) > 1:
            x, y = parse_vertex(words[1])
            prim["verts"].append([x + ox, y + oy])
            prim["colors"].append(list(color))
        i = 2
        if prim["textured"] and len(words) > i:
            uvw = words[i]
            prim["uvs"].append([uvw & 0xFF, (uvw >> 8) & 0xFF])
            prim["clut"] = (uvw >> 16) & 0xFFFF
            i += 1
        if variable:
            if len(words) > i:
                prim["size"] = [words[i] & 0x3FF, (words[i] >> 16) & 0x1FF]
        else:
            prim["size"] = list(RECT_SIZES[(op >> 3) & 3])

    elif kind == "line":
        gouraud = bool(op & 0x10)
        polyline = bool(op & 0x08)
        prim.update(semi=bool(op & 0x02), gouraud=gouraud)
        prim["stp"] = state.stp
        prim["polyline"] = polyline
        i = 0
        color = parse_color(words[0])
        i = 1
        while i < len(words):
            if words[i] == 0x55555555:
                break
            if gouraud and prim["verts"]:
                color = parse_color(words[i])
                i += 1
                if i >= len(words):
                    break
            x, y = parse_vertex(words[i])
            prim["verts"].append([x + ox, y + oy])
            prim["colors"].append(list(color))
            i += 1

    elif kind == "fill":
        # GP0(02): fill is in raw VRAM coordinates -- the draw offset and the
        # draw area do NOT apply, and it ignores the mask bit.
        prim["colors"].append(list(parse_color(words[0])))
        if len(words) > 1:
            prim["verts"].append([words[1] & 0x3FF, (words[1] >> 16) & 0x1FF])
        if len(words) > 2:
            prim["size"] = [words[2] & 0x3FF, (words[2] >> 16) & 0x1FF]

    elif kind == "copy":
        if len(words) > 1:
            prim["verts"].append([words[1] & 0x3FF, (words[1] >> 16) & 0x1FF])
        if op < 0xA0 and len(words) > 2:
            prim["verts"].append([words[2] & 0x3FF, (words[2] >> 16) & 0x1FF])
            if len(words) > 3:
                prim["size"] = [words[3] & 0x3FF, (words[3] >> 16) & 0x1FF]
        elif len(words) > 2:
            prim["size"] = [words[2] & 0x3FF, (words[2] >> 16) & 0x1FF]
        if entry.get("bld"):
            prim["bld"] = entry["bld"]

    prim["state"] = state.snapshot()
    return prim


def decode_entries(entries: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Decode a whole frame in issue order. Order matters -- see GpuState."""
    state = GpuState()
    ordered = sorted(entries, key=lambda e: int(e.get("seq", 0)))
    return [decode_entry(e, state) for e in ordered]


# ---------------------------------------------------------------------------
# Attribution
# ---------------------------------------------------------------------------

def attribute(prims: Sequence[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    """Group decoded primitives by the guest function that issued them.

    The per-function numbers are the whole point: a function whose primitives
    all lost their semi-transparency bit between a good and a bad frame is the
    function to go read.
    """
    out: Dict[str, Dict[str, Any]] = {}
    for p in prims:
        fn = p.get("func", "0x00000000")
        rec = out.setdefault(fn, {
            "func": fn,
            "packets": 0,
            "drawing": 0,
            "semi": 0,
            "textured": 0,
            "ops": {},
            "stp_modes": {},
            "ot_min": None,
            "ot_max": None,
            "ras": [],
            "bbox": None,
        })
        rec["packets"] += 1
        name = p["op_name"]
        rec["ops"][name] = rec["ops"].get(name, 0) + 1
        ra = p.get("ra")
        if ra and ra not in rec["ras"] and len(rec["ras"]) < 16:
            rec["ras"].append(ra)
        if p["kind"] in ("poly", "rect", "line", "fill"):
            rec["drawing"] += 1
            if p.get("semi"):
                rec["semi"] += 1
                m = STP_MODES.get(p.get("stp", 0), "?")
                rec["stp_modes"][m] = rec["stp_modes"].get(m, 0) + 1
            if p.get("textured"):
                rec["textured"] += 1
            ot = p.get("ot", 0xFFFF)
            if ot != 0xFFFF:
                rec["ot_min"] = ot if rec["ot_min"] is None else min(rec["ot_min"], ot)
                rec["ot_max"] = ot if rec["ot_max"] is None else max(rec["ot_max"], ot)
            bb = prim_bbox(p)
            if bb:
                rec["bbox"] = bb if rec["bbox"] is None else _union(rec["bbox"], bb)
    return out


def prim_bbox(p: Dict[str, Any]) -> Optional[List[int]]:
    verts = p.get("verts") or []
    if not verts:
        return None
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    size = p.get("size")
    if size and p["kind"] in ("rect", "fill"):
        x1 = x0 + max(0, size[0] - 1)
        y1 = y0 + max(0, size[1] - 1)
    return [x0, y0, x1, y1]


def _union(a: Sequence[int], b: Sequence[int]) -> List[int]:
    return [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3])]


# ---------------------------------------------------------------------------
# Capture / dump I/O
# ---------------------------------------------------------------------------

def capture(conn: DebugConn, frame: Optional[int] = None, count: int = 65536,
            label: str = "", verify_ring: bool = True) -> Dict[str, Any]:
    """Pull one frame's GP0 ring and decode it into a dump dict.

    Two failure modes are turned into errors rather than empty dumps, because
    both look exactly like "that frame drew nothing", which is a conclusion you
    might act on:

      * the frame has fallen out of the ring (or has not happened yet), and
      * the peer answered ok with no `entries` key at all -- which is what a
        stub or a mismatched server does.
    """
    span = None
    if verify_ring:
        try:
            span = conn.ring_span()
        except DebugError:
            span = None   # older server without gpu_ring_stats; carry on
    if frame is None:
        frame = span["newest"] if span else conn.frame()
    if span and span["total"] > 0 and not (span["oldest"] <= frame <= span["newest"]):
        raise DebugError(
            f"frame {frame} is not in the GP0 ring (it holds {span['oldest']}.."
            f"{span['newest']}). Capture closer to the moment, or raise the ring "
            f"size; a dump of an evicted frame is empty, not zero-drawn.")

    reply = conn.cmd("gpu_frame_dump", frame=int(frame), count=int(count))
    if "entries" not in reply:
        raise DebugError(
            "gpu_frame_dump replied ok but carried no 'entries' -- this is not a "
            "psx-runtime debug server. Check what is actually listening on "
            f"{conn.host}:{conn.port}.")
    entries = reply.get("entries", [])
    prims = decode_entries(entries)
    raw_count = int(reply.get("count", len(entries)))
    return {
        "kind": DUMP_KIND,
        "version": DUMP_VERSION,
        "label": label,
        "frame": int(reply.get("frame", frame)),
        "source": {"host": conn.host, "port": conn.port},
        "ring": span,
        "raw_count": raw_count,
        "requested": int(count),
        "capped": raw_count >= int(count),
        "max_words": int(reply.get("max_words", 12)),
        "prims": prims,
        "funcs": attribute(prims),
    }


SUMMARY_KIND = "psx-gpu-frame-summary"


def summarise_dump(dump: Dict[str, Any], dump_name: str = "") -> Dict[str, Any]:
    """A compact view of a dump: totals, opcode and blend-mode histograms, and
    the per-function attribution -- everything except the primitives.

    A busy frame's full dump runs to tens of megabytes. Nothing that only wants
    to know *who drew what* should have to parse that, so the summary is a
    separate artifact and RetComM Studio reads this rather than the dump.
    """
    ops: Dict[str, int] = {}
    modes: Dict[str, int] = {}
    drawing = 0
    truncated = 0
    for p in dump.get("prims", []):
        ops[p["op_name"]] = ops.get(p["op_name"], 0) + 1
        if p.get("truncated"):
            truncated += 1
        if p["kind"] in ("poly", "rect", "line", "fill"):
            drawing += 1
            if p.get("semi"):
                m = STP_MODES.get(p.get("stp", 0), "?")
                modes[m] = modes.get(m, 0) + 1
    funcs = sorted(dump.get("funcs", {}).values(),
                   key=lambda r: (-r.get("drawing", 0), r.get("func", "")))
    return {
        "kind": SUMMARY_KIND,
        "version": 1,
        "label": dump.get("label", ""),
        "frame": dump.get("frame", 0),
        "dump": dump_name,
        "packets": dump.get("raw_count", 0),
        "drawing": drawing,
        "capped": bool(dump.get("capped")),
        "truncated": truncated,
        "area": list(draw_area(dump)),
        "ops": ops,
        "modes": modes,
        "funcs": funcs,
    }


def save_summary(dump: Dict[str, Any], path: str, dump_name: str = "") -> Dict[str, Any]:
    summary = summarise_dump(dump, dump_name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    return summary


def save_dump(dump: Dict[str, Any], path: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dump, f, indent=1)


def load_dump(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        dump = json.load(f)
    if dump.get("kind") != DUMP_KIND:
        raise ValueError(f"{path}: not a {DUMP_KIND} dump")
    return dump


def draw_area(dump: Dict[str, Any]) -> Tuple[int, int, int, int]:
    """The frame's drawing clip rect, from the last GP0(E3)/GP0(E4) seen.

    Falls back to the bounding box of everything drawn, then to 320x240, so a
    dump that never set a draw area still renders something honest.
    """
    area = None
    for p in dump.get("prims", []):
        st = p.get("state")
        if st and st.get("area"):
            area = st["area"]
    if area and area[2] > area[0] and area[3] > area[1]:
        return int(area[0]), int(area[1]), int(area[2]) + 1, int(area[3]) + 1
    bb = None
    for p in dump.get("prims", []):
        b = prim_bbox(p)
        if b:
            bb = b if bb is None else _union(bb, b)
    if bb:
        return max(0, bb[0]), max(0, bb[1]), min(1024, bb[2] + 1), min(512, bb[3] + 1)
    return 0, 0, 320, 240
