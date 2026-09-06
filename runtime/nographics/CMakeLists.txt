cmake_minimum_required(VERSION 3.24)

project(psxrecomp-nographics-renderer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NOGRAPHICSAPI_ROOT "" CACHE PATH "Path to pinned NoGraphicsAPI checkout")
set(NOGRAPHICSAPI_EXPECTED_REVISION "469ebba558ee46f93c73a9f285979aa00fce7500" CACHE STRING "Expected NoGraphicsAPI Git revision")
set(VULKAN_HEADERS_ROOT "" CACHE PATH "Path to Vulkan-Headers 1.4.357+ checkout")
set(VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}" CACHE PATH "Installed Vulkan SDK path used for vulkan-1.lib")
set(NOGRAPHICSAPI_SLANGC "" CACHE FILEPATH "Path to slangc.exe")
set(NOGRAPHICSAPI_SPIRV_VAL "" CACHE FILEPATH "Path to spirv-val.exe")

if(NOT WIN32 OR MINGW)
    message(FATAL_ERROR "Build this DLL with MSVC or clang-cl; NoGraphicsAPI does not support MinGW")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NoGraphicsAPI requires a 64-bit target")
endif()
if(NOT NOGRAPHICSAPI_ROOT)
    message(FATAL_ERROR "Set -DNOGRAPHICSAPI_ROOT=...")
endif()
if(NOT VULKAN_HEADERS_ROOT)
    message(FATAL_ERROR "Set -DVULKAN_HEADERS_ROOT=...")
endif()
if(NOT NOGRAPHICSAPI_SLANGC)
    unset(NOGRAPHICSAPI_SLANGC CACHE)
    find_program(NOGRAPHICSAPI_SLANGC NAMES slangc REQUIRED)
endif()
if(NOT NOGRAPHICSAPI_SPIRV_VAL)
    unset(NOGRAPHICSAPI_SPIRV_VAL CACHE)
    find_program(NOGRAPHICSAPI_SPIRV_VAL NAMES spirv-val REQUIRED)
endif()
foreach(_ng_tool NOGRAPHICSAPI_SLANGC NOGRAPHICSAPI_SPIRV_VAL)
    if(NOT EXISTS "${${_ng_tool}}" OR IS_DIRECTORY "${${_ng_tool}}")
        message(FATAL_ERROR "${_ng_tool} must name an existing executable: ${${_ng_tool}}")
    endif()
endforeach()

find_program(GIT_EXECUTABLE
    NAMES git
    HINTS "C:/Program Files/Git/cmd"
    NO_DEFAULT_PATH)
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${NOGRAPHICSAPI_ROOT}" rev-parse HEAD
    RESULT_VARIABLE _ng_rev_result
    OUTPUT_VARIABLE _ng_rev
    ERROR_VARIABLE _ng_rev_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT _ng_rev_result EQUAL 0 OR NOT _ng_rev STREQUAL NOGRAPHICSAPI_EXPECTED_REVISION)
    message(FATAL_ERROR "NoGraphicsAPI checkout revision '${_ng_rev}' does not match '${NOGRAPHICSAPI_EXPECTED_REVISION}': ${_ng_rev_error}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${NOGRAPHICSAPI_ROOT}" diff --quiet HEAD --
    RESULT_VARIABLE _ng_dirty)
if(NOT _ng_dirty EQUAL 0)
    message(FATAL_ERROR "NoGraphicsAPI checkout has tracked changes; the revision pin requires unmodified sources")
endif()

set(NOGRAPHICSAPI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NOGRAPHICSAPI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(Vulkan_INCLUDE_DIR "${VULKAN_HEADERS_ROOT}/include" CACHE PATH "" FORCE)
find_library(Vulkan_LIBRARY NAMES vulkan-1 HINTS "${VULKAN_SDK_ROOT}/Lib" "${VULKAN_SDK_ROOT}/lib" REQUIRED)
set(Vulkan_LIBRARY "${Vulkan_LIBRARY}" CACHE FILEPATH "" FORCE)
add_subdirectory("${NOGRAPHICSAPI_ROOT}" nographicsapi)

function(psx_ng_compile_slang output source entry stage)
    get_filename_component(_out_dir "${output}" DIRECTORY)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out_dir}"
        COMMAND "${NOGRAPHICSAPI_SLANGC}" "${source}"
            -target spirv
            -profile spirv_1_5
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            -fvk-use-c-layout
            -matrix-layout-row-major
            -capability spvDescriptorHeapEXT
            -entry "${entry}"
            -stage "${stage}"
            -o "${output}"
        COMMAND "${NOGRAPHICSAPI_SPIRV_VAL}" --target-env vulkan1.4 --scalar-block-layout "${output}"
        DEPENDS "${source}"
        VERBATIM
    )
endfunction()

set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
psx_ng_compile_slang("${_generated}/psx_ng_raster.comp.spv" "${CMAKE_CURRENT_SOURCE_DIR}/psx_ng_raster.slang" computeMain compute)
psx_ng_compile_slang("${_generated}/psx_ng_present.vert.spv" "${CMAKE_CURRENT_SOURCE_DIR}/psx_ng_present.slang" vertexMain vertex)
psx_ng_compile_slang("${_generated}/psx_ng_present.frag.spv" "${CMAKE_CURRENT_SOURCE_DIR}/psx_ng_present.slang" fragmentMain fragment)

add_custom_command(
    OUTPUT "${_generated}/psx_ng_raster_spv.h"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${_generated}/psx_ng_raster.comp.spv -DOUTPUT=${_generated}/psx_ng_raster_spv.h -DSYMBOL=psx_ng_raster_spv -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS "${_generated}/psx_ng_raster.comp.spv" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    VERBATIM
)
add_custom_command(
    OUTPUT "${_generated}/psx_ng_present_vert_spv.h"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${_generated}/psx_ng_present.vert.spv -DOUTPUT=${_generated}/psx_ng_present_vert_spv.h -DSYMBOL=psx_ng_present_vert_spv -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS "${_generated}/psx_ng_present.vert.spv" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    VERBATIM
)
add_custom_command(
    OUTPUT "${_generated}/psx_ng_present_frag_spv.h"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${_generated}/psx_ng_present.frag.spv -DOUTPUT=${_generated}/psx_ng_present_frag_spv.h -DSYMBOL=psx_ng_present_frag_spv -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS "${_generated}/psx_ng_present.frag.spv" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
    VERBATIM
)

add_library(psx_nographics_runtime SHARED
    psx_ng_renderer.cpp
    psx_ng_shared.h
    psx_ng_raster.slang
    psx_ng_present.slang
    "${_generated}/psx_ng_raster_spv.h"
    "${_generated}/psx_ng_present_vert_spv.h"
    "${_generated}/psx_ng_present_frag_spv.h"
)

set_target_properties(psx_nographics_runtime PROPERTIES OUTPUT_NAME "psx_nographics")

target_include_directories(psx_nographics_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_link_libraries(psx_nographics_runtime PRIVATE NoGraphicsAPI::NoGraphicsAPI "${Vulkan_LIBRARY}")
target_compile_definitions(psx_nographics_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(psx_nographics_runtime PRIVATE /EHs-c- /GR- /W4)
else()
    target_compile_options(psx_nographics_runtime PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)
endif()
