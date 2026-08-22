"""Guard the deterministic savestate completion receipt used by load gates."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/savestate.h").read_text(encoding="utf-8")
STATE = (ROOT / "src/savestate.c").read_text(encoding="utf-8")
SERVER = (ROOT / "src/debug_server.c").read_text(encoding="utf-8")

assert "void savestate_status_json(char* buf, size_t cap);" in HEADER
assert '\\"generation\\"' in STATE and '\\"pending\\"' in STATE
assert "s_status_generation++" in STATE
assert '"savestate_status"' in SERVER
assert "savestate_status_json(status, sizeof status)" in SERVER
print("savestate status protocol guard passed")
