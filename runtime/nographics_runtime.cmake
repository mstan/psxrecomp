# Optional NoGraphicsAPI runtime DLL integration.
#
# The main runtime is built with the RetComM Clang/MinGW toolchain, while
# upstream NoGraphicsAPI rejects MinGW and requires C++20. Keep that boundary at
# a C ABI DLL loaded by the runtime bridge; do not link NoGraphicsAPI objects or
# C++ headers into the runtime executable.

option(PSX_ENABLE_NOGRAPHICS
    "Enable the supplemental NoGraphicsAPI renderer bridge and stage its DLL" OFF)

set(PSX_NOGRAPHICS_DLL "" CACHE FILEPATH
    "Prebuilt NoGraphicsAPI runtime C ABI DLL to copy beside runtime targets")
set(PSX_NOGRAPHICS_PROJECT_DIR "" CACHE PATH
    "Standalone MSVC CMake project that builds the NoGraphicsAPI runtime DLL")
set(PSX_NOGRAPHICS_BUILD_DIR "" CACHE PATH
    "Parent build directory for per-runtime-target NoGraphicsAPI builds; empty = current binary directory")
set(PSX_NOGRAPHICS_TARGET "psx_nographics_runtime" CACHE STRING
    "Target name in PSX_NOGRAPHICS_PROJECT_DIR that builds the NoGraphicsAPI DLL")
set(PSX_NOGRAPHICS_DLL_NAME "psx_nographics.dll" CACHE STRING
    "Runtime DLL file name copied beside the executable and loaded by the C bridge")
set(PSX_NOGRAPHICS_CONFIG "Release" CACHE STRING
    "Configuration used when building the standalone NoGraphicsAPI DLL")
set_property(CACHE PSX_NOGRAPHICS_CONFIG PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
set(PSX_NOGRAPHICS_CMAKE "" CACHE FILEPATH
    "Native Visual Studio CMake executable used for the standalone NoGraphicsAPI DLL")
set(PSX_NOGRAPHICS_VS_GENERATOR "Visual Studio 17 2022" CACHE STRING
    "CMake generator used for the standalone NoGraphicsAPI DLL")
set(PSX_NOGRAPHICS_VS_ARCH "x64" CACHE STRING
    "CMake -A architecture used for the standalone NoGraphicsAPI DLL")
set(PSX_NOGRAPHICS_API_ROOT "" CACHE PATH
    "sebbbi/NoGraphicsAPI checkout passed to the standalone DLL project")
set(PSX_NOGRAPHICS_VULKAN_HEADERS_ROOT "" CACHE PATH
    "Vulkan-Headers 1.4.357+ checkout passed to the standalone DLL project")
set(PSX_NOGRAPHICS_VULKAN_LIBRARY "" CACHE FILEPATH
    "vulkan-1 import library passed to the standalone DLL project")
set(PSX_NOGRAPHICS_SLANGC "" CACHE FILEPATH
    "Slang compiler passed to the standalone DLL project")
set(PSX_NOGRAPHICS_SPIRV_VAL "" CACHE FILEPATH
    "spirv-val executable passed to the standalone DLL project")
set(PSX_NOGRAPHICS_CMAKE_ARGS "" CACHE STRING
    "Additional semicolon-separated configure arguments for the standalone NoGraphicsAPI DLL project")

function(psxrecomp_configure_nographics_runtime target)
    set(_ng_bridge "${PSXRECOMP_ROOT}/runtime/src/gpu_ng_renderer.c")
    if(NOT EXISTS "${_ng_bridge}")
        message(FATAL_ERROR
            "runtime/src/gpu_ng_renderer.c is required. It supplies the "
            "NoGraphicsAPI bridge and its PSX_ENABLE_NOGRAPHICS=OFF stub.")
    endif()
    target_sources(${target} PRIVATE "${_ng_bridge}")

    if(NOT PSX_ENABLE_NOGRAPHICS)
        message(STATUS
            "NoGraphicsAPI backend: disabled (PSX_ENABLE_NOGRAPHICS=OFF)")
        return()
    endif()

    if(NOT WIN32)
        message(FATAL_ERROR
            "PSX_ENABLE_NOGRAPHICS currently stages a Windows DLL; disable it "
            "or add a platform-specific loader/staging path first.")
    endif()
    if(NOT PSX_NOGRAPHICS_DLL_NAME STREQUAL "psx_nographics.dll")
        message(FATAL_ERROR
            "PSX_NOGRAPHICS_DLL_NAME must stay psx_nographics.dll because the "
            "C ABI header and runtime loader use that fixed side-by-side name.")
    endif()

    set(_ng_dll "${PSX_NOGRAPHICS_DLL}")
    set(_ng_build_target "")
    if(_ng_dll)
        if(NOT EXISTS "${_ng_dll}")
            message(FATAL_ERROR
                "PSX_NOGRAPHICS_DLL does not exist: ${_ng_dll}")
        endif()
    elseif(PSX_NOGRAPHICS_PROJECT_DIR)
        if(NOT EXISTS "${PSX_NOGRAPHICS_PROJECT_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "PSX_NOGRAPHICS_PROJECT_DIR must contain CMakeLists.txt: "
                "${PSX_NOGRAPHICS_PROJECT_DIR}")
        endif()
        if(NOT PSX_NOGRAPHICS_CMAKE)
            find_program(_ng_cmake_candidate
                NAMES cmake
                HINTS
                    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
                    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
                NO_DEFAULT_PATH)
            if(_ng_cmake_candidate)
                set(PSX_NOGRAPHICS_CMAKE "${_ng_cmake_candidate}" CACHE FILEPATH
                    "Native Visual Studio CMake executable used for the standalone NoGraphicsAPI DLL" FORCE)
            endif()
        endif()
        if(NOT PSX_NOGRAPHICS_CMAKE)
            message(FATAL_ERROR
                "PSX_ENABLE_NOGRAPHICS=ON with PSX_NOGRAPHICS_PROJECT_DIR requires "
                "native Visual Studio CMake. Set PSX_NOGRAPHICS_CMAKE.")
        endif()
        if(PSX_NOGRAPHICS_BUILD_DIR)
            # Several executables (runtime and oracle) can use this helper in
            # the same Ninja graph. Give each producer a distinct DLL path.
            set(_ng_build_dir "${PSX_NOGRAPHICS_BUILD_DIR}/${target}_nographics_runtime")
        else()
            set(_ng_build_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_nographics_runtime")
        endif()
        get_filename_component(_ng_cmake_name "${PSX_NOGRAPHICS_CMAKE}" NAME)
        if(NOT _ng_cmake_name STREQUAL "cmake.exe")
            message(FATAL_ERROR
                "PSX_NOGRAPHICS_CMAKE must point to native Visual Studio cmake.exe: "
                "${PSX_NOGRAPHICS_CMAKE}")
        endif()
        set(_ng_cmake_dir "")
        get_filename_component(_ng_cmake_dir "${PSX_NOGRAPHICS_CMAKE}" DIRECTORY)
        if(NOT _ng_cmake_dir MATCHES "Microsoft Visual Studio.*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin$")
            message(FATAL_ERROR
                "PSX_NOGRAPHICS_CMAKE must be the Visual Studio-bundled CMake, "
                "not an MSYS/MinGW shim: ${PSX_NOGRAPHICS_CMAKE}")
        endif()
        find_program(_ng_native_git
            NAMES git
            HINTS "C:/Program Files/Git/cmd"
            NO_DEFAULT_PATH)
        if(NOT _ng_native_git)
            message(FATAL_ERROR
                "PSX_ENABLE_NOGRAPHICS requires native Git at C:/Program Files/Git/cmd/git.exe")
        endif()
        get_filename_component(_ng_git_dir "${_ng_native_git}" DIRECTORY)
        set(_ng_native_path "${_ng_cmake_dir};${_ng_git_dir};C:/Windows/System32;C:/Windows")
        set(_ng_bin_dir "${_ng_build_dir}/bin")
        string(TOUPPER "${PSX_NOGRAPHICS_CONFIG}" _ng_config_upper)
        set(_ng_dll "${_ng_bin_dir}/${PSX_NOGRAPHICS_CONFIG}/${PSX_NOGRAPHICS_DLL_NAME}")
        set(_ng_config_args "")
        if(PSX_NOGRAPHICS_API_ROOT)
            list(APPEND _ng_config_args "-DNOGRAPHICSAPI_ROOT=${PSX_NOGRAPHICS_API_ROOT}")
        endif()
        if(PSX_NOGRAPHICS_VULKAN_HEADERS_ROOT)
            list(APPEND _ng_config_args "-DVULKAN_HEADERS_ROOT=${PSX_NOGRAPHICS_VULKAN_HEADERS_ROOT}")
        endif()
        if(PSX_NOGRAPHICS_VULKAN_LIBRARY)
            list(APPEND _ng_config_args "-DVulkan_LIBRARY=${PSX_NOGRAPHICS_VULKAN_LIBRARY}")
        endif()
        if(PSX_NOGRAPHICS_SLANGC)
            list(APPEND _ng_config_args "-DNOGRAPHICSAPI_SLANGC=${PSX_NOGRAPHICS_SLANGC}")
        endif()
        if(PSX_NOGRAPHICS_SPIRV_VAL)
            list(APPEND _ng_config_args "-DNOGRAPHICSAPI_SPIRV_VAL=${PSX_NOGRAPHICS_SPIRV_VAL}")
        endif()
        if(PSX_NOGRAPHICS_CMAKE_ARGS)
            list(APPEND _ng_config_args ${PSX_NOGRAPHICS_CMAKE_ARGS})
        endif()
        set(_ng_build_target "${target}_nographics_runtime_dll")
        add_custom_target(${_ng_build_target}
            COMMAND ${CMAKE_COMMAND} -E env "PATH=${_ng_native_path}"
                    "${PSX_NOGRAPHICS_CMAKE}"
                    -S "${PSX_NOGRAPHICS_PROJECT_DIR}"
                    -B "${_ng_build_dir}"
                    -G "${PSX_NOGRAPHICS_VS_GENERATOR}"
                    -A "${PSX_NOGRAPHICS_VS_ARCH}"
                    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${_ng_bin_dir}"
                    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_${_ng_config_upper}=${_ng_bin_dir}/${PSX_NOGRAPHICS_CONFIG}"
                    "-DPSX_NOGRAPHICS_DLL_NAME=${PSX_NOGRAPHICS_DLL_NAME}"
                    ${_ng_config_args}
            COMMAND ${CMAKE_COMMAND} -E env "PATH=${_ng_native_path}"
                    "${PSX_NOGRAPHICS_CMAKE}"
                    --build "${_ng_build_dir}"
                    --config "${PSX_NOGRAPHICS_CONFIG}"
                    --target "${PSX_NOGRAPHICS_TARGET}"
            BYPRODUCTS "${_ng_dll}"
            COMMENT "Building standalone NoGraphicsAPI runtime DLL"
            VERBATIM)
    else()
        message(FATAL_ERROR
            "PSX_ENABLE_NOGRAPHICS=ON requires PSX_NOGRAPHICS_DLL=<dll> or "
            "PSX_NOGRAPHICS_PROJECT_DIR=<standalone CMake project>.")
    endif()

    set(_ng_stage_target "${target}_stage_nographics_runtime_dll")
    add_custom_target(${_ng_stage_target}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ng_dll}"
            "$<TARGET_FILE_DIR:${target}>/${PSX_NOGRAPHICS_DLL_NAME}"
        DEPENDS "${_ng_dll}"
        COMMENT "Staging NoGraphicsAPI runtime DLL"
        VERBATIM)
    if(_ng_build_target)
        add_dependencies(${_ng_stage_target} ${_ng_build_target})
    endif()
    add_dependencies(${target} ${_ng_stage_target})
    target_compile_definitions(${target} PRIVATE
        PSX_HAVE_NOGRAPHICS=1
        "PSX_NOGRAPHICS_DLL_NAME=\"${PSX_NOGRAPHICS_DLL_NAME}\"")
    message(STATUS "NoGraphicsAPI backend: staging ${_ng_dll}")
endfunction()
