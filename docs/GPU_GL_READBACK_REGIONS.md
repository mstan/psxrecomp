# OpenGL native readback regions

OpenGL owns the rendered video memory. A CPU read must see all prior GPU writes.
The backend now keeps a separate conservative rectangle for GPU writes that
have not reached the CPU array. It reads that rectangle into the full-width CPU
array with an explicit row stride. It then clears the CPU readback debt.

The rectangle includes clipped primitives, fill segments and copy destinations.
It also retains framebuffer clearing debt across depth24 transitions. GPU
uploads and queued draws still finish before packing and reading. Texture
sampling can consume packing debt without consuming CPU readback debt. A
conservative union is safe for GPU-to-CPU reads because the GPU image is current;
CPU-to-GPU uploads still use their exact rectangle list.

This does not change raster rules, read values, guest clocks, draw order or
precision. It does not switch OpenGL to software raster ownership. The original
interlaced row clipping remains in place. State recreation clears the new debt;
CPU-authority mode retains its existing no-readback behavior.

## Verification

`runtime/tests/test_gl_readback_region.c` uses an actual hidden SDL/OpenGL
context. It checks the complete native CPU image against full hardware readback
after ordered synthetic workloads. It covers single pixels, odd-width row
stride, disjoint uploads, texture dependencies, wrapping fills, overlapping
copies, masks, blending, clipping, precision margins and state restore.
The hardware rasterizer is shared by the comparison; this proves coherence,
not independent raster accuracy. No retail assets are required.

Run `python runtime/tests/run_gl_readback_region.py --compiler-bin <mingw-bin>
--sdl-root <deps> --output <new-directory>`. The SDL dependency root must contain
`sdl3-src/include` and `sdl3-build/libSDL3.a`. The Windows runner uses hidden
windows. It tests
native and4x scales. `--gl-source <old-gpu_gl_renderer.c> --expect-unbounded`
checks the previous implementation: pixel comparisons pass, and only the small
transfer assertion fails. Each command/result is saved in `receipt.json` under a fresh run directory inside the output root. Configure `runtime/` with `BUILD_TESTING=ON` and `PSX_GL_READBACK_SDL_ROOT=<deps>` on MinGW to register `gl_readback_region_test`; run it with `ctest --test-dir <build> -R gl_readback_region_test --output-on-failure`. Missing dependencies print a configure-time message and do not count as a hardware pass.

## Prior art and scope

Existing renderer coherence code supplied the ordering and dirty-area rules.
DuckStation's software readback path was consulted as an alternative. This correction keeps the frozen branch's GPU
authority and needs no title hook or controller policy change. Future work can
reduce row submissions or use a more precise dirty set if measured demand
justifies it.
