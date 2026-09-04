"""Guard the deterministic savestate completion receipt used by load gates."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "include/savestate.h").read_text(encoding="utf-8")
STATE = (ROOT / "src/savestate.c").read_text(encoding="utf-8")
SERVER = (ROOT / "src/debug_server.c").read_text(encoding="utf-8")
INTERRUPTS_H = (ROOT / "include/interrupts.h").read_text(encoding="utf-8")
INTERRUPTS = (ROOT / "src/interrupts.c").read_text(encoding="utf-8")
REWIND = (ROOT / "src/psx_rewind.c").read_text(encoding="utf-8")
DIRTY = (ROOT / "src/dirty_ram_interp.c").read_text(encoding="utf-8")
BOOT_STATE_H = (ROOT / "include/boot_state.h").read_text(encoding="utf-8")
DMA = (ROOT / "src/dma.c").read_text(encoding="utf-8")

# The per-word DMA2 cursor and XA DATA_END pending bit grow the snapshot wire.
# Lock the format change to v7 so an old file is rejected before any state
# section is applied.
assert "#define DMA_GPU_LL_WIRE (4u + (10u * 4u))" in DMA
assert "#define BOOT_STATE_VERSION 7u" in BOOT_STATE_H
assert "#define BOOT_STATE_VERSION_MIN_READ 7u" in BOOT_STATE_H
assert "Reject\n * them at the header before any section changes the live machine." in BOOT_STATE_H

assert "void savestate_status_json(char* buf, size_t cap);" in HEADER
assert '\\"generation\\"' in STATE and '\\"pending\\"' in STATE
assert "s_status_generation++" in STATE
assert '"savestate_status"' in SERVER
assert "savestate_status_json(status, sizeof status)" in SERVER
assert "int psx_irq_resume_context_snapshot_safe(void);" in INTERRUPTS_H
assert "int psx_irq_resume_context_snapshot_safe_at(uint32_t resume_pc);" in INTERRUPTS_H
assert "uint32_t psx_irq_resume_context_snapshot_pc(void);" in INTERRUPTS_H
assert "psx_irq_resume_context_snapshot_safe(void)" in INTERRUPTS
assert "psx_irq_resume_context_snapshot_safe_at(uint32_t resume_pc)" in INTERRUPTS
assert "psx_irq_resume_context_snapshot_pc(void)" in INTERRUPTS
assert "g_cosim_dirty_pump_site == 0" in INTERRUPTS
assert "case 1: /* transfer surface: target PC is materialized in CPUState */" in INTERRUPTS
assert "cands[n++] = psx_irq_resume_context_snapshot_pc();" in STATE
assert "cands[n++] = psx_irq_resume_context_snapshot_pc();" in REWIND
assert "int pc_ok = savestate_resume_pc_ok(pc);" in STATE
assert "(resume_pc == 0u && !pc_matches_cpu) || !snapshot_safe || !pc_ok" in STATE
assert "!psx_irq_resume_context_snapshot_safe_at(pc) || !resume_pc_ok(pc)" in REWIND
assert "psx_check_interrupts_at(cpu, pc)" in DIRTY
print("savestate status protocol guard passed")
