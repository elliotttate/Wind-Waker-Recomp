cmake_minimum_required(VERSION 3.20)
project(bluewake_host LANGUAGES C CXX)

include(CTest)

set(GXRUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/GXRuntime)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/RecompCorePatches.cmake)

option(BLUEWAKE_ENABLE_DSP_ADAPTER
       "Build the optional source-compatible RecompCore DSP adapter" OFF)
option(BLUEWAKE_ENABLE_DSP_IPO
       "Require interprocedural optimization for the donor DSP route" ON)
option(BLUEWAKE_ENABLE_DEVELOPER_TRACING
       "Compile opt-in developer trace probes into the host" OFF)
option(BLUEWAKE_EDGE_CENSUS
       "Build the chassis edge-service decomposition census (inert when OFF)" OFF)
set(BLUEWAKE_DSP_DONOR_DIR ""
    CACHE PATH "RecompCore source checkout for the optional DSP adapter")
set(BLUEWAKE_DSP_DONOR_BUILD_DIR ""
    CACHE PATH "RecompCore CMake build directory for the optional DSP adapter")

add_subdirectory(${GXRUNTIME_DIR} gxruntime_build)

if(TARGET GXRuntime::aurora)
    set(_bluewake_interp_dir ${GXRUNTIME_DIR}/graphics/aurora)
    add_executable(bluewake_frame_interp_test
        ${_bluewake_interp_dir}/tests/frame_interp_test.cpp
        ${_bluewake_interp_dir}/lib/gfx/frame_interp.cpp)
    target_compile_features(bluewake_frame_interp_test PRIVATE cxx_std_20)
    target_include_directories(bluewake_frame_interp_test PRIVATE
        ${_bluewake_interp_dir}/include ${GXRUNTIME_DIR}/graphics/gxcore/include)
    target_link_libraries(bluewake_frame_interp_test PRIVATE absl::flat_hash_map absl::flat_hash_set)
    add_test(NAME bluewake_frame_interp_test COMMAND bluewake_frame_interp_test)
endif()

add_executable(bluewake_host
    src/main.c
    src/aram_dma.c
    src/audio_capture.c
    src/ipl_sram.c
    src/card_runtime.c
    src/callback_delivery.c
    src/cycle_domain.c
    src/delivery_digest.c
    src/edge_intercepts.c
    src/game_options.c
    src/forest_water.c
    src/fast_load.c
    src/fps_watch.c
    src/jump_button.c
    src/climb.c
    src/settings_menu.cpp
    src/simulation_mode.c
    src/sprint.c
    src/quick_doors.c
    src/haptics.c
    src/draw_tags.c
    src/mouse_camera.c
    src/return_census.c
    src/interrupt_sources.c
    src/cold_fallback.c
    src/external_memory.c
    src/fpu_context.c
    src/pad_event_schedule.c
    src/pad_waypoint.c
    src/pad_wire.c
    src/rel_scratch_allocator.c
    src/scheduler_contract.c
    src/save_state.c)
target_link_libraries(bluewake_host PRIVATE gxruntime GXRuntime::aurora z)
# The options menu (settings_menu.cpp) draws with Aurora's Dear ImGui.
if(TARGET imgui)
    target_link_libraries(bluewake_host PRIVATE imgui)
endif()
set_target_properties(bluewake_host PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(bluewake_host PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp
)
target_compile_definitions(bluewake_host PRIVATE
    DOLRECOMP_CPU_HEADER="core/cpu.h"
    BLUEWAKE_ENABLE_DEVELOPER_TRACING=$<BOOL:${BLUEWAKE_ENABLE_DEVELOPER_TRACING}>
    BLUEWAKE_EDGE_CENSUS=$<BOOL:${BLUEWAKE_EDGE_CENSUS}>)

if(APPLE)
    set(BLUEWAKE_APP_DIR "${CMAKE_CURRENT_BINARY_DIR}/BlueWake.app")
    add_custom_command(TARGET bluewake_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${BLUEWAKE_APP_DIR}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:bluewake_host>"
            "${BLUEWAKE_APP_DIR}/Contents/MacOS/BlueWake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/BlueWakeInfo.plist"
            "${BLUEWAKE_APP_DIR}/Contents/Info.plist"
        COMMAND /usr/bin/codesign --force --deep --sign -
            "${BLUEWAKE_APP_DIR}"
        COMMENT "Assembling BlueWake.app for native macOS inspection"
        VERBATIM)
endif()

add_executable(bluewake_cpu_boundary_test ../../tests/cpu_boundary_test.c)
target_link_libraries(bluewake_cpu_boundary_test PRIVATE gxruntime)
target_include_directories(bluewake_cpu_boundary_test PRIVATE
    ${GXRUNTIME_DIR}/include
)

add_executable(bluewake_scheduler_contract_test
    ../../tests/scheduler_contract_test.c
    src/scheduler_contract.c)
target_link_libraries(bluewake_scheduler_contract_test PRIVATE gxruntime)
target_include_directories(bluewake_scheduler_contract_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_scheduler_contract_test
    COMMAND bluewake_scheduler_contract_test)

add_executable(bluewake_cycle_domain_test
    ../../tests/cycle_domain_test.c
    src/cycle_domain.c)
target_link_libraries(bluewake_cycle_domain_test PRIVATE gxruntime)
target_include_directories(bluewake_cycle_domain_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_cycle_domain_test COMMAND bluewake_cycle_domain_test)

add_executable(bluewake_actor_search_budget_test ../../tests/actor_search_budget_test.c)
target_include_directories(bluewake_actor_search_budget_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME bluewake_actor_search_budget_test COMMAND bluewake_actor_search_budget_test)

add_executable(bluewake_interrupt_sources_test
    ../../tests/interrupt_sources_test.c
    src/cycle_domain.c
    src/interrupt_sources.c)
target_link_libraries(bluewake_interrupt_sources_test PRIVATE gxruntime)
target_include_directories(bluewake_interrupt_sources_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_interrupt_sources_test
    COMMAND bluewake_interrupt_sources_test)

add_executable(bluewake_delivery_digest_test
    ../../tests/delivery_digest_test.c
    src/delivery_digest.c)
target_link_libraries(bluewake_delivery_digest_test PRIVATE gxruntime)
target_include_directories(bluewake_delivery_digest_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_delivery_digest_test
    COMMAND bluewake_delivery_digest_test)

add_executable(bluewake_return_census_test
    ../../tests/return_census_test.c
    src/return_census.c)
target_include_directories(bluewake_return_census_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp
)
add_test(NAME bluewake_return_census_test
    COMMAND bluewake_return_census_test)

add_executable(bluewake_composite_dispatch_loop_test
    ../../tests/composite_dispatch_loop_test.c
    ../../cmake/composite/dispatch_loop.c)
target_link_libraries(bluewake_composite_dispatch_loop_test PRIVATE gxruntime)
target_include_directories(bluewake_composite_dispatch_loop_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/composite
)
add_test(NAME bluewake_composite_dispatch_loop_test
    COMMAND bluewake_composite_dispatch_loop_test)

add_executable(bluewake_simulation_timing_test
    ../../tests/simulation_timing_test.c
    ../../cmake/composite/simulation_timing.c)
target_link_libraries(bluewake_simulation_timing_test PRIVATE gxruntime)
target_include_directories(bluewake_simulation_timing_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/composite)
add_test(NAME bluewake_simulation_timing_test COMMAND bluewake_simulation_timing_test)

add_executable(bluewake_inline_fp_test ../../tests/inline_fp_test.c)
target_link_libraries(bluewake_inline_fp_test PRIVATE gxruntime)
target_include_directories(bluewake_inline_fp_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/composite)
add_test(NAME bluewake_inline_fp_test COMMAND bluewake_inline_fp_test)

add_executable(bluewake_native_math_test
    ../../tests/native_math_test.c
    ../../cmake/composite/native_math.c
    ../../cmake/composite/native_work_pool.c)
target_link_libraries(bluewake_native_math_test PRIVATE gxruntime ${CMAKE_DL_LIBS})
target_compile_options(bluewake_native_math_test PRIVATE -ffp-contract=off)
target_include_directories(bluewake_native_math_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/composite
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp)
add_test(NAME bluewake_native_math_test COMMAND bluewake_native_math_test)

add_executable(bluewake_fpu_context_test
    ../../tests/fpu_context_test.c
    src/fpu_context.c)
target_link_libraries(bluewake_fpu_context_test PRIVATE gxruntime)
target_include_directories(bluewake_fpu_context_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_fpu_context_test COMMAND bluewake_fpu_context_test)

add_executable(bluewake_cold_fallback_test
    ../../tests/cold_fallback_test.c
    src/cold_fallback.c)
target_link_libraries(bluewake_cold_fallback_test PRIVATE gxruntime)
target_include_directories(bluewake_cold_fallback_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_cold_fallback_test COMMAND bluewake_cold_fallback_test)

add_executable(bluewake_aram_dma_contract_test
    ../../tests/aram_dma_contract_test.c
    src/aram_dma.c)
target_link_libraries(bluewake_aram_dma_contract_test PRIVATE gxruntime)
target_include_directories(bluewake_aram_dma_contract_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_aram_dma_contract_test
    COMMAND bluewake_aram_dma_contract_test)

add_executable(bluewake_audio_capture_test
    ../../tests/audio_capture_test.c
    src/audio_capture.c)
target_include_directories(bluewake_audio_capture_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME bluewake_audio_capture_test COMMAND bluewake_audio_capture_test)

add_executable(bluewake_ipl_sram_test
    ../../tests/ipl_sram_test.c
    src/ipl_sram.c)
target_include_directories(bluewake_ipl_sram_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME bluewake_ipl_sram_test COMMAND bluewake_ipl_sram_test)

add_executable(bluewake_card_runtime_test
    ../../tests/card_runtime_test.c
    src/card_runtime.c)
target_link_libraries(bluewake_card_runtime_test PRIVATE gxruntime)
target_include_directories(bluewake_card_runtime_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_card_runtime_test COMMAND bluewake_card_runtime_test)

add_executable(bluewake_edge_intercepts_test
    ../../tests/edge_intercepts_test.c
    src/edge_intercepts.c
    src/game_options.c
    src/card_runtime.c)
target_link_libraries(bluewake_edge_intercepts_test PRIVATE gxruntime)
target_include_directories(bluewake_edge_intercepts_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_edge_intercepts_test
    COMMAND bluewake_edge_intercepts_test)

add_executable(bluewake_forest_water_test
    ../../tests/forest_water_test.c
    src/forest_water.c)
target_link_libraries(bluewake_forest_water_test PRIVATE gxruntime ${CMAKE_DL_LIBS})
target_include_directories(bluewake_forest_water_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp)
add_test(NAME bluewake_forest_water_test COMMAND bluewake_forest_water_test)

add_executable(bluewake_external_memory_test
    ../../tests/external_memory_test.c
    src/external_memory.c)
target_link_libraries(bluewake_external_memory_test PRIVATE gxruntime)
target_include_directories(bluewake_external_memory_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_external_memory_test
    COMMAND bluewake_external_memory_test)

add_executable(bluewake_pad_event_schedule_test
    ../../tests/pad_event_schedule_test.c
    src/pad_event_schedule.c)
target_link_libraries(bluewake_pad_event_schedule_test PRIVATE gxruntime)
target_include_directories(bluewake_pad_event_schedule_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_pad_event_schedule_test
    COMMAND bluewake_pad_event_schedule_test)

add_executable(bluewake_pad_waypoint_test
    ../../tests/pad_waypoint_test.c
    src/pad_waypoint.c)
target_link_libraries(bluewake_pad_waypoint_test PRIVATE gxruntime)
target_include_directories(bluewake_pad_waypoint_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_pad_waypoint_test COMMAND bluewake_pad_waypoint_test)

add_executable(bluewake_pad_wire_test
    ../../tests/pad_wire_test.c
    src/pad_wire.c)
target_link_libraries(bluewake_pad_wire_test PRIVATE gxruntime)
target_include_directories(bluewake_pad_wire_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME bluewake_pad_wire_test COMMAND bluewake_pad_wire_test)

add_executable(bluewake_rel_scratch_allocator_test
    ../../tests/rel_scratch_allocator_test.c
    src/rel_scratch_allocator.c)
target_include_directories(bluewake_rel_scratch_allocator_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME bluewake_rel_scratch_allocator_test
    COMMAND bluewake_rel_scratch_allocator_test)

add_executable(bluewake_staging_budget_test
    ../../tests/staging_budget_test.cpp)
target_compile_features(bluewake_staging_budget_test PRIVATE cxx_std_20)
target_include_directories(bluewake_staging_budget_test PRIVATE
    ${GXRUNTIME_DIR}/graphics/aurora/lib)
add_test(NAME bluewake_staging_budget_test
    COMMAND bluewake_staging_budget_test)

# Manual GPU oracle: exercises gxcore texture decode, TEV alpha/compare,
# Aurora blending, and framebuffer readback without private game data.
add_executable(bluewake_gxcore_alpha_pixels_test
    ../../tests/gxcore_alpha_pixels_test.cpp)
target_compile_features(bluewake_gxcore_alpha_pixels_test PRIVATE cxx_std_20)
target_include_directories(bluewake_gxcore_alpha_pixels_test PRIVATE
    ${GXRUNTIME_DIR}/graphics/aurora/lib)
target_link_libraries(bluewake_gxcore_alpha_pixels_test PRIVATE
    GXRuntime::aurora GXRuntime::gxcore)

# Manual GPU oracle: proves that a TEV indirect stage displaces the regular
# texture coordinate through the actual gxcore/Aurora pixel path.
add_executable(bluewake_gxcore_indirect_pixels_test
    ../../tests/gxcore_indirect_pixels_test.cpp)
target_compile_features(bluewake_gxcore_indirect_pixels_test PRIVATE cxx_std_20)
target_include_directories(bluewake_gxcore_indirect_pixels_test PRIVATE
    ${GXRUNTIME_DIR}/graphics/aurora/lib)
target_link_libraries(bluewake_gxcore_indirect_pixels_test PRIVATE
    GXRuntime::aurora GXRuntime::gxcore)

# Manual GPU oracle: proves GX destination-alpha override feeds a later
# destination-alpha blend through the actual gxcore/Aurora pixel path.
add_executable(bluewake_gxcore_dstalpha_pixels_test
    ../../tests/gxcore_dstalpha_pixels_test.cpp)
target_compile_features(bluewake_gxcore_dstalpha_pixels_test PRIVATE cxx_std_20)
target_include_directories(bluewake_gxcore_dstalpha_pixels_test PRIVATE
    ${GXRUNTIME_DIR}/graphics/aurora/lib)
target_link_libraries(bluewake_gxcore_dstalpha_pixels_test PRIVATE
    GXRuntime::aurora GXRuntime::gxcore)

# Manual GPU oracle: proves GXSetZCompLoc writes depth before a failing alpha
# test, so a same-depth draw using a strict comparison remains occluded.
add_executable(bluewake_gxcore_early_depth_pixels_test
    ../../tests/gxcore_early_depth_pixels_test.cpp)
target_compile_features(bluewake_gxcore_early_depth_pixels_test PRIVATE cxx_std_20)
target_include_directories(bluewake_gxcore_early_depth_pixels_test PRIVATE
    ${GXRUNTIME_DIR}/graphics/aurora/lib)
target_link_libraries(bluewake_gxcore_early_depth_pixels_test PRIVATE
    GXRuntime::aurora GXRuntime::gxcore)

# Manual GPU oracle: proves Wind Waker's late ZTexture REPLACE clear writes
# sampled 24-bit depth, allowing a later GX_LESS draw past the far clear value.
add_executable(bluewake_gxcore_ztexture_pixels_test
    ../../tests/gxcore_ztexture_pixels_test.cpp)
target_compile_features(bluewake_gxcore_ztexture_pixels_test PRIVATE cxx_std_20)
target_include_directories(bluewake_gxcore_ztexture_pixels_test PRIVATE
    ${GXRUNTIME_DIR}/graphics/aurora/lib)
target_link_libraries(bluewake_gxcore_ztexture_pixels_test PRIVATE
    GXRuntime::aurora GXRuntime::gxcore)

# Manual GPU oracle: proves a GX half-scale EFB copy samples the complete
# source rectangle and scales it into the smaller destination texture.
add_executable(bluewake_gxcore_efb_half_scale_pixels_test
    ../../tests/gxcore_efb_half_scale_pixels_test.cpp)
target_compile_features(bluewake_gxcore_efb_half_scale_pixels_test PRIVATE cxx_std_20)
target_include_directories(bluewake_gxcore_efb_half_scale_pixels_test PRIVATE
    ${GXRUNTIME_DIR}/graphics/aurora/lib)
target_link_libraries(bluewake_gxcore_efb_half_scale_pixels_test PRIVATE
    GXRuntime::aurora GXRuntime::gxcore)

add_executable(bluewake_generated_scheduler_contract_test
    ../../tests/generated_scheduler_contract_test.c)
target_link_libraries(bluewake_generated_scheduler_contract_test PRIVATE
    gxruntime ${CMAKE_DL_LIBS})
target_include_directories(bluewake_generated_scheduler_contract_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp
)
set(BLUEWAKE_COMPOSITE_MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../build/composite-lib/gGZLE01_recomp.dylib)
if(EXISTS ${BLUEWAKE_COMPOSITE_MODULE})
    add_test(NAME bluewake_generated_scheduler_contract_test
        COMMAND bluewake_generated_scheduler_contract_test
            ${BLUEWAKE_COMPOSITE_MODULE})
endif()

add_executable(bluewake_generated_aram_stream_contract_test
    ../../tests/generated_aram_stream_contract_test.c)
target_link_libraries(bluewake_generated_aram_stream_contract_test PRIVATE
    gxruntime ${CMAKE_DL_LIBS})
target_include_directories(bluewake_generated_aram_stream_contract_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp
)
if(EXISTS ${BLUEWAKE_COMPOSITE_MODULE})
    add_test(NAME bluewake_generated_aram_stream_contract_test
        COMMAND bluewake_generated_aram_stream_contract_test
            ${BLUEWAKE_COMPOSITE_MODULE})
endif()

add_executable(bluewake_generated_dvd_callback_contract_test
    ../../tests/generated_dvd_callback_contract_test.c
    src/callback_delivery.c)
target_link_libraries(bluewake_generated_dvd_callback_contract_test PRIVATE
    gxruntime ${CMAKE_DL_LIBS})
target_include_directories(bluewake_generated_dvd_callback_contract_test PRIVATE
    ${GXRUNTIME_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp
)
if(EXISTS ${BLUEWAKE_COMPOSITE_MODULE})
    add_test(NAME bluewake_generated_dvd_callback_contract_test
        COMMAND bluewake_generated_dvd_callback_contract_test
            ${BLUEWAKE_COMPOSITE_MODULE})
endif()

add_executable(bluewake_headless_pad_test ../../tests/headless_pad_test.c)
target_link_libraries(bluewake_headless_pad_test PRIVATE gxruntime)
target_include_directories(bluewake_headless_pad_test PRIVATE
    ${GXRUNTIME_DIR}/include
)

if(BLUEWAKE_ENABLE_DSP_ADAPTER)
    if(NOT EXISTS "${BLUEWAKE_DSP_DONOR_DIR}/Source/Core/Core/DSP/DSPCore.h")
        message(FATAL_ERROR "BLUEWAKE_DSP_DONOR_DIR must point to RecompCore")
    endif()
    if(NOT EXISTS "${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Source/Core/Core/libcore.a")
        message(FATAL_ERROR "BLUEWAKE_DSP_DONOR_BUILD_DIR must contain libcore.a")
    endif()

    add_library(bluewake_dsp_adapter STATIC
        src/dsp_adapter.cpp
        src/dsp_adapter_c.cpp
        src/dsp_adapter_donor_stubs.cpp
        src/dsp_hle_backend.cpp)
    target_compile_features(bluewake_dsp_adapter PUBLIC cxx_std_23)
    target_include_directories(bluewake_dsp_adapter PRIVATE
        ${BLUEWAKE_DSP_DONOR_DIR}/Source/Core
        ${BLUEWAKE_DSP_DONOR_DIR}/Source
        ${BLUEWAKE_DSP_DONOR_DIR}/Externals
        ${BLUEWAKE_DSP_DONOR_DIR}/Externals/xxhash/xxHash
        /opt/homebrew/include
    )
    find_library(BLUEWAKE_DSP_FMT_LIBRARY NAMES fmt
        PATHS /opt/homebrew/opt/fmt/lib /opt/local/lib)
    if(NOT BLUEWAKE_DSP_FMT_LIBRARY)
        message(FATAL_ERROR "fmt is required for the optional DSP adapter")
    endif()
    file(GLOB BLUEWAKE_DSP_OBJECTS
        "${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Source/Core/Core/CMakeFiles/core.dir/DSP/*.o"
        "${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Source/Core/Core/CMakeFiles/core.dir/DSP/Interpreter/*.o"
        "${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Source/Core/Core/CMakeFiles/core.dir/DSP/Jit/DSPEmitterBase.cpp.o"
        "${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Source/Core/Core/CMakeFiles/core.dir/HW/DSPHLE/*.o"
        "${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Source/Core/Core/CMakeFiles/core.dir/HW/DSPHLE/UCodes/*.o")
    list(LENGTH BLUEWAKE_DSP_OBJECTS BLUEWAKE_DSP_OBJECT_COUNT)
    if(BLUEWAKE_DSP_OBJECT_COUNT LESS 20)
        message(FATAL_ERROR "The donor DSP object set is incomplete")
    endif()
    set(BLUEWAKE_DSP_ARCHIVE
        "${CMAKE_CURRENT_BINARY_DIR}/libbluewake-dsp-donor.a")
    add_custom_command(OUTPUT "${BLUEWAKE_DSP_ARCHIVE}"
        COMMAND /usr/bin/libtool -static -o "${BLUEWAKE_DSP_ARCHIVE}"
            ${BLUEWAKE_DSP_OBJECTS}
        DEPENDS ${BLUEWAKE_DSP_OBJECTS}
        VERBATIM)
    add_custom_target(bluewake_dsp_donor_archive
        DEPENDS "${BLUEWAKE_DSP_ARCHIVE}")
    add_library(bluewake_dsp_donor STATIC IMPORTED GLOBAL)
    set_target_properties(bluewake_dsp_donor PROPERTIES
        IMPORTED_LOCATION "${BLUEWAKE_DSP_ARCHIVE}")
    add_dependencies(bluewake_dsp_adapter bluewake_dsp_donor_archive)
    target_link_libraries(bluewake_dsp_adapter PRIVATE
        bluewake_dsp_donor
        ${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Source/Core/Common/libcommon.a
        ${BLUEWAKE_DSP_DONOR_BUILD_DIR}/Externals/xxhash/libxxhash.a
        ${BLUEWAKE_DSP_FMT_LIBRARY}
        z
        iconv
        "-framework CoreFoundation"
        "-framework Foundation"
    )

    add_executable(bluewake_dsp_adapter_test src/dsp_adapter_test.cpp)
    target_link_libraries(bluewake_dsp_adapter_test PRIVATE bluewake_dsp_adapter)
    target_include_directories(bluewake_dsp_adapter_test PRIVATE
        ${BLUEWAKE_DSP_DONOR_DIR}/Source/Core
        ${BLUEWAKE_DSP_DONOR_DIR}/Source)
    target_compile_options(bluewake_dsp_adapter_test PRIVATE -UNDEBUG)
    add_test(NAME bluewake_dsp_adapter_test COMMAND bluewake_dsp_adapter_test
        ${BLUEWAKE_DSP_DONOR_DIR}/Data/Sys/GC/dsp_rom.bin
        ${BLUEWAKE_DSP_DONOR_DIR}/Data/Sys/GC/dsp_coef.bin)

    add_executable(bluewake_dsp_adapter_c_test src/dsp_adapter_c_test.c)
    target_link_libraries(bluewake_dsp_adapter_c_test PRIVATE bluewake_dsp_adapter)
    target_compile_options(bluewake_dsp_adapter_c_test PRIVATE -UNDEBUG)
    add_test(NAME bluewake_dsp_adapter_c_test COMMAND bluewake_dsp_adapter_c_test
        ${BLUEWAKE_DSP_DONOR_DIR}/Data/Sys/GC/dsp_rom.bin
        ${BLUEWAKE_DSP_DONOR_DIR}/Data/Sys/GC/dsp_coef.bin)

    target_compile_definitions(bluewake_host PRIVATE BLUEWAKE_HAS_DSP_ADAPTER=1)
    target_link_libraries(bluewake_host PRIVATE bluewake_dsp_adapter)

    if(BLUEWAKE_ENABLE_DSP_IPO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT BLUEWAKE_DSP_IPO_SUPPORTED
            OUTPUT BLUEWAKE_DSP_IPO_ERROR LANGUAGES C CXX)
        if(NOT BLUEWAKE_DSP_IPO_SUPPORTED)
            message(FATAL_ERROR
                "DSP IPO is required but unsupported: ${BLUEWAKE_DSP_IPO_ERROR}")
        endif()

        file(STRINGS "${BLUEWAKE_DSP_DONOR_BUILD_DIR}/CMakeCache.txt"
            BLUEWAKE_DSP_DONOR_IPO_CACHE
            REGEX "^CMAKE_INTERPROCEDURAL_OPTIMIZATION(:[^=]+)?=ON$")
        if(NOT BLUEWAKE_DSP_DONOR_IPO_CACHE)
            message(FATAL_ERROR
                "BLUEWAKE_DSP_DONOR_BUILD_DIR must be configured with "
                "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON")
        endif()

        set_property(TARGET
            bluewake_dsp_adapter
            bluewake_dsp_adapter_test
            bluewake_dsp_adapter_c_test
            bluewake_host
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()
