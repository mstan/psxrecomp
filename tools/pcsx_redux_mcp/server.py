#!/usr/bin/env python3
"""
PCSX-Redux MCP Server — SSE transport (matches Ghidra MCP pattern)
Runs as a persistent HTTP server on port 8078.

Start: python server.py
Configure in ~/.claude/settings.json:
  "pcsx-redux": { "type": "sse", "url": "http://localhost:8078/sse" }

PCSX-Redux must have:
  - Web server on port 8079
  - startup.lua loaded in Lua Console
"""

import os
import struct
import time

import requests
import uvicorn
from mcp.server import Server
from mcp.server.sse import SseServerTransport
from mcp.types import Tool, TextContent
from starlette.applications import Starlette
from starlette.routing import Mount, Route

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

PCSX_BASE    = "http://localhost:8079"
MCP_PORT     = 8078
VRAM_OUT     = "C:/temp/pcsx_vram.png"
SHOT_OUT     = "C:/temp/pcsx_screenshot.png"
CMD_FILE     = "C:/temp/pcsx_cmd.lua"
RESULT_FILE  = "C:/temp/pcsx_result.txt"
RAM_SIZE     = 0x200000
SCRATCH_SIZE = 0x400
LUA_TIMEOUT  = 5.0

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

def api_get(path, **kw):
    return requests.get(f"{PCSX_BASE}{path}", timeout=10, **kw)

def api_post(path, **kw):
    return requests.post(f"{PCSX_BASE}{path}", timeout=10, **kw)

# ---------------------------------------------------------------------------
# File-based Lua IPC
# ---------------------------------------------------------------------------

def exec_lua(code: str, timeout: float = LUA_TIMEOUT) -> str:
    if os.path.exists(RESULT_FILE):
        os.remove(RESULT_FILE)
    with open(CMD_FILE, "w", encoding="utf-8") as f:
        f.write(code)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(0.02)
        if os.path.exists(RESULT_FILE):
            with open(RESULT_FILE, "r", encoding="utf-8") as f:
                result = f.read()
            try:
                os.remove(RESULT_FILE)
            except OSError:
                pass
            return result
    if os.path.exists(CMD_FILE):
        try:
            os.remove(CMD_FILE)
        except OSError:
            pass
    return "ERROR: timeout — is emulation running and startup.lua loaded?"

# ---------------------------------------------------------------------------
# VRAM conversion
# ---------------------------------------------------------------------------

def vram_raw_to_png(raw: bytes, path: str, x=0, y=0, w=1024, h=512):
    from PIL import Image
    img = Image.new("RGB", (w, h))
    px = img.load()
    for row in range(h):
        for col in range(w):
            idx = ((y + row) * 1024 + (x + col)) * 2
            if idx + 1 >= len(raw):
                break
            word = struct.unpack_from("<H", raw, idx)[0]
            px[col, row] = (
                (word & 0x1F) << 3,
                ((word >> 5) & 0x1F) << 3,
                ((word >> 10) & 0x1F) << 3,
            )
    img.save(path)

def ps1_to_offset(addr: int) -> int:
    return addr & (RAM_SIZE - 1)

def hex_dump(data: bytes, base: int) -> str:
    lines = []
    for i in range(0, len(data), 16):
        seg = data[i:i+16]
        h = " ".join(f"{b:02X}" for b in seg)
        a = "".join(chr(b) if 32 <= b < 127 else "." for b in seg)
        lines.append(f"  {base+i:08X}  {h:<47}  {a}")
    return "\n".join(lines)

def word_dump(data: bytes, base: int) -> str:
    lines = []
    for i in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, i)[0]
        lines.append(f"  0x{base+i:08X} = 0x{w:08X}  ({w})")
    return "\n".join(lines)

# ---------------------------------------------------------------------------
# MCP server
# ---------------------------------------------------------------------------

app = Server("pcsx-redux")

TOOLS = [
    Tool(name="pcsx_status",
         description="Get PCSX-Redux emulation status.",
         inputSchema={"type": "object", "properties": {}}),
    Tool(name="pcsx_pause",
         description="Pause emulation.",
         inputSchema={"type": "object", "properties": {}}),
    Tool(name="pcsx_resume",
         description="Resume emulation.",
         inputSchema={"type": "object", "properties": {}}),
    Tool(name="pcsx_reset_soft",
         description="Soft-reset the PS1.",
         inputSchema={"type": "object", "properties": {}}),
    Tool(name="pcsx_reset_hard",
         description="Hard-reset the PS1.",
         inputSchema={"type": "object", "properties": {}}),
    Tool(name="pcsx_dump_vram",
         description=(
             "Dump PS1 VRAM as PNG (full 1024x512 or sub-region). "
             "Compare reference VRAM against our renderer's C:/temp/game_vram.png. "
             "Works even when paused."
         ),
         inputSchema={"type": "object", "properties": {
             "x":      {"type": "integer", "description": "Left pixel (default 0)"},
             "y":      {"type": "integer", "description": "Top pixel (default 0)"},
             "width":  {"type": "integer", "description": "Width (default 1024)"},
             "height": {"type": "integer", "description": "Height (default 512)"},
             "out":    {"type": "string",  "description": "Output PNG path"},
         }}),
    Tool(name="pcsx_screenshot",
         description="Crop VRAM to PS1 display area and save PNG. Works even when paused.",
         inputSchema={"type": "object", "properties": {
             "x":      {"type": "integer", "description": "VRAM X origin (default 0)"},
             "y":      {"type": "integer", "description": "VRAM Y origin (default 0)"},
             "width":  {"type": "integer", "description": "Width (default 320)"},
             "height": {"type": "integer", "description": "Height (default 240)"},
         }}),
    Tool(name="pcsx_read_ram",
         description=(
             "Read bytes from PS1 RAM at a PS1 virtual address. "
             "Returns hex dump + 32-bit word view. Works even when paused."
         ),
         inputSchema={"type": "object", "required": ["address", "length"], "properties": {
             "address": {"type": "string",  "description": "Hex address e.g. 0x800A5970"},
             "length":  {"type": "integer", "description": "Bytes to read (max 65536)"},
         }}),
    Tool(name="pcsx_read_scratchpad",
         description=(
             "Read PS1 scratchpad (1KB at 0x1F800000). "
             "Key offsets: 0x174=cam_x, 0x178=cam_y, 0x1CC=load_state, 0x24A=ent_push_count. "
             "Requires emulation running."
         ),
         inputSchema={"type": "object", "properties": {
             "offset": {"type": "integer", "description": "Byte offset (default 0)"},
             "length": {"type": "integer", "description": "Bytes to read (default 256)"},
         }}),
    Tool(name="pcsx_read_registers",
         description="Read all PS1 CPU registers (GPR r0-r31, pc). Requires emulation running.",
         inputSchema={"type": "object", "properties": {}}),
    Tool(name="pcsx_save_state",
         description="Save emulator state to file. Requires emulation running.",
         inputSchema={"type": "object", "required": ["path"], "properties": {
             "path": {"type": "string", "description": "File path e.g. C:/temp/gameplay.state"},
         }}),
    Tool(name="pcsx_load_state",
         description="Load a previously saved emulator state.",
         inputSchema={"type": "object", "required": ["path"], "properties": {
             "path": {"type": "string", "description": "File path to load"},
         }}),
    Tool(name="pcsx_press_button",
         description=(
             "Inject a PS1 controller button press for N frames. "
             "Buttons: Cross, Circle, Square, Triangle, L1, L2, R1, R2, "
             "Up, Down, Left, Right, Start, Select."
         ),
         inputSchema={"type": "object", "required": ["button"], "properties": {
             "button": {"type": "string",  "description": "Button name"},
             "frames": {"type": "integer", "description": "Hold frames (default 1)"},
             "slot":   {"type": "integer", "description": "Controller slot (default 0)"},
         }}),
    Tool(name="pcsx_exec_lua",
         description=(
             "Execute Lua code in PCSX-Redux via file IPC. "
             "Full PCSX API access. Requires emulation running + startup.lua loaded."
         ),
         inputSchema={"type": "object", "required": ["code"], "properties": {
             "code": {"type": "string", "description": "Lua code; use 'return' for a value"},
         }}),
]


@app.list_tools()
async def list_tools():
    return TOOLS


@app.call_tool()
async def call_tool(name: str, arguments: dict):
    import asyncio
    loop = asyncio.get_event_loop()
    try:
        return await loop.run_in_executor(None, _dispatch, name, arguments)
    except requests.exceptions.ConnectionError:
        return [TextContent(type="text", text=(
            "ERROR: Cannot connect to PCSX-Redux on port 8079. "
            "Enable web server: Configuration > Emulation > Web Server port 8079."
        ))]
    except Exception as e:
        return [TextContent(type="text", text=f"ERROR: {type(e).__name__}: {e}")]


def _dispatch(name: str, arguments: dict):

    if name == "pcsx_status":
        return [TextContent(type="text", text=api_get("/api/v1/execution-flow").text)]

    if name == "pcsx_pause":
        api_post("/api/v1/execution-flow?function=pause")
        return [TextContent(type="text", text="Paused.")]

    if name == "pcsx_resume":
        api_post("/api/v1/execution-flow?function=start")
        return [TextContent(type="text", text="Resumed.")]

    if name == "pcsx_reset_soft":
        return [TextContent(type="text", text=exec_lua("PCSX.softResetEmulator()\nreturn 'soft reset'"))]

    if name == "pcsx_reset_hard":
        return [TextContent(type="text", text=exec_lua("PCSX.hardResetEmulator()\nreturn 'hard reset'"))]

    if name == "pcsx_dump_vram":
        x   = arguments.get("x", 0)
        y   = arguments.get("y", 0)
        w   = arguments.get("width", 1024)
        h   = arguments.get("height", 512)
        out = arguments.get("out", VRAM_OUT)
        raw = api_get("/api/v1/gpu/vram/raw").content
        vram_raw_to_png(raw, out, x, y, w, h)
        return [TextContent(type="text", text=f"VRAM saved: {out} ({w}x{h} at {x},{y})")]

    if name == "pcsx_screenshot":
        x = arguments.get("x", 0)
        y = arguments.get("y", 0)
        w = arguments.get("width", 320)
        h = arguments.get("height", 240)
        raw = api_get("/api/v1/gpu/vram/raw").content
        vram_raw_to_png(raw, SHOT_OUT, x, y, w, h)
        return [TextContent(type="text", text=f"Screenshot saved: {SHOT_OUT} ({w}x{h})")]

    if name == "pcsx_read_ram":
        addr   = int(arguments["address"], 16)
        length = min(int(arguments["length"]), 65536)
        offset = ps1_to_offset(addr)
        raw    = api_get("/api/v1/cpu/ram/raw").content
        chunk  = raw[offset:offset+length]
        out = [f"PS1 RAM @ 0x{addr:08X} ({length} bytes):",
               hex_dump(chunk, addr), "", "32-bit LE words:", word_dump(chunk, addr)]
        return [TextContent(type="text", text="\n".join(out))]

    if name == "pcsx_read_scratchpad":
        offset = arguments.get("offset", 0)
        length = min(arguments.get("length", 256), SCRATCH_SIZE)
        lua = f"local p=PCSX.getScratchPtr()\nlocal t={{}}\nfor i={offset},{offset+length-1} do t[#t+1]=string.format('%02x',p[i]) end\nreturn table.concat(t,' ')"
        result = exec_lua(lua)
        try:
            raw  = bytes(int(h, 16) for h in result.split())
            base = 0x1F800000 + offset
            out  = [f"Scratchpad @ 0x{base:08X} ({length} bytes):",
                    hex_dump(raw, base), "", "32-bit LE words:", word_dump(raw, base)]
            return [TextContent(type="text", text="\n".join(out))]
        except Exception:
            return [TextContent(type="text", text=result)]

    if name == "pcsx_read_registers":
        lua = (
            "local r=PCSX.getRegisters()\nlocal g=r.GPR\n"
            "local names={'r0','at','v0','v1','a0','a1','a2','a3',"
            "'t0','t1','t2','t3','t4','t5','t6','t7',"
            "'s0','s1','s2','s3','s4','s5','s6','s7',"
            "'t8','t9','k0','k1','gp','sp','fp','ra'}\n"
            "local out={}\n"
            "for i,n in ipairs(names) do out[#out+1]=string.format('%-4s = 0x%08x',n,g.r[i-1]) end\n"
            "out[#out+1]=string.format('pc   = 0x%08x',r.pc)\n"
            "return table.concat(out,'\\n')"
        )
        return [TextContent(type="text", text=exec_lua(lua))]

    if name == "pcsx_save_state":
        path = arguments["path"].replace("\\", "/")
        lua  = f"local s=PCSX.createSaveState()\nlocal f=Support.File.open('{path}','CREATE')\nf:write(s)\nf:close()\nreturn 'Saved to {path}'"
        return [TextContent(type="text", text=exec_lua(lua))]

    if name == "pcsx_load_state":
        path = arguments["path"].replace("\\", "/")
        lua  = f"local f=Support.File.open('{path}','READ')\nPCSX.loadSaveState(f)\nf:close()\nreturn 'Loaded from {path}'"
        return [TextContent(type="text", text=exec_lua(lua))]

    if name == "pcsx_press_button":
        button = arguments["button"]
        frames = arguments.get("frames", 1)
        slot   = arguments.get("slot", 0)
        lua = (
            f"local pad=PCSX.SIO0.slots[{slot}+1].pads[1]\n"
            f"local btn=PCSX.CONSTS.PAD.BUTTON.{button.upper()}\n"
            f"pad.setOverride(btn,true)\n"
            f"local n=0\nlocal L\n"
            f"L=PCSX.Events.createEventListener('GPU::Vsync',function()\n"
            f"  n=n+1\n  if n>={frames} then pad.clearOverride(btn) L:remove() end\n"
            f"end)\n"
            f"return 'Button {button} for {frames} frames'"
        )
        return [TextContent(type="text", text=exec_lua(lua))]

    if name == "pcsx_exec_lua":
        return [TextContent(type="text", text=exec_lua(arguments["code"]))]

    return [TextContent(type="text", text=f"Unknown tool: {name}")]


# ---------------------------------------------------------------------------
# SSE server (same pattern as Ghidra MCP)
# ---------------------------------------------------------------------------

def make_starlette_app(server: Server) -> Starlette:
    sse = SseServerTransport("/messages/")

    async def handle_sse(request):
        async with sse.connect_sse(request.scope, request.receive, request._send) as streams:
            await server.run(streams[0], streams[1], server.create_initialization_options())

    return Starlette(routes=[
        Route("/sse", endpoint=handle_sse),
        Mount("/messages/", app=sse.handle_post_message),
    ])


if __name__ == "__main__":
    print(f"PCSX-Redux MCP server starting on port {MCP_PORT}")
    print(f"Configure Claude Code: \"pcsx-redux\": {{\"type\": \"sse\", \"url\": \"http://localhost:{MCP_PORT}/sse\"}}")
    starlette_app = make_starlette_app(app)
    uvicorn.run(starlette_app, host="127.0.0.1", port=MCP_PORT)
