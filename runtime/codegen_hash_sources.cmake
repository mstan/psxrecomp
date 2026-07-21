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
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/function_discovery.cpp
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/src/function_discovery.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/recompiler/include/gte_register_classification.h

    # --- Shard COMPILE surface -------------------------------------------------
    # The cg tag must invalidate on anything a shard compiles/links against, not
    # just what the emitter WRITES. A change to the overlay ABI header, the
    # CPUState layout, or the dispatch-shim link contract leaves the emitter
    # sources untouched — so without these the tag stayed put, existing DLLs kept
    # loading, and every NEW shard silently failed to compile against the moved-on
    # runtime (the recurring "header change broke the shards" class). Folding the
    # compile surface in makes such a change reshard SAME-TREE instead of breaking
    # silently. overlay_api.h transitively pulls stdint; cpu_state.h is its only
    # project include; the .inc is the shim every shard links against.
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/overlay_api.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/cpu_state.h
    ${PSXRECOMP_CODEGEN_HASH_ROOT}/runtime/include/overlay_dispatch_preamble.c.inc)
