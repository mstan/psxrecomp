cmake_minimum_required(VERSION 3.20)
project(psx_renderer_probe C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH "psxrecomp checkout root")
set(PSX_RENDERER_PROBE_VULKAN_SOURCE "${PSXRECOMP_ROOT}/runtime/src/gpu_vk_renderer.c" CACHE FILEPATH "Vulkan renderer source for A/B harness runs")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3)
find_package(OpenGL REQUIRED)

if(DEFINED ENV{VULKAN_SDK})
  set(_vulkan_sdk "$ENV{VULKAN_SDK}")
endif()
find_program(GLSLC_EXE NAMES glslc HINTS "${_vulkan_sdk}/Bin" REQUIRED)

file(GLOB PSX_RENDERER_PROBE_SHADERS CONFIGURE_DEPENDS
  "${PSXRECOMP_ROOT}/runtime/shaders/*.vert"
  "${PSXRECOMP_ROOT}/runtime/shaders/*.frag"
  "${PSXRECOMP_ROOT}/runtime/shaders/*.comp"
)
set(PSX_RENDERER_PROBE_VKGEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/psx-runtime_vkgen")
set(PSX_RENDERER_PROBE_VK_HEADER "${PSX_RENDERER_PROBE_VKGEN_DIR}/vk_shaders_spv.h")
add_custom_command(
  OUTPUT "${PSX_RENDERER_PROBE_VK_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PSX_RENDERER_PROBE_VKGEN_DIR}"
  COMMAND "${Python3_EXECUTABLE}" "${PSXRECOMP_ROOT}/tools/embed_spirv.py"
          --glslc "${GLSLC_EXE}"
          --out "${PSX_RENDERER_PROBE_VK_HEADER}"
          ${PSX_RENDERER_PROBE_SHADERS}
  DEPENDS "${PSXRECOMP_ROOT}/tools/embed_spirv.py" ${PSX_RENDERER_PROBE_SHADERS}
  VERBATIM
)
add_custom_target(psx_renderer_probe_vkgen DEPENDS "${PSX_RENDERER_PROBE_VK_HEADER}")

set(PSX_RENDERER_PROBE_SOURCES
  main.c
  stubs.c
  "${PSXRECOMP_ROOT}/runtime/src/gpu_render.c"
  "${PSXRECOMP_ROOT}/runtime/src/gpu_sw_renderer.c"
  "${PSXRECOMP_ROOT}/runtime/src/gpu_gl_renderer.c"
  "${PSX_RENDERER_PROBE_VULKAN_SOURCE}"
  "${PSXRECOMP_ROOT}/runtime/src/gpu_vram_dirty.c"
)

add_executable(renderer_probe ${PSX_RENDERER_PROBE_SOURCES} "${PSX_RENDERER_PROBE_VK_HEADER}")
add_dependencies(renderer_probe psx_renderer_probe_vkgen)

target_include_directories(renderer_probe PRIVATE
  "${PSXRECOMP_ROOT}/runtime/include"
  "${PSXRECOMP_ROOT}/runtime/src"
  "${PSX_RENDERER_PROBE_VKGEN_DIR}"
  "$ENV{VULKAN_SDK}/Include"
)
target_compile_definitions(renderer_probe PRIVATE
  SDL_MAIN_HANDLED=1
  PSX_SDL3=1
  PSX_HAVE_VULKAN=1
  PSX_NO_DEBUG_TOOLS=1
  PSX_RENDERER_PROBE=1
)

if(TARGET SDL3::SDL3)
  target_link_libraries(renderer_probe PRIVATE SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
  target_link_libraries(renderer_probe PRIVATE SDL3::SDL3-static)
endif()
target_link_libraries(renderer_probe PRIVATE OpenGL::GL)
if(WIN32)
  target_link_libraries(renderer_probe PRIVATE ws2_32 user32 gdi32 shell32 ole32 uuid)
endif()


if(MINGW)
  target_link_options(renderer_probe PRIVATE -Wl,--stack,16777216)
endif()
