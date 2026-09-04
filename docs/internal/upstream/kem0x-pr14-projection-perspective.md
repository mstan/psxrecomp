# PR #14 projection precision and perspective textures

Source: [kem0x PR #14](https://github.com/mstan/psxrecomp/pull/14), commit
[`ec889d6e5956fc4f34acfee41d87f30b24123b7f`](https://github.com/mstan/psxrecomp/commit/ec889d6e5956fc4f34acfee41d87f30b24123b7f).

This branch parks two coupled, opt-in visual experiments together:

- retain discarded GTE 16.16 screen fractions and use exact packed-coordinate
  matches for supersampled software-renderer vertices;
- track exact SWC2 RAM provenance through generated, strict, dirty-RAM,
  overlay, and fallback interpreters so textured world polygons can use
  perspective-correct UV interpolation without guessing from screen position.

They share the same projection side cache, GP0 triangle preparation, and
software-renderer consumption path; splitting those internals would duplicate
most of the risk-bearing machinery. Both remain disabled until their existing
generic setters are called.

Excluded: all Web host exports and every title-specific state, action, address,
or presentation hook from the source commit. The separate 60 Hz presentation
experiment is parked on its own branch. Authorship is retained in the commit
trailer.

## UNPARKED

Both are now reachable enhancements, wired up in response to
[issue #92](https://github.com/mstan/psxrecomp/issues/92):

- `[video] geometry_correction` and `[video] perspective_texturing` call the
  generic setters this branch left dangling (`gte_geometry_correction_set`,
  `gpu_texture_correction_set`), from `game.toml` or `settings.toml`;
- the software-renderer-only consumption path became a `GpuRenderBackend`
  sideband (`gr_set_precise_triangle` / `gr_set_perspective_triangle`), and
  OpenGL and Vulkan now consume it too — the parked code only reached the
  software rasterizer, while the default renderer is OpenGL.

See `docs/ENHANCEMENTS.md` §G1 for the shipped design. This file stays as the record
of what was imported and what was deliberately left out.
