cmake_minimum_required(VERSION 3.20)
project(PSXRecomp C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

include("${CMAKE_CURRENT_SOURCE_DIR}/../runtime/chd_dependency.cmake")

add_subdirectory(lib/fmt)

# Vendored fmt 9.1.0 enables its `consteval` format-string check on Apple
# clang via the `__cpp_consteval` clause in core.h, even though the same
# block tries to exclude Apple toolchains — and Apple clang 21 then rejects
# fmt's own literal format strings ("call to consteval function is not a
# constant expression"). Force FMT_CONSTEVAL empty to take fmt's
# non-consteval compile-time-check path. Scoped to non-MSVC so Windows is
# untouched.
if(NOT MSVC)
    target_compile_definitions(fmt PUBLIC FMT_CONSTEVAL=)
endif()

# Rabbitizer MIPS decoder
add_library(rabbitizer STATIC)
target_sources(rabbitizer PRIVATE
    lib/rabbitizer/cplusplus/src/analysis/LoPairingInfo.cpp
    lib/rabbitizer/cplusplus/src/analysis/RegistersTracker.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstrId.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstrIdType.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionBase.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionCpu.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionR3000GTE.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionR5900.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionRsp.cpp
    lib/rabbitizer/src/analysis/RabbitizerLoPairingInfo.c
    lib/rabbitizer/src/analysis/RabbitizerRegistersTracker.c
    lib/rabbitizer/src/analysis/RabbitizerTrackedRegisterState.c
    lib/rabbitizer/src/common/RabbitizerConfig.c
    lib/rabbitizer/src/common/RabbitizerVersion.c
    lib/rabbitizer/src/common/Utils.c
    lib/rabbitizer/src/instructions/RabbitizerInstrCategory.c
    lib/rabbitizer/src/instructions/RabbitizerInstrDescriptor.c
    lib/rabbitizer/src/instructions/RabbitizerInstrId.c
    lib/rabbitizer/src/instructions/RabbitizerInstrIdType.c
    lib/rabbitizer/src/instructions/RabbitizerInstrSuffix.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionCpu/RabbitizerInstructionCpu_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Disassemble.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Examination.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Operand.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerRegister.c
    lib/rabbitizer/src/instructions/RabbitizerRegisterDescriptor.c
)
target_include_directories(rabbitizer PRIVATE
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/rabbitizer/tables
)

# PSXRecomp — MIPS→C recompiler tool
set(PSXRECOMP_GAME_SOURCES
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/control_flow.cpp
    src/function_analysis.cpp
    src/code_generator.cpp
    src/pgxp_hook_emitter.cpp
    src/bios_address_model.cpp
    src/annotations.cpp
    src/config_loader.cpp
    src/recompiler_patch.cpp
    src/main_psx.cpp
)

add_executable(psxrecomp-game ${PSXRECOMP_GAME_SOURCES})
target_include_directories(psxrecomp-game PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
    lib/ELFIO
)
target_link_libraries(psxrecomp-game PRIVATE fmt rabbitizer)
if(MINGW)
    target_link_options(psxrecomp-game PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp-game PRIVATE /STACK:16777216)
endif()

# psxrecomp-toml — PS-X EXE → game.toml generator.
# Standalone tool that analyses a PS-X EXE and generates a complete game.toml
# config with auto-detected fields (load_address, entry_pc, text_size,
# stack_base) and optional JAL-target seed generation.
add_executable(psxrecomp-toml
    src/ps1_exe_parser.cpp
    src/main_toml.cpp
)
target_include_directories(psxrecomp-toml PRIVATE
    include
    lib/fmt/include
)
target_link_libraries(psxrecomp-toml PRIVATE fmt)

# psxrecomp — user-facing media-to-source project generator.
add_executable(psxrecomp
    src/main_cli.cpp
    src/cli_boot_path.cpp
    src/ps1_exe_parser.cpp
    ../runtime/src/cue_sheet.cpp
    ../runtime/src/iso_reader.cpp
)
target_include_directories(psxrecomp PRIVATE
    include
    ../runtime/include
    lib/fmt/include
)
target_link_libraries(psxrecomp PRIVATE fmt chdr-static)
if(MINGW)
    target_link_options(psxrecomp PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp PRIVATE /STACK:16777216)
endif()

# ---- baked codegen hash (stale-binary guard) --------------------------------
# Bake the SAME emitter-source hash the runtime folds into its overlay cache
# tag (runtime/runtime.cmake -> hash_codegen.cmake, shared canonical list in
# runtime/codegen_hash_sources.cmake) into THIS binary, exposed via
# `psxrecomp-game --codegen-hash`. compile_overlays.py compares it against the
# hash it stamps on the cache dir (from --runtime-include) and HARD-FAILS on
# mismatch. Closes the 2026-07-01 Tomba pause-menu wedge class: a stale
# recompiler binary emitting shards that get filed under the CURRENT cg tag
# (read tag == write tag, content stale — invisible to the tag alone).
set(PSXRECOMP_CODEGEN_HASH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/codegen_hash_sources.cmake)
set(_baked_hash_hdr ${CMAKE_CURRENT_BINARY_DIR}/psxrecomp_baked_codegen_hash.h)
add_custom_command(
    OUTPUT  ${_baked_hash_hdr}
    COMMAND ${CMAKE_COMMAND} -DOUT=${_baked_hash_hdr}
            "-DSRCS=${PSXRECOMP_CODEGEN_HASH_SRCS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/hash_codegen.cmake
    DEPENDS ${PSXRECOMP_CODEGEN_HASH_SRCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/hash_codegen.cmake
    COMMENT "Hashing codegen sources -> psxrecomp_baked_codegen_hash.h"
    VERBATIM)
add_custom_target(psxrecomp_baked_codegen_hash DEPENDS ${_baked_hash_hdr})
add_dependencies(psxrecomp-game psxrecomp_baked_codegen_hash)
# Force main_psx.cpp to recompile whenever the hash header content changes
# (same OBJECT_DEPENDS hardening as runtime.cmake — __has_include depfile gap).
set_source_files_properties(src/main_psx.cpp PROPERTIES OBJECT_DEPENDS ${_baked_hash_hdr})
target_include_directories(psxrecomp-game PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# NOTE (2026-07-27): the legacy `PSXRecomp` target was REMOVED here.
# It built ${PSXRECOMP_GAME_SOURCES} — byte-for-byte the same source list as
# `psxrecomp-game` above — and was a leftover from before that target was
# introduced. Because Windows and macOS filesystems are case-insensitive, its
# output `PSXRecomp.exe` collided with the user-facing CLI `psxrecomp.exe`
# (the `psxrecomp` target above): both wrote the SAME file on disk and the
# last one linked won. In practice the CLI lost, so `psxrecomp.exe` shipped in
# `psxrecomp-cli-windows-x86_64.zip` (tools/build_cli.py) was actually the game
# recompiler, with a completely different argument grammar than the README
# documents. The bug is invisible on Linux, which is why it survived.
# The `PSXRecomp.exe` users are told to run in README's "Release Package"
# section is a DIFFERENT artifact — the BIOS-only runtime, whose name comes
# from OUTPUT_NAME in runtime/runtime.cmake — and is unaffected by this removal.
# See the collision guard at the bottom of this file.

# psxrecomp-bios — Phase 1a BIOS boot slice recompiler.
# Strict, fail-loud, ingests bios/SCPH1001.BIN as a flat ROM at 0xBFC00000.
# Independent of PSXRecomp's PS1-EXE entry point. Shares mips_decoder +
# ps1_exe_parser sources because mips_decoder.h depends on PS1Executable
# (used only by decode_executable, which psxrecomp-bios does not call).
add_executable(psxrecomp-bios
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/strict_translator.cpp
    src/pgxp_hook_emitter.cpp
    src/bios_slice_walker.cpp
    src/bios_address_model.cpp
    src/function_discovery.cpp
    src/full_function_emitter.cpp
    src/config_loader.cpp
    src/recompiler_patch.cpp
    src/main_bios.cpp
)
target_include_directories(psxrecomp-bios PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
)
target_link_libraries(psxrecomp-bios PRIVATE fmt rabbitizer)
if(MINGW)
    target_link_options(psxrecomp-bios PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp-bios PRIVATE /STACK:16777216)
endif()

# Released Windows CLI archives must run without a separate MinGW/MSYS2
# install. Prefer llvm-mingw (+ -static) in CI so emitters do not need
# libgcc/libstdc++ harvested from MSYS2. Keep opt-in for local debug builds.
option(PSXRECOMP_STATIC_CLI "Statically link Windows runtime into CLI tools" OFF)
if(PSXRECOMP_STATIC_CLI AND WIN32 AND (MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    foreach(_cli_target psxrecomp psxrecomp-game psxrecomp-toml psxrecomp-bios)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_link_options(${_cli_target} PRIVATE
                -static -static-libgcc -static-libstdc++)
        else()
            # llvm-mingw / Clang: fully static CRT + libc++ / libunwind / pthread.
            target_link_options(${_cli_target} PRIVATE -static)
        endif()
    endforeach()
endif()

# BIOS address-model equivalence test — sweeps every 4-byte address through
# the profile-driven BiosAddressModel and compares against verbatim copies of
# the pre-refactor hardcoded SCPH1001 helpers; also exercises the from_config
# invariants. Run from the framework root: recompiler/build/bios_address_model_test
add_executable(bios_address_model_test
    tests/bios_address_model_test.cpp
    src/bios_address_model.cpp
    src/config_loader.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(bios_address_model_test PRIVATE
    include
    src
    lib/fmt/include
    lib/toml11
)
target_link_libraries(bios_address_model_test PRIVATE fmt)

# L1 decoder conformance test — compares MipsDecoder against Rabbitizer
# oracle for every instruction in SCPH1001.BIN code regions.
add_executable(l1_decoder_test
    tests/l1_decoder_test.cpp
    src/mips_decoder.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(l1_decoder_test PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(l1_decoder_test PRIVATE fmt rabbitizer)

# L2 structural codegen test — verifies StrictTranslator output patterns
# for each MIPS instruction form.
add_executable(l2_structural_test
    tests/l2_structural_test.cpp
    src/mips_decoder.cpp
    src/ps1_exe_parser.cpp
    src/strict_translator.cpp
    src/pgxp_hook_emitter.cpp
)
target_include_directories(l2_structural_test PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(l2_structural_test PRIVATE fmt rabbitizer)

if(BUILD_TESTING)
    add_executable(debug_trace_ranges_test
        ../runtime/tests/test_debug_trace_ranges.c
        ../runtime/src/debug_trace_ranges.c
    )
    target_include_directories(debug_trace_ranges_test PRIVATE
        ../runtime/include)
    add_test(NAME debug_trace_ranges_test COMMAND debug_trace_ranges_test)

    add_executable(cli_boot_path_test
        tests/cli_boot_path_test.cpp
        src/cli_boot_path.cpp
    )
    target_include_directories(cli_boot_path_test PRIVATE src)
    add_test(NAME cli_boot_path_test COMMAND cli_boot_path_test)

    add_executable(recompiler_patch_test
        tests/recompiler_patch_test.cpp
        src/ps1_exe_parser.cpp
        src/mips_decoder.cpp
        src/control_flow.cpp
        src/function_analysis.cpp
        src/code_generator.cpp
        src/pgxp_hook_emitter.cpp
        src/annotations.cpp
        src/bios_address_model.cpp
        src/config_loader.cpp
        src/recompiler_patch.cpp
    )
    target_include_directories(recompiler_patch_test PRIVATE
        include
        src
        lib/rabbitizer/include
        lib/rabbitizer/cplusplus/include
        lib/fmt/include
        lib/toml11
        lib/ELFIO
    )
    target_link_libraries(recompiler_patch_test PRIVATE fmt rabbitizer)
    add_test(NAME recompiler_patch_test COMMAND recompiler_patch_test)

    add_executable(full_function_emitter_test
        tests/full_function_emitter_test.cpp
        src/ps1_exe_parser.cpp
        src/mips_decoder.cpp
        src/strict_translator.cpp
        src/pgxp_hook_emitter.cpp
        src/bios_address_model.cpp
        src/function_discovery.cpp
        src/full_function_emitter.cpp
        src/config_loader.cpp
        src/recompiler_patch.cpp
    )
    target_include_directories(full_function_emitter_test PRIVATE
        include
        src
        lib/rabbitizer/include
        lib/rabbitizer/cplusplus/include
        lib/fmt/include
        lib/toml11
    )
    target_link_libraries(full_function_emitter_test PRIVATE fmt rabbitizer)
    add_test(NAME full_function_emitter_test COMMAND full_function_emitter_test)

    add_executable(recompiler_patch_cxx17_header_test
        tests/recompiler_patch_cxx17_header_test.cpp
    )
    target_include_directories(recompiler_patch_cxx17_header_test PRIVATE src)
    set_target_properties(recompiler_patch_cxx17_header_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME recompiler_patch_cxx17_header_test
             COMMAND recompiler_patch_cxx17_header_test)

    # [video] geometry_correction / perspective_texturing opt-in plumbing.
    add_executable(video_enhancement_settings_test
        tests/video_enhancement_settings_test.cpp
        src/config_loader.cpp
        src/ps1_exe_parser.cpp
    )
    target_include_directories(video_enhancement_settings_test PRIVATE
        include
        src
        lib/fmt/include
        lib/toml11
    )
    target_link_libraries(video_enhancement_settings_test PRIVATE fmt)
    add_test(NAME video_enhancement_settings_test
             COMMAND video_enhancement_settings_test)

    # Needs bios/SCPH1001.toml — run from the framework root.
    add_test(NAME bios_address_model_test
             COMMAND bios_address_model_test
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # These targets were already built by every test build, but without
    # add_test() entries ctest could neither run nor fail them.
    add_test(NAME l2_structural_test COMMAND l2_structural_test)
    add_test(NAME reachable_discovery_test COMMAND reachable_discovery_test)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME cli_project_packaging
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_project_packaging.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # These two invoke psxrecomp-game, whose BIOS-profile resolution
        # probes fixed working-directory-relative paths (bios/<name>.toml,
        # then psxrecomp/bios/<name>.toml) -- run them from the framework
        # root so the shipped profiles are found.
        add_test(
            NAME overlay_cross_page_delay_codegen
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_cross_page_delay_codegen.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME fmv_cycle_batch_codegen
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fmv_cycle_batch_codegen.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME overlay_shim_compile_contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_shim_compile.py)
        add_test(
            NAME dirty_text_continuation_guards
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_dirty_text_continuation_guards.py)
        add_test(
            NAME dirty_text_admission_guards
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_dirty_text_admission_guards.py)
        add_test(
            NAME bios_backend_constant_codegen
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bios_backend_constant_codegen.py
                    --recompiler $<TARGET_FILE:psxrecomp-bios>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_test(
            NAME stall_report
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_stall_report.py
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # ---- codegen tests that were written but never registered -----------
        # These existed in recompiler/tests/ with no add_test() entry, so they
        # had no way to run and no way to fail. Each was executed manually
        # before being wired here; all pass. They drive psxrecomp-game, so they
        # run from the framework root for BIOS-profile resolution, exactly like
        # the two above.
        foreach(_t IN ITEMS
                aot_base_recovery
                bios_resident_shard
                msvc_cps_codegen
                overlay_guard_codegen
                overlay_store_barrier_codegen
                reachable_discovery_codegen)
            add_test(
                NAME ${_t}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${_t}.py
                        --recompiler $<TARGET_FILE:psxrecomp-game>
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        endforeach()

        # test_aot_overlay_discovery.py had no add_test() entry at all. It runs
        # far enough to exercise real discovery, then fails in
        # check_real_batched_fragment_publication: that check reaches
        # MOD.compile_fragment_batch, which spawns psxrecomp-game with cwd set
        # to its own TemporaryDirectory, so BIOS-profile resolution (which
        # probes bios/SCPH1001.toml relative to the working directory) finds
        # nothing and returns "recompiler-error: no BIOS profile found". Every
        # other subprocess in the same file passes cwd=str(ROOT) explicitly, so
        # this looks like a missed cwd rather than a product defect — but it
        # needs an owner, and DISABLED keeps it counted until then.
        add_test(
            NAME aot_overlay_discovery
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_aot_overlay_discovery.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)

        # ---- runtime source-invariant guards --------------------------------
        # These live in runtime/tests/ but only READ runtime source, so they
        # need neither a generated BIOS nor a built runtime. Registering them
        # here (as dirty_text_continuation_guards already was) means they run
        # from a plain recompiler build, which is the only tree a newcomer can
        # configure before regenerating a BIOS. Each was run manually first;
        # all pass.
        #
        foreach(_t IN ITEMS
                bios_selection_guards
                capture_history
                default_renderer_guards
                mod_owned_display_controls
                fmv_quiet_guards
                fps_telemetry_opt_in
                frame_interpolation_display_rate
                hybrid_dev_any_input
                launcher_vulkan_option
                mod_controller_override
                mod_load_acceleration
                overlay_candidate_capacity
                release_zip
                overlay_decodable_fallback
                overlay_init_guard
                sdl3_main_single_include
                vk_build_default
                vk_color_self_barrier
                vk_command_buffer_batching
                vk_draw_area_batch_boundary
                vk_mailbox_policy
                vk_present_timing
                vk_present_wait_stage
                vk_staging_cache)
            add_test(
                NAME ${_t}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_${_t}.py
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
        endforeach()

        # ---- known-failing guards: DISABLED, not de-registered ---------------
        # These three were previously left out of the list above with a comment
        # explaining that they FAIL and that wiring a red check would recreate
        # the problem that took CI off PRs. That reasoning is right; the remedy
        # was not. De-registering is precisely how a test stops being counted,
        # and it is the mechanism that let 16 C/C++ tests rot in runtime/tests/.
        #
        # ctest's DISABLED property is the intended tool: the test stays
        # registered, stays visible in `ctest -N`, and is reported as Disabled
        # rather than silently absent — without turning the run red. Delete the
        # matching set_tests_properties line to re-enable one once it is fixed.
        #
        #   interpreter_perf_guards    psx_devices_mmio_sync's recompute-deadline
        #                              branch never clears g_psx_cycle_fast_limit,
        #                              and the guard still names the pre-rename
        #                              s_next_service_cycle
        #   runtime_perf_diag_guards   asserts a substring no longer present in
        #                              the runtime source
        #   overlay_pair_dedup_runtime needs its companion harness built; not a
        #                              source-only guard
        foreach(_t IN ITEMS
                interpreter_perf_guards
                runtime_perf_diag_guards
                overlay_pair_dedup_runtime)
            add_test(
                NAME ${_t}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/tests/test_${_t}.py
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
            set_tests_properties(${_t} PROPERTIES DISABLED TRUE)
        endforeach()
    endif()
endif()

# Reachable main-EXE discovery test: parser-bound validation plus exact-entry
# analysis behavior for direct, indirect, seeded, and out-of-bound targets.
add_executable(reachable_discovery_test
    tests/reachable_discovery_test.cpp
    src/function_analysis.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(reachable_discovery_test PRIVATE
    include
    lib/fmt/include
)
target_link_libraries(reachable_discovery_test PRIVATE fmt)

# ---- test-registration completeness guard -----------------------------------
# Reads both CMakeLists.txt files as text, so it catches an orphan in either
# project from whichever one is being configured. Placed before the collision
# guard, which must stay last because it needs every target declared; this one
# inspects files on disk and has no such ordering requirement.
include(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/check_test_registration.cmake)
psxrecomp_check_all_tests_registered()

# ---- output-name collision guard (must stay LAST) ---------------------------
# Fails configure if any two targets here would write the same file on a
# case-insensitive filesystem. See the module header for the shipped-bug story.
include(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/check_target_name_collisions.cmake)
psxrecomp_check_output_name_collisions()
