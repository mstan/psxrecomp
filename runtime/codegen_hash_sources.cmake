# codegen_hash_sources.cmake — the CANONICAL list of recompiler codegen sources
# folded into the overlay cache tag hash (hash_codegen.cmake).
#
# Included from BOTH:
#   - runtime/runtime.cmake        (bakes the hash into the runtime's cg tag)
#   - recompiler/CMakeLists.txt    (bakes the SAME hash into psxrecomp-game,
#                                   exposed via `--codegen-hash`)
# so the two hash computations can never drift by list divergence. Any file
# added to the emitter pipeline must be added HERE, once.
#
# Input:  PSXRECOMP_CODEGEN_HASH_ROOT — the psxrecomp repo root.
# Output: PSXRECOMP_CODEGEN_HASH_SRCS — absolute paths to hash.

# NOTE: include the emitter .cpp translation units, not just their headers. A
# change confined to full_function_emitter.cpp (e.g. the 2026-07-03 Class-A
# shell-window overlay-dispatch fix) alters both generated dispatch and overlay
# code but left the hash unchanged when only the .h files were listed — a stale
# overlay-cache hazard (same class as the "ws emission not in cg hash" gap).
set(PSXRECOMP_CODEGEN_HASH_SRCS
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/code_generator.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/mips_decoder.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/control_flow.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/basic_block.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/function_analysis.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/recompiler_patch.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/recompiler_patch.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/full_function_emitter.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/full_function_emitter.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/strict_translator.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/strict_translator.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/pgxp_hook_emitter.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/pgxp_hook_emitter.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/function_discovery.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/function_discovery.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/include/gte_register_classification.h

    # --- image view + walker headers ------------------------------------------
    # Same gap as the .cpp/.h note above, one level down. The image VIEW decides
    # what the emitter is even allowed to see: ps1_exe_parser.h now carries two
    # distinct bounds (the READ bound end_address() and the ANALYSIS bound
    # analysis_end_address(), which stops short of an overlay capture's trailing
    # delay-slot guard words), and ps1_exe_parser.cpp decodes the producer's
    # guard-byte declaration. A change confined to either would move every
    # overlay shard's CFG while leaving the cg tag untouched — precisely the
    # stale-but-same-tag hazard this list exists to prevent. The walker/analysis
    # headers are listed for the same reason: their structs and bounds helpers
    # are part of the emitter contract even when the .cpp files do not move.
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/include/ps1_exe_parser.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/ps1_exe_parser.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/include/control_flow.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/include/function_analysis.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/include/basic_block.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/include/code_generator.h

    # --- Shard COMPILE surface -------------------------------------------------
    # The cg tag must invalidate on anything a shard compiles/links against, not
    # just what the emitter WRITES. A change to the overlay ABI header, the
    # CPUState layout, or the dispatch-shim link contract leaves the emitter
    # sources untouched — so without these the tag stayed put, existing DLLs kept
    # loading, and every NEW shard silently failed to compile against the moved-on
    # runtime (the recurring "header change broke the shards" class). Folding the
    # compile surface in makes such a change reshard SAME-TREE instead of breaking
    # silently. overlay_api.h pulls cpu_state.h and its cycle-helper headers; the
    # .inc is the shim every shard links against.
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/overlay_api.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/cpu_state.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/pgxp_hooks.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/psx_cyc.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/psx_cycles.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/overlay_dispatch_preamble.c.inc)
