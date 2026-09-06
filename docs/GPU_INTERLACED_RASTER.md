# Interlaced primitive drawing

The GPU preserves one row parity while drawing in 480-line interlaced mode
when GP0(E1h) prohibits drawing to the display area. The display origin and
current field select that parity. Progressive modes and the draw-to-display
override draw both parities. Fills, copies and VRAM transfers keep both fields.

`gpu_raster_skipped_row()` reads current GPU state for each primitive. The
renderer facade applies the field mask with native-row clip rectangles and
restores the original clip. This also preserves the corresponding rows in
scaled surfaces. Each clipped triangle retains its precision and perspective
metadata, which hardware backends otherwise consume on the first submission.
No title address or title detection selects this behavior.

Pro Pinball: Timeshock! USA SLUS-00639 repeatedly draws and reads a test pixel
at (1020,511) during startup. Frozen source 63446a28 always returned the drawn
pixel and stayed black. This correction lets the guest's existing field test
finish. The same disc, BIOS and settings then reach the table.

The field latch uses the runtime's existing vertical-blank model. This change
does not claim scanline-accurate timing. Row clipping can increase submissions
for large interlaced primitives; no general performance claim is made.

## Verification

Configure `runtime/` with `BUILD_TESTING=ON`, then run
`ctest --test-dir <build> -R gpu_interlace_render_test --output-on-failure`.
The fixture covers the mode predicate, origin/field parity, all nine primitive
families, clip restoration, transfers, mode changes, scaled output and
triangle metadata consumed by a backend. The original renderer fails the
same row-preservation assertions.

## Prior art

- [PSX-SPX GPU specification](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/)
  describes interlace affecting draw commands.
- [DuckStation difficult games](https://github.com/stenzek/duckstation/wiki/Difficult-to-Emulate-Games)
  identifies required line skipping for Pro Pinball startup.

These sources supplied behavioral evidence. The implementation and regression
fixture were written independently. Exact title run receipts and route limits
are maintained by the Timeshock correction project.
