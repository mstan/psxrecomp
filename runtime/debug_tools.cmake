include_guard(GLOBAL)

# AUTO keeps multi-config build trees honest: production configurations strip
# diagnostics while Debug and RelWithDebInfo retain them. Explicit ON/OFF
# remains a tree-wide override, matching the historical cache option.
set(PSX_DEBUG_TOOLS "AUTO" CACHE STRING
    "Debug tools policy (AUTO, ON, or OFF)")
set_property(CACHE PSX_DEBUG_TOOLS PROPERTY STRINGS AUTO ON OFF)

string(TOUPPER "${PSX_DEBUG_TOOLS}" _psx_debug_tools_mode)
if(NOT _psx_debug_tools_mode STREQUAL "AUTO" AND
   NOT _psx_debug_tools_mode STREQUAL "ON" AND
   NOT _psx_debug_tools_mode STREQUAL "OFF")
    message(FATAL_ERROR
        "PSX_DEBUG_TOOLS must be AUTO, ON, or OFF "
        "(got '${PSX_DEBUG_TOOLS}')")
endif()

function(psxrecomp_apply_debug_tools target)
    # Re-resolve in the caller's scope so this remains correct if the module's
    # globally guarded function is used from more than one directory.
    string(TOUPPER "${PSX_DEBUG_TOOLS}" _target_debug_tools_mode)
    if(_target_debug_tools_mode STREQUAL "AUTO")
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Release,MinSizeRel>:PSX_NO_DEBUG_TOOLS=1>)
    elseif(_target_debug_tools_mode STREQUAL "OFF")
        target_compile_definitions(${target} PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()
endfunction()
