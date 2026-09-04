# v0.3.2-alpha

- Replaces OpenGL temporal frame blending's shared second context and
  presentation thread with a deadline-driven path on the renderer's original
  thread and context. All OpenGL swaps now have one owner, no application mutex
  spans pacing or driver work, and the stock path is unchanged when disabled.
- Documents the feature as temporal blending rather than motion-vector frame
  generation and adds cadence plus context-ownership regression tests.

---

# PSXRecomp OpenBIOS integration

OpenBIOS is now a first-class, redistributable runtime backend.

Highlights:

- Packages the PSXRecomp v4 BIOS runtime as `PSXRecomp.exe`.
- Bundles PCSX-Redux OpenBIOS and its MIT notice.
- Uses OpenBIOS when the player has not explicitly selected another BIOS.
- Keeps a verified, user-provided `SCPH1001.BIN` as an optional runtime choice.
- Lets launcher users clear a retail selection and return to OpenBIOS.
- Prevents savestates created under one BIOS from loading under the other.
- Adds Xbox-style controller support through SDL/XInput.
- Adds configurable controller mapping via `input.ini`.
- Includes the load-delay ownership fix validated in Tomba!, plus OpenBIOS
  smoke coverage in Ape Escape, Tomba! 2, and Mega Man X6.

This package does not include a retail PS1 BIOS, game disc image, generated game
code, save data, or copyrighted Sony/game assets.
