include_guard(GLOBAL)

option(PSX_RUNTIME_IPO
    "Experimental IPO/LTO for optimized runtime targets (requires supported C/C++ toolchain)"
    OFF)

function(psxrecomp_apply_runtime_ipo target)
    # OFF must not alter a parent project's explicit IPO policy or add flags.
    if(NOT PSX_RUNTIME_IPO)
        return()
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _psx_ipo_ok OUTPUT _psx_ipo_error LANGUAGES C CXX)
    if(NOT _psx_ipo_ok)
        message(FATAL_ERROR
            "PSX_RUNTIME_IPO=ON was requested, but C/C++ IPO is unsupported.\n"
            "Use a compatible compiler/linker or configure PSX_RUNTIME_IPO=OFF.\n"
            "${_psx_ipo_error}")
    endif()
    # Target/configuration-scoped: do not silently enable IPO in third-party
    # libraries, build helpers, Debug, or custom configurations.
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_DEBUG FALSE)
    foreach(_psx_ipo_config RELEASE RELWITHDEBINFO MINSIZEREL)
        set_property(TARGET ${target}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_psx_ipo_config} TRUE)
    endforeach()
    message(STATUS "psxrecomp: experimental runtime IPO enabled for ${target} (optimized configurations)")
endfunction()
