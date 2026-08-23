cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)
include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- Native/oracle runtime targets ----

set(PSXRECOMP_ROOT "${CMAKE_SOURCE_DIR}/..")
include("${CMAKE_SOURCE_DIR}/runtime.cmake")

psxrecomp_add_runtime_target(psx-runtime
    DEBUG_PORT 4370
    WINDOW_TITLE "psxrecomp - BIOS"
    DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    EXE_NAME "PSXRecomp"
)

# With -DPSX_PGXP_VARIANT=ON, psxrecomp_add_runtime_target auto-clones a
# psx-runtime-pgxp sibling (PSXRecomp_pgxp.exe) compiled with -DPSX_PGXP=1 —
# the framework-dev validation surface for the PGXP value-propagation engine
# (ENHANCEMENTS.md G1.10). Titles get their <exe>_pgxp the same way.

if(BUILD_TESTING)
    add_executable(launcher_device_roundtrip_test
        tests/test_launcher_device_roundtrip.cpp)
    target_include_directories(launcher_device_roundtrip_test PRIVATE include)
    add_test(NAME launcher_device_roundtrip_test
             COMMAND launcher_device_roundtrip_test)

    # COP0.CAUSE IP2 must mirror the INTC line combinationally, never latch.
    add_executable(cause_ip2_combinational_test
        tests/test_cause_ip2_combinational.c)
    add_test(NAME cause_ip2_combinational_test
             COMMAND cause_ip2_combinational_test)

    add_executable(psx_cyc_batch_test tests/test_psx_cyc_batch.c)
    target_include_directories(psx_cyc_batch_test PRIVATE include)
    target_compile_definitions(psx_cyc_batch_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME psx_cyc_batch_test COMMAND psx_cyc_batch_test)

    add_executable(psx_icache_fastpath_test
        tests/test_psx_icache_fastpath.c
        src/psx_icache.c
    )
    target_include_directories(psx_icache_fastpath_test PRIVATE include)
    # PSX_OVERLAY_DLL_BUILD is the seam psx_cycles.h already provides for a TU
    # that supplies its own psx_advance_cycles (overlay DLLs forward it through
    # a callback shim). Without it the header inlines the production definition
    # and this test's counting stub is a redefinition — the whole runtime test
    # suite stopped building at "redefinition of 'psx_advance_cycles'", which
    # also blocked every test registered after it.
    target_compile_definitions(psx_icache_fastpath_test PRIVATE
        PSX_ENABLE_BLOCK_CYCLES=1
        PSX_OVERLAY_DLL_BUILD=1)
    add_test(NAME psx_icache_fastpath_test COMMAND psx_icache_fastpath_test)

    add_executable(host_keymap_test
        tests/test_host_keymap.c
        src/host_keymap.c)
    target_include_directories(host_keymap_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(host_keymap_test PRIVATE
        SDL_MAIN_HANDLED
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(host_keymap_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME host_keymap_test COMMAND host_keymap_test)

    # Registered since before this change, but it did NOT LINK on origin/master:
    # sio.c had grown psx_read_word / psx_get_cycle_count / i_mask /
    # g_debug_last_store_pc references that this test's stub block never gained,
    # so `ctest` in a runtime tree could not even build. The stubs are added now
    # (a registered test that cannot compile is worse than an unregistered one —
    # it breaks the whole suite's build), which exposes a real disagreement:
    # 27 checks fail, starting with enter.id expected=0x73 (DualShock analog ID)
    # actual=0xFF (no device). Constant-clock and monotonic-clock stubs give
    # byte-identical results, so the cycle stub is not the discriminator — this
    # is the SIO/peripheral axis, not the harness. DISABLED until owned.
    add_executable(sio_dualshock_rumble_test
        tests/test_sio_dualshock_rumble.c
        src/sio.c)
    target_include_directories(sio_dualshock_rumble_test PRIVATE include)
    add_test(NAME sio_dualshock_rumble_test COMMAND sio_dualshock_rumble_test)
    set_tests_properties(sio_dualshock_rumble_test PROPERTIES DISABLED TRUE)

    add_executable(gte_register_access_test
        tests/test_gte_register_access.cpp
        src/gte.cpp
        src/pgxp.cpp
    )
    target_include_directories(gte_register_access_test PRIVATE include)
    target_compile_definitions(gte_register_access_test PRIVATE
        PSX_NO_DEBUG_TOOLS=1
        PSX_GTE_REGISTER_TEST=1
        PSX_ENABLE_BLOCK_CYCLES=1)
    add_test(NAME gte_register_access_test COMMAND gte_register_access_test)

    # PGXP value-propagation engine (ENHANCEMENTS.md G1.2/G1.3): shadow
    # load/store/COP2 roundtrips, half-word semantics, validate-on-read
    # (foreign overwrite + DMA-overwrite rejection), MOVE/repack arithmetic,
    # suppression bracket, invalidate-all, and GetPreciseVertex safeguards
    # (truncation agreement + tolerance clamp). White-box: includes src/pgxp.cpp
    # and stubs the gte.cpp fallback-cache extern.
    add_executable(pgxp_test tests/test_pgxp.cpp src/pgxp.cpp)
    target_include_directories(pgxp_test PRIVATE include)
    add_test(NAME pgxp_test COMMAND pgxp_test)

    add_executable(psx_instr_cost_test tests/test_psx_instr_cost.cpp)
    target_include_directories(psx_instr_cost_test PRIVATE include)
    add_test(NAME psx_instr_cost_test COMMAND psx_instr_cost_test)

    add_executable(ws_aspect_cone_math_test
        tests/test_ws_aspect_cone_math.c
        src/ws_aspect_cone_math.c)
    target_include_directories(ws_aspect_cone_math_test PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(ws_aspect_cone_math_test PRIVATE m)
    endif()
    add_test(NAME ws_aspect_cone_math_test COMMAND ws_aspect_cone_math_test)

    add_executable(ws_ui_group_test
        tests/test_ws_ui_group.c
        src/ws_ui_group.c)
    target_include_directories(ws_ui_group_test PRIVATE include)
    add_test(NAME ws_ui_group_test COMMAND ws_ui_group_test)

    add_executable(mod_gpu_dma_aperture_test
        tests/test_mod_gpu_dma_aperture.c)
    target_include_directories(mod_gpu_dma_aperture_test PRIVATE include)
    add_test(NAME mod_gpu_dma_aperture_test
             COMMAND mod_gpu_dma_aperture_test)

    # Clean-room SPU DSP fidelity (issue #103): SPU IRQ on every RAM access
    # class, reverb (write gating + work-area wrap + CD-only path), noise
    # generator, volume sweeps / direct decode, capture buffers, and
    # savestate coverage of all the new state. Includes src/spu.c directly
    # (white-box) and stubs its externs.
    # src/crc32.c is linked rather than stubbed: spu.c's capture/shadow paths
    # feed real CRCs that this test checks, so a zero-returning stub would make
    # those assertions vacuous. (This target did not link at all before —
    # crc32_update was undefined on origin/master too.)
    add_executable(spu_fidelity_test tests/test_spu_fidelity.c src/crc32.c)
    target_include_directories(spu_fidelity_test PRIVATE include)
    add_test(NAME spu_fidelity_test COMMAND spu_fidelity_test)
    # Same story as sio_dualshock_rumble_test: registered but unlinkable on
    # origin/master (crc32_update undefined), so it never ran. With crc32 linked
    # it now runs 137 checks and fails 4, all in one ADSR sweep region
    # (mid > 0, late > mid, fall_mid < 0x7FFE, fall_late < fall_mid). Rebuilding
    # it against a zero-returning crc32 stub reproduces exactly those 4, so the
    # crc32 linkage is not the cause — the envelope sweep genuinely disagrees.
    set_tests_properties(spu_fidelity_test PROPERTIES DISABLED TRUE)

    add_executable(overlay_capture_retry_test
        tests/test_overlay_capture_retry.cpp
        src/overlay_capture.c)
    target_include_directories(overlay_capture_retry_test PRIVATE
        include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(overlay_capture_retry_test PRIVATE
        SDL_MAIN_HANDLED PSX_OVERLAY_CAPTURE_TEST=1
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>)
    target_link_libraries(overlay_capture_retry_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME overlay_capture_retry_test COMMAND overlay_capture_retry_test)

    add_executable(overlay_path_canon_test
        tests/test_overlay_path_canon.c
        src/overlay_path_canon.c)
    target_include_directories(overlay_path_canon_test PRIVATE include)
    add_test(NAME overlay_path_canon_test COMMAND overlay_path_canon_test)

    # The player-facing "skip the BIOS" flag must resolve to the same boot
    # behaviour on retail SCPH-1001 and on the bundled OpenBIOS, whose
    # kernel-call HLE is (correctly) refused for want of a DeliverEvent anchor.
    add_executable(bios_hle_plan_test
        tests/test_bios_hle_plan.c
        src/bios_hle_plan.c)
    target_include_directories(bios_hle_plan_test PRIVATE include)
    add_test(NAME bios_hle_plan_test COMMAND bios_hle_plan_test)

    # A cue, conventional raw image, or Steam .car image is a valid pick; each
    # must mount the same disc and identify off the same data track.
    add_executable(disc_path_resolve_test
        tests/test_disc_path_resolve.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp)
    target_include_directories(disc_path_resolve_test PRIVATE include)
    target_link_libraries(disc_path_resolve_test PRIVATE chdr-static)
    add_test(NAME disc_path_resolve_test COMMAND disc_path_resolve_test)

    # Both BIOS filename conventions (bare vs region-qualified) resolve, in
    # both directions, so a config and a dump folder may disagree.
    add_executable(bios_rom_alias_test tests/test_bios_rom_alias.cpp)
    target_include_directories(bios_rom_alias_test PRIVATE
        ${PSXRECOMP_ROOT}/recompiler/include)
    add_test(NAME bios_rom_alias_test COMMAND bios_rom_alias_test)

    # Multi-file cue TOC mapping (data track + CD-DA in separate bins).
    add_executable(iso_reader_cdda_test
        tests/test_iso_reader_cdda.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_cdda_test PRIVATE include)
    target_link_libraries(iso_reader_cdda_test PRIVATE chdr-static)
    add_test(NAME iso_reader_cdda_test COMMAND iso_reader_cdda_test)

    add_executable(iso_reader_sbi_test
        tests/test_iso_reader_sbi.cpp
        src/iso_reader.cpp
        src/cue_sheet.cpp)
    target_include_directories(iso_reader_sbi_test PRIVATE include)
    target_link_libraries(iso_reader_sbi_test PRIVATE chdr-static)
    add_test(NAME iso_reader_sbi_test COMMAND iso_reader_sbi_test)

    add_executable(mod_packages_test
        tests/test_mod_packages.cpp
        src/mod_packages.cpp
        src/crc32.c
        src/psx_sha256.c)
    target_include_directories(mod_packages_test PRIVATE
        include
        ../recompiler/lib/toml11)
    add_test(NAME mod_packages_test COMMAND mod_packages_test)

    add_executable(mod_runtime_test
        tests/test_mod_runtime.cpp
        src/mod_runtime.cpp
        src/mod_packages.cpp
        src/disc_path.cpp
        src/cue_sheet.cpp
        src/iso_reader.cpp
        src/crc32.c
        src/psx_sha256.c)
    target_include_directories(mod_runtime_test PRIVATE
        include
        ../recompiler/lib/toml11)
    target_link_libraries(mod_runtime_test PRIVATE chdr-static)
    add_test(NAME mod_runtime_test COMMAND mod_runtime_test)

    if(WIN32)
        add_executable(autocompile_publication_test
            tests/test_autocompile_publication.c
            src/autocompile.c)
        target_include_directories(autocompile_publication_test PRIVATE include)
        target_compile_definitions(autocompile_publication_test PRIVATE
            PSX_AUTOCOMPILE_TEST=1)
        add_test(NAME autocompile_publication_test
                 COMMAND autocompile_publication_test)

        # A misconfigured overlay_autocompile_cmd must be reported on a channel
        # that exists: the stdout warnings reach nobody under -mwindows, so a
        # broken autocompile silently degrades the whole run to the interpreter.
        add_executable(autocompile_degraded_test
            tests/test_autocompile_degraded.c
            src/autocompile.c)
        target_include_directories(autocompile_degraded_test PRIVATE include)
        add_test(NAME autocompile_degraded_test
                 COMMAND autocompile_degraded_test)
    endif()

    # ---- previously-unregistered standalone tests -------------------------
    # These 16 existed in runtime/tests/ with no add_test() entry. Their only
    # build instructions were a gcc command line in each file's header comment,
    # which nothing executes — so when the runtime API moved under them they
    # broke silently and stayed broken. Four had already rotted when they were
    # collected here (2026-08-11):
    #
    #   test_netplay_snap_ring.c        duplicate TU in the documented command;
    #                                   no stubs for the VRAM-incremental
    #                                   boot_state queries  (PR #134)
    #   test_psx_cycle_event_boundaries.c
    #                                   psx_cycles_resync_after_restore() grew a
    #                                   CPUState* parameter; psx_cycles.c grew
    #                                   three externs the stub block never did
    #   test_spu_end_without_repeat.c   spu.c grew psx_irq_raise / crc32_update
    #   test_sio_card_protocol.c        builds, but 147 of 309 checks fail —
    #                                   registered DISABLED below, see beads
    #
    # Registering them is only half the fix; check_test_registration.cmake at
    # the end of this file is what stops the next one from going unnoticed.
    # Each test's build recipe now lives HERE and nowhere else — the header
    # comments were reduced to a `ctest -R <name>` pointer so there is exactly
    # one description of how a test is built, and it is one the build executes.

    add_executable(cdrom_irq_mask_test tests/test_cdrom_irq_mask.c)
    target_include_directories(cdrom_irq_mask_test PRIVATE src)
    add_test(NAME cdrom_irq_mask_test COMMAND cdrom_irq_mask_test)

    # Mirrors func_00005A08 (BIOS card READ chain handler) against a mock CPU,
    # so it pins the disassembly reading without linking the recompiled BIOS.
    add_executable(chain_handler_check_test tests/test_chain_handler_check.c)
    target_include_directories(chain_handler_check_test PRIVATE include src)
    add_test(NAME chain_handler_check_test COMMAND chain_handler_check_test)

    # Bug B: the pacing decision must take ONE counter sample, so the unsigned
    # subtraction cannot underflow into a ~24.7-day SDL_Delay.
    add_executable(frame_pacing_test tests/test_frame_pacing.c)
    target_include_directories(frame_pacing_test PRIVATE include)
    add_test(NAME frame_pacing_test COMMAND frame_pacing_test)

    add_executable(gpu_primitive_reject_test tests/test_gpu_primitive_reject.c)
    target_include_directories(gpu_primitive_reject_test PRIVATE include)
    add_test(NAME gpu_primitive_reject_test COMMAND gpu_primitive_reject_test)

    add_executable(gpu_sw_edges_test tests/test_gpu_sw_edges.c)
    target_include_directories(gpu_sw_edges_test PRIVATE src)
    add_test(NAME gpu_sw_edges_test COMMAND gpu_sw_edges_test)

    add_executable(gpu_vk_upload_alignment_test
        tests/test_gpu_vk_upload_alignment.c)
    target_include_directories(gpu_vk_upload_alignment_test PRIVATE src)
    add_test(NAME gpu_vk_upload_alignment_test
             COMMAND gpu_vk_upload_alignment_test)

    # Portable rollback helpers (retcomm-rbengine) — required for MotK netplay
    # unit tests that use the netplay_* compatibility shims.
    set(_RBE_ROOT "${PSXRECOMP_ROOT}/lib/retcomm-rbengine")
    set(_RNET_ROOT "${PSXRECOMP_ROOT}/lib/recomp-net")
    if(EXISTS "${_RBE_ROOT}/CMakeLists.txt" AND EXISTS "${_RNET_ROOT}/CMakeLists.txt")
        if(NOT TARGET recomp_net)
            set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(RNET_ENABLE_ICE OFF CACHE BOOL "" FORCE)
            add_subdirectory("${_RNET_ROOT}" "${CMAKE_BINARY_DIR}/recomp-net-tests")
        endif()
        if(NOT TARGET retcomm_rbengine)
            set(RBE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(RECOMP_NET_ROOT "${_RNET_ROOT}" CACHE PATH "" FORCE)
            add_subdirectory("${_RBE_ROOT}" "${CMAKE_BINARY_DIR}/retcomm-rbengine-tests")
        endif()
        set(PSXRECOMP_TEST_HAS_RBENGINE TRUE)
    else()
        set(PSXRECOMP_TEST_HAS_RBENGINE FALSE)
        message(STATUS
            "netplay_*_tests: lib/retcomm-rbengine or lib/recomp-net absent - skipped")
    endif()

    if(PSXRECOMP_TEST_HAS_RBENGINE)
        add_executable(netplay_hash_confirm_test
            tests/test_netplay_hash_confirm.c)
        target_include_directories(netplay_hash_confirm_test PRIVATE include)
        target_link_libraries(netplay_hash_confirm_test PRIVATE retcomm_rbengine)
        add_test(NAME netplay_hash_confirm_test COMMAND netplay_hash_confirm_test)

        # save/load glue #included from the test; ring ops come from rbengine.
        add_executable(netplay_snap_ring_test tests/test_netplay_snap_ring.c)
        target_include_directories(netplay_snap_ring_test PRIVATE include)
        target_compile_definitions(netplay_snap_ring_test PRIVATE
            NETPLAY_SNAP_RING_TEST_STUB_BOOT_STATE
            PSX_HAS_RECOMP_NET=1)
        target_link_libraries(netplay_snap_ring_test PRIVATE retcomm_rbengine)
        add_test(NAME netplay_snap_ring_test COMMAND netplay_snap_ring_test)

        add_executable(rb_post_tip_filter_test tests/test_rb_post_tip_filter.c)
        target_include_directories(rb_post_tip_filter_test PRIVATE include)
        target_link_libraries(rb_post_tip_filter_test PRIVATE retcomm_rbengine)
        add_test(NAME rb_post_tip_filter_test COMMAND rb_post_tip_filter_test)

        add_executable(netplay_input_hist_test
            tests/test_netplay_input_hist.c
            src/netplay_input_hist.c)
        target_include_directories(netplay_input_hist_test PRIVATE
            include
            ${_RNET_ROOT}/include)
        target_compile_definitions(netplay_input_hist_test PRIVATE
            PSX_HAS_RECOMP_NET=1)
        target_link_libraries(netplay_input_hist_test PRIVATE retcomm_rbengine)
        add_test(NAME netplay_input_hist_test COMMAND netplay_input_hist_test)
    endif()

    add_executable(overlay_store_cycle_barrier_test
        tests/test_overlay_store_cycle_barrier.c)
    target_include_directories(overlay_store_cycle_barrier_test PRIVATE include)
    add_test(NAME overlay_store_cycle_barrier_test
             COMMAND overlay_store_cycle_barrier_test)

    add_executable(psx_cycle_event_boundaries_test
        tests/test_psx_cycle_event_boundaries.c
        src/psx_cycles.c)
    target_include_directories(psx_cycle_event_boundaries_test PRIVATE include)
    add_test(NAME psx_cycle_event_boundaries_test
             COMMAND psx_cycle_event_boundaries_test)

    add_executable(spu_end_without_repeat_test
        tests/test_spu_end_without_repeat.c)
    target_include_directories(spu_end_without_repeat_test PRIVATE include)
    add_test(NAME spu_end_without_repeat_test
             COMMAND spu_end_without_repeat_test)

    add_executable(spu_pitch_zero_test
        tests/test_spu_pitch_zero.c)
    target_include_directories(spu_pitch_zero_test PRIVATE include)
    add_test(NAME spu_pitch_zero_test
             COMMAND spu_pitch_zero_test)

    add_executable(spu_gaussian_test tests/test_spu_gaussian.c)
    target_include_directories(spu_gaussian_test PRIVATE include)
    add_test(NAME spu_gaussian_test COMMAND spu_gaussian_test)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME rewind_toggle_combo_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rewind_toggle_combo.py)
        add_test(NAME host_refresh_sanitize_test
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_refresh_sanitize.py)
    endif()

    # DISABLED, not de-registered. It links and runs, but 147 of its 309 checks
    # fail: the no-card path is clean (141/141) while the card-present READ
    # reply stream comes back shifted by one and then stuck (idx[1] 0xFF for an
    # expected 0x08 FLAG, then 0x08 for every subsequent byte). That is either a
    # real sio.c protocol regression or a stale expectation in the test, and it
    # needs an owner either way.
    #
    # De-registering a failing test is exactly the mechanism that let these 16
    # rot in the first place, and wiring it RED would recreate the red-check-
    # nobody-trusts problem that took CI off PRs. DISABLED keeps it built, keeps
    # it listed in `ctest -N`, and keeps it countable, without failing the run.
    # Re-enable by deleting the set_tests_properties line once triaged.
    add_executable(sio_card_protocol_test
        tests/test_sio_card_protocol.c
        src/sio.c
        src/memcard.c)
    target_include_directories(sio_card_protocol_test PRIVATE include src)
    # Runs in a build-tree scratch dir, NOT in tests/: the test writes
    # test_dir/card1.mcd relative to the working directory (self-provisioning it
    # via mkdir if absent), and tests/test_dir/card1.mcd is a TRACKED fixture —
    # running it from tests/ overwrites that file and leaves the repo dirty.
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sio_card_protocol_scratch)
    add_test(NAME sio_card_protocol_test COMMAND sio_card_protocol_test
             WORKING_DIRECTORY
                 ${CMAKE_CURRENT_BINARY_DIR}/sio_card_protocol_scratch)
    set_tests_properties(sio_card_protocol_test PROPERTIES DISABLED TRUE)

    # SIO1 serial link (accuracy/axis4_sio1_serial.md): the device core is a
    # pure instance with injected clock + IRQ callback, so these link only
    # sio1.c + psx_link.c -- no runtime stub wall (contrast sio_card_protocol).
    add_executable(sio1_registers_test
        tests/test_sio1_registers.c
        src/sio1.c
        src/psx_link.c)
    target_include_directories(sio1_registers_test PRIVATE include)
    add_test(NAME sio1_registers_test COMMAND sio1_registers_test)

    add_executable(sio1_link_loopback_test
        tests/test_sio1_link_loopback.c
        src/sio1.c
        src/psx_link.c)
    target_include_directories(sio1_link_loopback_test PRIVATE include)
    add_test(NAME sio1_link_loopback_test COMMAND sio1_link_loopback_test)

    add_executable(sio1_snapshot_test
        tests/test_sio1_snapshot.c
        src/sio1.c
        src/psx_link.c)
    target_include_directories(sio1_snapshot_test PRIVATE include)
    add_test(NAME sio1_snapshot_test COMMAND sio1_snapshot_test)

    # POSIX-only: dlopen()s a fixture .so out of a synthetic cache tree, so it
    # cannot link under MinGW/MSVC (-ldl). run_overlay_posix_test.sh already
    # stages the cache tree, builds the fixture and the test, and runs it — so
    # register THAT rather than reimplementing its fixture staging here.
    if(UNIX)
        find_program(BASH_EXECUTABLE bash)
        if(BASH_EXECUTABLE)
            add_test(NAME overlay_posix_test
                     COMMAND ${BASH_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_overlay_posix_test.sh)
        else()
            message(STATUS "overlay_posix_test: bash not found - skipped")
        endif()
    endif()
endif()

# ---- test-registration completeness guard (must stay LAST) ------------------
# Fails configure if a file in runtime/tests/ or recompiler/tests/ is neither
# registered as a test nor listed as a deliberate exception. See the module
# header for the rot story this exists to prevent.
include(${CMAKE_CURRENT_SOURCE_DIR}/check_test_registration.cmake)
psxrecomp_check_all_tests_registered()

if(NOT MSVC)
    psxrecomp_add_runtime_target(psx-oracle
        ORACLE
        DEBUG_PORT 4371
        WINDOW_TITLE "psxrecomp - Oracle"
        DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    )
endif()

# ---- psx-beetle target (Beetle PSX libretro standalone, MinGW) ----
#
# Standalone binary that runs Beetle PSX (mednafen-psx libretro core) with
# a debug server exposing the SAME wire-protocol commands as psx-runtime,
# on a different port (4380). No recomp code linked.
#
# Requires beetle-psx built via:
#   cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0

set(BEETLE_LIB "${CMAKE_SOURCE_DIR}/../beetle-psx/libmednafen_psx.a")

if(NOT MSVC AND EXISTS "${BEETLE_LIB}")
    message(STATUS "psx-beetle: found ${BEETLE_LIB}")

    set(BEETLE_DIR "${CMAKE_SOURCE_DIR}/../beetle-psx")
    set(LRC "${BEETLE_DIR}/libretro-common")
    set(LIBRETRO_COMMON_SOURCES
        ${LRC}/streams/file_stream.c
        ${LRC}/streams/file_stream_transforms.c
        ${LRC}/vfs/vfs_implementation.c
        ${LRC}/file/file_path.c
        ${LRC}/compat/compat_strl.c
        ${LRC}/compat/fopen_utf8.c
        ${LRC}/compat/compat_posix_string.c
        ${LRC}/string/stdstring.c
        ${LRC}/lists/string_list.c
        ${LRC}/encodings/encoding_utf.c
        ${LRC}/time/rtime.c
        ${LRC}/rthreads/rthreads.c
        ${LRC}/memmap/memalign.c
        ${LRC}/hash/rhash.c
    )

    add_executable(psx-beetle
        src/beetle_main.cpp
        src/beetle_libretro.cpp
        src/beetle_debug_server.c
        src/frame_pacing.c
        src/host_time.c
        src/parity_trace.c
        src/device_trace.c
        src/audio_trace.c
        ${LIBRETRO_COMMON_SOURCES}
    )

    target_include_directories(psx-beetle PRIVATE
        include
        ${PSX_SDL_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/../beetle-psx/libretro-common/include
        ${CMAKE_SOURCE_DIR}/../beetle-psx
    )

    target_compile_definitions(psx-beetle PRIVATE
        PSX_BEETLE_BUILD=1
        DEFAULT_DEBUG_PORT=4380
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>
    )
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(psx-beetle PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    target_link_libraries(psx-beetle PRIVATE
        "${BEETLE_LIB}"
        ${PSX_SDL_LIBRARIES}
    )

    if(WIN32 OR MINGW)
        target_link_libraries(psx-beetle PRIVATE ws2_32 -lpthread)
    endif()

    if(MINGW)
        target_link_options(psx-beetle PRIVATE -Wl,--stack,67108864)
    endif()
elseif(NOT MSVC)
    message(STATUS "psx-beetle: libmednafen_psx.a not found - skipping psx-beetle")
endif()
