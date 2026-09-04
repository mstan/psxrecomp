# Progressive symbols (PSX)

Discover → label → manipulate. Do not leave newly identified functions as raw
hex in host hooks.

## Map

Game-root `symbols.toml` (scaffolded by New Project Layout):

```toml
[[func]]
pc = 0x80031234
name = "BootEntry"
emit = false
status = "guessed"   # guessed | confirmed | hot
note = "SYSTEM.CNF entry"
```

## Sync

```bash
python3 tools/sync_symbols.py          # → psx_symbols.h (PSX_FN_*)
python3 tools/sync_symbols.py --check  # CI / pre-commit drift check
```

Use `PSX_FN_BootEntry` (or the `func_XXXXXXXX` alias) from host code. Keep
`emit = false` until the entry is safe to own for AOT.

## Status vs SNES

Metal Warriors ships a fuller toolchain (`bank*.cfg` + `MW_FN_*`). PSX sync
today is **header macros only**; recompiler friendly-name promotion can grow
later without changing the map shape.
