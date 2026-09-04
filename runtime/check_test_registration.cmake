# Guard against test files that no build system knows about.
#
# WHY THIS EXISTS
# ---------------
# Until 2026-08-11, 16 of the 41 C/C++ tests in runtime/tests/ had no add_test()
# entry anywhere. Their only build instructions were a gcc command line written
# into each file's header comment — and a comment cannot fail. So when the
# runtime API moved under them, they broke and nobody found out:
#
#   * test_netplay_snap_ring.c stopped linking when boot_state grew its
#     VRAM-incremental queries. Its documented recipe ALSO listed a .c file the
#     test already #includes, so the recipe produced duplicate symbols. Both
#     defects sat there until someone tried to run it by hand (PR #134).
#   * test_psx_cycle_event_boundaries.c called psx_cycles_resync_after_restore()
#     after that function grew a CPUState* parameter.
#   * test_spu_end_without_repeat.c never grew stubs for two externs spu.c
#     picked up.
#   * test_sio_card_protocol.c drifted until 147 of its 309 checks failed.
#
# Registering those 16 fixes the instance. It does not fix the class: the class
# is that adding an unregistered test is silent, and silence is the default.
# This check inverts that default. A new file under tests/ must either be
# registered or be listed below with a reason — there is no third option that
# configures successfully.
#
# HOW IT WORKS
# ------------
# The two projects here (recompiler/ and runtime/) are separate project()s with
# separate BUILD_TESTING gates, and runtime/ additionally needs a generated BIOS
# to configure — so there is no single tree in which CMake can see every
# registered target. Rather than force them together (which would make the
# BIOS-free recompiler tree unconfigurable for newcomers), this reads both
# CMakeLists.txt files as TEXT and compares the test files they mention against
# the test files on disk. That works identically from either project, so both
# call it and each independently catches an orphan in the other.
#
# Usage: call at the END of a CMakeLists.txt.
#   include(${CMAKE_CURRENT_SOURCE_DIR}/check_test_registration.cmake)
#   psxrecomp_check_all_tests_registered()

set(_PSXRECOMP_TESTREG_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Files under tests/ that are deliberately not registered as their own test.
# Every entry needs a reason. An entry with no reason is a silent orphan with
# extra steps, which is the thing this file exists to prevent.
set(PSXRECOMP_TESTS_NOT_REGISTERED
    "test_overlay_posix.c|built and run by tests/run_overlay_posix_test.sh, which stages the dlopen fixture tree; that script is registered as overlay_posix_test on UNIX"
)

function(psxrecomp_check_all_tests_registered)
    set(_root "${_PSXRECOMP_TESTREG_DIR}/..")

    # ---- what the build systems say -------------------------------------
    set(_declared "")
    foreach(_cml "${_root}/runtime/CMakeLists.txt"
                 "${_root}/recompiler/CMakeLists.txt")
        if(NOT EXISTS "${_cml}")
            continue()
        endif()
        file(READ "${_cml}" _text)

        # Direct references: tests/test_foo.c, ../runtime/tests/test_foo.py,
        # tests/foo_test.cpp ... Matched narrowly (test_ prefix / _test suffix)
        # so that ordinary sources on the same add_executable line cannot be
        # mistaken for a registration.
        string(REGEX MATCHALL "test_[A-Za-z0-9_]+\\.(c|cpp|py)" _hits "${_text}")
        string(REGEX MATCHALL "[A-Za-z0-9_]+_test\\.(c|cpp)" _hits2 "${_text}")
        foreach(_h IN LISTS _hits _hits2)
            string(REGEX REPLACE "\\.(c|cpp|py)$" "" _stem "${_h}")
            list(APPEND _declared "${_stem}")
        endforeach()

        # foreach(_t IN ITEMS a b c) blocks register test_<item>.py. Match only
        # up to the first ')' so the list items are captured but the add_test()
        # body inside the loop is not.
        string(REGEX MATCHALL "foreach\\(_t IN ITEMS[^)]*\\)" _loops "${_text}")
        foreach(_loop IN LISTS _loops)
            string(REGEX REPLACE "foreach\\(_t IN ITEMS" "" _loop "${_loop}")
            string(REGEX MATCHALL "[A-Za-z0-9_]+" _items "${_loop}")
            foreach(_i IN LISTS _items)
                list(APPEND _declared "test_${_i}")
            endforeach()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _declared)

    # ---- what is actually on disk ---------------------------------------
    # CONFIGURE_DEPENDS matters more than usual here: without it, ADDING a test
    # file would not re-run configure, so this check would sleep through the
    # exact event it exists to catch.
    file(GLOB _found CONFIGURE_DEPENDS
        "${_root}/runtime/tests/test_*.c"
        "${_root}/runtime/tests/test_*.cpp"
        "${_root}/runtime/tests/test_*.py"
        "${_root}/recompiler/tests/*_test.cpp"
        "${_root}/recompiler/tests/test_*.py")

    # ---- diff -------------------------------------------------------------
    set(_orphans "")
    foreach(_path IN LISTS _found)
        get_filename_component(_file "${_path}" NAME)
        get_filename_component(_stem "${_path}" NAME_WE)

        set(_excused FALSE)
        foreach(_ex IN LISTS PSXRECOMP_TESTS_NOT_REGISTERED)
            string(REGEX REPLACE "\\|.*$" "" _ex_file "${_ex}")
            if(_ex_file STREQUAL _file)
                set(_excused TRUE)
                break()
            endif()
        endforeach()
        if(_excused)
            continue()
        endif()

        list(FIND _declared "${_stem}" _idx)
        if(_idx EQUAL -1)
            file(RELATIVE_PATH _rel "${_root}" "${_path}")
            list(APPEND _orphans "${_rel}")
        endif()
    endforeach()

    if(_orphans)
        list(JOIN _orphans "\n    " _pretty)
        message(FATAL_ERROR
            "Test file(s) present on disk but registered nowhere:\n"
            "    ${_pretty}\n\n"
            "An unregistered test cannot run, cannot fail, and rots silently — "
            "four of them had already broken against the runtime API before "
            "anyone noticed (see the header of "
            "runtime/check_test_registration.cmake).\n\n"
            "Fix by EITHER:\n"
            "  * adding an add_test() entry in runtime/CMakeLists.txt or "
            "recompiler/CMakeLists.txt — register it DISABLED via "
            "set_tests_properties(<name> PROPERTIES DISABLED TRUE) if it is "
            "known-failing, so it stays visible in `ctest -N` instead of "
            "vanishing; or\n"
            "  * adding '<filename>|<reason>' to "
            "PSXRECOMP_TESTS_NOT_REGISTERED in "
            "runtime/check_test_registration.cmake, if it is a fixture or is "
            "driven by another test.")
    endif()

    list(LENGTH _found _n_found)
    message(STATUS
        "test-registration guard: ${_n_found} test file(s), all registered")
endfunction()
