cmake_minimum_required(VERSION 3.24)

project(psxrecomp-nographics-probe CXX)
include(CTest)

set(NOGRAPHICSAPI_ROOT "" CACHE PATH "Path to sebbbi/NoGraphicsAPI checkout")
set(NOGRAPHICSAPI_EXPECTED_REVISION "469ebba558ee46f93c73a9f285979aa00fce7500" CACHE STRING "Expected NoGraphicsAPI Git revision")
set(VULKAN_HEADERS_ROOT "" CACHE PATH "Path to Vulkan-Headers 1.4.357+ checkout")
set(VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}" CACHE PATH "Installed Vulkan SDK path used only for vulkan-1.lib")

if(NOT NOGRAPHICSAPI_ROOT)
    message(FATAL_ERROR "Set -DNOGRAPHICSAPI_ROOT=/path/to/NoGraphicsAPI")
endif()
if(NOT VULKAN_HEADERS_ROOT)
    message(FATAL_ERROR "Set -DVULKAN_HEADERS_ROOT=/path/to/Vulkan-Headers-1.4.357")
endif()
if(MINGW)
    message(FATAL_ERROR "NoGraphicsAPI does not support MinGW; configure this probe with MSVC or clang-cl")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NoGraphicsAPI requires a 64-bit target")
endif()

find_package(Git)
if(Git_FOUND AND NOGRAPHICSAPI_EXPECTED_REVISION)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${NOGRAPHICSAPI_ROOT}" rev-parse HEAD
        RESULT_VARIABLE _nographics_rev_parse_result
        OUTPUT_VARIABLE _nographics_head
        ERROR_VARIABLE _nographics_rev_parse_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _nographics_rev_parse_result EQUAL 0)
        message(FATAL_ERROR
            "Git failed to read NoGraphicsAPI revision from "
            "'${NOGRAPHICSAPI_ROOT}': ${_nographics_rev_parse_error}. Set "
            "-DNOGRAPHICSAPI_EXPECTED_REVISION= to allow an unverified checkout.")
    endif()
    if(NOT _nographics_head STREQUAL NOGRAPHICSAPI_EXPECTED_REVISION)
        message(FATAL_ERROR
            "NoGraphicsAPI checkout is '${_nographics_head}', expected "
            "'${NOGRAPHICSAPI_EXPECTED_REVISION}'. Set "
            "-DNOGRAPHICSAPI_EXPECTED_REVISION= to allow another revision.")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${NOGRAPHICSAPI_ROOT}" diff --quiet HEAD --
        RESULT_VARIABLE _nographics_dirty
    )
    if(NOT _nographics_dirty EQUAL 0)
        message(FATAL_ERROR
            "NoGraphicsAPI checkout has local changes; revision pin only proves "
            "HEAD. Set -DNOGRAPHICSAPI_EXPECTED_REVISION= to allow a dirty tree.")
    endif()
elseif(NOGRAPHICSAPI_EXPECTED_REVISION)
    message(FATAL_ERROR
        "Git is required to verify NOGRAPHICSAPI_EXPECTED_REVISION. Set "
        "-DNOGRAPHICSAPI_EXPECTED_REVISION= to allow an unverified checkout.")
endif()

set(NOGRAPHICSAPI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NOGRAPHICSAPI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(Vulkan_INCLUDE_DIR "${VULKAN_HEADERS_ROOT}/include" CACHE PATH "" FORCE)
find_library(Vulkan_LIBRARY
    NAMES vulkan-1
    HINTS "${VULKAN_SDK_ROOT}/Lib" "${VULKAN_SDK_ROOT}/lib"
    REQUIRED
)
set(Vulkan_LIBRARY "${Vulkan_LIBRARY}" CACHE FILEPATH "" FORCE)

add_subdirectory("${NOGRAPHICSAPI_ROOT}" nographicsapi-upstream)

add_executable(nographics_create_device_probe
    nographics_create_device_probe.cpp
)
target_link_libraries(nographics_create_device_probe PRIVATE NoGraphicsAPI::NoGraphicsAPI)
target_link_libraries(nographics_create_device_probe PRIVATE "${Vulkan_LIBRARY}")

if(BUILD_TESTING)
    add_test(NAME nographics_create_device_probe COMMAND nographics_create_device_probe)
    set_tests_properties(nographics_create_device_probe PROPERTIES SKIP_RETURN_CODE 77)
endif()
