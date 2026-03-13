# Changelog

## Milestones

### March 2026

- **2026-03-11** Save BIOS stubs working — no "Save Error!"; save data persistence TBD (SIO not yet implemented)
- **2026-03-10** Snapshot system: F6 saves RAM+VRAM state, F7 applies it; scripted test automation via `--load-snapshot`
- **2026-03-10** Semi-transparent batch flushing — mace attack trail smear fixed
- **2026-03-09** Equipment menu CLUT save/restore fixed (GPU GPUREAD reads returning correct VRAM pixels)
- **2026-03-09** Mace weapon attack working — fires, completes, no lag
- **2026-03-09** Pig grab and throw working
- **2026-03-08** Tomba can move and jump — D-pad axes correct
- **2026-03-08** Entity sprites and HUD visible with no flickering

### February 2026

- **2026-02-28** XA-ADPCM audio streaming from disc (background music)
- **2026-02-27** SPU 24-voice ADPCM working; FMV video and audio playback
- **2026-02-27** Main menu fully visible and interactive
- **2026-02-xx** First pixels on screen
- **2026-02-xx** GPU packet pipeline: GP0/GP1 parser → OpenGL renderer
- **2026-02-xx** Runner boots, GLFW window opens, cooperative fibers working
- **2026-02-xx** Recompiler generates `tomba_full.c` (~200K lines, 2955 functions)
