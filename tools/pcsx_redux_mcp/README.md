# PCSX-Redux MCP Server

An MCP (Model Context Protocol) server that lets Claude Code query a live PCSX-Redux instance
during debugging. Claude can read PS1 RAM, VRAM, registers, inject button presses, and execute
arbitrary Lua — all against a real PS1 running the game, as ground truth for comparing behavior
against PSXRecomp.

## How it works

- `server.py` — Python MCP server (SSE transport, port 8078). Exposes tools to Claude Code.
- `startup.lua` — Lua script loaded into PCSX-Redux. Polls a temp file each frame and executes
  any Lua code written to it, returning results. This is the IPC bridge between the Python server
  and PCSX-Redux's Lua runtime.
- `navigate_to_gameplay.lua` — Helper script that auto-navigates from boot to gameplay and saves
  a checkpoint state. Useful for getting to a known position quickly.

## Requirements

- [PCSX-Redux](https://github.com/grumpycoders/pcsx-redux) (Windows build)
- Python 3.8+
- Dependencies: `pip install -r requirements.txt`

## Setup

### 1. Enable PCSX-Redux web server

In PCSX-Redux: **Configuration → Emulation → Web Server**, set port to `8079` and enable it.

### 2. Load your game and open the Lua console

Start your game in PCSX-Redux. Open the Lua console via **Debug → Lua Console**.

### 3. Load the startup script

In the Lua console, run:

```lua
loadfile("path/to/tools/pcsx_redux_mcp/startup.lua")()
```

Replace the path with the actual path on your machine. You should see:
```
[psxrecomp-mcp] ready — listener stored as _psxrecomp_listener
```

### 4. Start the MCP server

```bash
cd tools/pcsx_redux_mcp
pip install -r requirements.txt
python server.py
```

The server starts on port 8078.

### 5. Configure Claude Code

Add to your Claude Code MCP settings (`~/.claude/settings.json`):

```json
{
  "mcpServers": {
    "pcsx-redux": {
      "type": "sse",
      "url": "http://localhost:8078/sse"
    }
  }
}
```

Reconnect Claude Code (`/mcp`) and the PCSX tools will be available.

## Available tools

| Tool | Description |
|------|-------------|
| `pcsx_status` | Get emulation status |
| `pcsx_pause` / `pcsx_resume` | Pause and resume emulation |
| `pcsx_reset_soft` / `pcsx_reset_hard` | Reset the PS1 |
| `pcsx_dump_vram` | Dump full VRAM or a sub-region as PNG |
| `pcsx_screenshot` | Crop VRAM to display area and save PNG |
| `pcsx_read_ram` | Read PS1 RAM at any address (hex + word view) |
| `pcsx_read_scratchpad` | Read PS1 scratchpad (1KB at 0x1F800000) |
| `pcsx_read_registers` | Read all CPU registers (GPR + PC) |
| `pcsx_save_state` | Save emulator state to file |
| `pcsx_load_state` | Load a previously saved state |
| `pcsx_press_button` | Inject a controller button press for N frames |
| `pcsx_exec_lua` | Execute arbitrary Lua in PCSX-Redux |

## navigate_to_gameplay.lua

Auto-navigates from boot to the first gameplay area and saves a checkpoint state. Edit the
`ISO_PATH` and `SAVE_PATH` variables at the top of the file to match your paths, then run in
the Lua console:

```lua
loadfile("path/to/tools/pcsx_redux_mcp/navigate_to_gameplay.lua")()
```

## Caveats

- **Paths are hardcoded** — `startup.lua` and `navigate_to_gameplay.lua` contain hardcoded paths
  (`F:/Projects/psxrecomp-v2/...`). Update these to match your setup before use.
- **Save state load is unreliable** — `pcsx_load_state` works inconsistently. Starting PCSX-Redux
  fresh and navigating manually is more reliable for reaching specific game states.
- **Emulation must be running** — most tools require active emulation. Paused is fine for RAM/VRAM
  reads; fully stopped will not work.
- **IPC latency** — the Lua bridge fires once per frame (GPU::Vsync). Commands are processed with
  up to one frame of latency (~16ms at 60Hz).
