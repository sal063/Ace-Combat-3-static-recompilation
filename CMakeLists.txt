cmake_minimum_required(VERSION 3.20)
if(NOT CMAKE_CXX_COMPILER AND CMAKE_C_COMPILER MATCHES "clang(\\.exe)?$")
  string(REGEX REPLACE "clang(\\.exe)?$" "clang++\\1" CMAKE_CXX_COMPILER "${CMAKE_C_COMPILER}")
endif()
project(ac3 C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SDL3_ROOT "C:/SDL3" CACHE PATH "SDL3 development root")
find_package(Vulkan REQUIRED COMPONENTS glslc)

# The game's code is not in this repository. It is translated to C here, from
# the ACE3.BIN on your own disc, the first time the project is configured.
set(AC3_GAME_EXE ${CMAKE_SOURCE_DIR}/game/ACE3.BIN CACHE FILEPATH "ACE3.BIN from the game disc")
set(AC3_GAME_EXE_SHA256 2c3f2ce41268176128973ea0cd9b28784a26c0061bff5550c99599388f13ebe9)
set(AC3_RECOMP_DIR ${CMAKE_SOURCE_DIR}/recomp/main)
if(NOT EXISTS ${AC3_RECOMP_DIR}/ac3_table.c)
  if(NOT EXISTS ${AC3_GAME_EXE})
    message(FATAL_ERROR
      "${AC3_GAME_EXE} not found.\n"
      "Copy it out of your disc 1 image first:\n"
      "  python tools/ac3_disc.py extract \"<disc 1>.bin\" ACE3.BIN game/ACE3.BIN")
  endif()
  file(SHA256 ${AC3_GAME_EXE} exe_sha256)
  if(NOT exe_sha256 STREQUAL AC3_GAME_EXE_SHA256)
    message(FATAL_ERROR
      "${AC3_GAME_EXE} is not the executable this project was written against "
      "(Japanese release, Rev 1, SLPS-02020/02021).")
  endif()
  message(STATUS "Translating ${AC3_GAME_EXE} to C")
  execute_process(
    COMMAND python ${CMAKE_SOURCE_DIR}/tools/recompile.py ${AC3_GAME_EXE}
            ${CMAKE_SOURCE_DIR}/recomp/analysis.json ${AC3_RECOMP_DIR}
            --symbols ${CMAKE_SOURCE_DIR}/recomp/psyq_symbols.json
            --hle ${CMAKE_SOURCE_DIR}/runtime/hle/hle_functions.txt
    RESULT_VARIABLE recompile_result)
  if(NOT recompile_result EQUAL 0)
    message(FATAL_ERROR "tools/recompile.py failed")
  endif()
endif()

file(GLOB AC3_RUNTIME_SRC ${CMAKE_SOURCE_DIR}/runtime/*.c)
file(GLOB AC3_HLE_SRC     ${CMAKE_SOURCE_DIR}/runtime/hle/*.c)
file(GLOB AC3_MAIN_SRC    ${AC3_RECOMP_DIR}/*.c)
file(GLOB AC3_HOST_SRC    ${CMAKE_SOURCE_DIR}/host/*.c)
file(GLOB AC3_HW_SRC      ${CMAKE_SOURCE_DIR}/renderer/hw/*.c)

set(AC3_RENDER_SRC
    ${CMAKE_SOURCE_DIR}/renderer/gpu_vulkan.c
    ${CMAKE_SOURCE_DIR}/renderer/ac3_sdl_input.c
    ${CMAKE_SOURCE_DIR}/renderer/ac3_sdl_audio.c)

set(AC3_INCLUDES
    ${CMAKE_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/runtime/hle)

add_library(ac3_recomp_main STATIC ${AC3_MAIN_SRC})
target_include_directories(ac3_recomp_main PUBLIC ${AC3_INCLUDES})
target_compile_options(ac3_recomp_main PRIVATE -O1 -w)

set(SHADER_DIR ${CMAKE_SOURCE_DIR}/renderer/shaders)
set(EMBED_SPV ${CMAKE_SOURCE_DIR}/tools/embed_spv.py)

function(ac3_embed_shaders target header glslc_flags)
    set(cmds)
    set(args)
    set(deps ${EMBED_SPV} ${SHADER_DIR}/hw_fx_common.glsl ${SHADER_DIR}/hw_bloom.glsl)
    foreach(sh ${ARGN})
        string(REPLACE "." "_" sym ${sh})
        list(APPEND cmds COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${glslc_flags}
             ${SHADER_DIR}/${sh} -o ${SHADER_DIR}/${sh}.spv)
        list(APPEND args spv_${sym}=${SHADER_DIR}/${sh}.spv)
        list(APPEND deps ${SHADER_DIR}/${sh})
    endforeach()
    add_custom_command(
        OUTPUT ${header}
        ${cmds}
        COMMAND ${CMAKE_COMMAND} -E env python ${EMBED_SPV} ${header} ${args}
        DEPENDS ${deps}
        COMMENT "compiling and embedding shaders: ${target}")
    add_custom_target(${target} DEPENDS ${header})
endfunction()

ac3_embed_shaders(ac3_shaders ${CMAKE_SOURCE_DIR}/renderer/ac3_shaders.h ""
    psx.vert psx.frag)
ac3_embed_shaders(ac3_shaders_hw ${CMAKE_SOURCE_DIR}/renderer/ac3_shaders_hw.h ""
    hw_prim.vert hw_prim.frag hw_display.frag)
ac3_embed_shaders(ac3_shaders_fx ${CMAKE_SOURCE_DIR}/renderer/ac3_shaders_fx.h "-O;-I;${SHADER_DIR}"
    hw_bg.frag hw_prime.frag hw_tree.vert hw_tree.frag hw_cloud.frag hw_cloud_resolve.frag
    hw_post.frag hw_tile.vert hw_tile.frag hw_bloom_down.frag hw_bloom_up.frag
    hw_sunvis.frag hw_composite.frag hw_ocean.vert hw_ocean.frag
    ocean_spectrum.comp ocean_fft.comp ocean_finalize.comp)

function(ac3_game target)
    add_executable(${target}
        ${AC3_RUNTIME_SRC} ${AC3_HLE_SRC} ${AC3_RENDER_SRC} ${AC3_HOST_SRC} ${ARGN})
    target_include_directories(${target} PRIVATE ${AC3_INCLUDES}
        ${CMAKE_SOURCE_DIR}/renderer
        ${SDL3_ROOT}/include
        ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE ac3_recomp_main
        ${SDL3_ROOT}/lib/x64/SDL3.lib
        ${Vulkan_LIBRARIES})
    target_compile_options(${target} PRIVATE -O1 -Wall -Wextra -Wno-unused-parameter
        -Wno-deprecated-declarations)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SDL3_ROOT}/lib/x64/SDL3.dll $<TARGET_FILE_DIR:${target}>)
endfunction()

ac3_game(ac3)
add_dependencies(ac3 ac3_shaders)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(ac3_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(ac3_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends
    ${SDL3_ROOT}/include ${Vulkan_INCLUDE_DIRS})
target_compile_options(ac3_imgui PRIVATE -O2 -w)

ac3_game(ac3_enhanced ${AC3_HW_SRC} ${CMAKE_SOURCE_DIR}/renderer/hw/hw_ui.cpp)
add_dependencies(ac3_enhanced ac3_shaders ac3_shaders_hw ac3_shaders_fx)
target_compile_definitions(ac3_enhanced PRIVATE AC3_ENHANCED=1)
target_link_libraries(ac3_enhanced PRIVATE ac3_imgui)

set_source_files_properties(${CMAKE_SOURCE_DIR}/runtime/hle/gpu_sw.c
    PROPERTIES COMPILE_OPTIONS "-O2")
