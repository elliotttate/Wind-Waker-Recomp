# BlueWake iPadOS/iOS host (Route A: AOT-translated retail code + GXRuntime/Aurora).
#
# Reuses runtime/host/src unchanged; the only iOS-specific source is the entry
# shim that sets default paths inside the app container and hands control to
# the host's main() under SDL's UIKit application lifecycle.
cmake_minimum_required(VERSION 3.25)
project(bluewake_ios LANGUAGES C CXX OBJC OBJCXX)

set(BLUEWAKE_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(GXRUNTIME_DIR ${BLUEWAKE_REPO_ROOT}/ref/recompcore/GXRuntime)
include(${BLUEWAKE_REPO_ROOT}/cmake/RecompCorePatches.cmake)
set(BLUEWAKE_HOST_SRC ${BLUEWAKE_REPO_ROOT}/runtime/host/src)

# Source paths compiled into the app (assert and log messages) are written
# relative to the checkout, so a build does not carry the builder's home folder.
get_filename_component(BLUEWAKE_REPO_ROOT_ABS "${BLUEWAKE_REPO_ROOT}" ABSOLUTE)
add_compile_options("-ffile-prefix-map=${BLUEWAKE_REPO_ROOT_ABS}/=")

option(BLUEWAKE_IOS_ENABLE_DSP "Link the donor DSP adapter (required: JAS waits on the DSP at boot)" ON)
option(BLUEWAKE_IOS_DEVELOPER_TRACING "Compile BLUEWAKE_TRACE_* probes into the host" OFF)
set(BLUEWAKE_IOS_DSP_COMMON_SOURCES "" CACHE INTERNAL "")

find_package(Threads REQUIRED)
add_subdirectory(${GXRUNTIME_DIR} gxruntime_build EXCLUDE_FROM_ALL)

set(BLUEWAKE_HOST_SOURCES
    ${BLUEWAKE_HOST_SRC}/main.c
    ${BLUEWAKE_HOST_SRC}/aram_dma.c
    ${BLUEWAKE_HOST_SRC}/audio_capture.c
    ${BLUEWAKE_HOST_SRC}/ipl_sram.c
    ${BLUEWAKE_HOST_SRC}/card_runtime.c
    ${BLUEWAKE_HOST_SRC}/callback_delivery.c
    ${BLUEWAKE_HOST_SRC}/cycle_domain.c
    ${BLUEWAKE_HOST_SRC}/delivery_digest.c
    ${BLUEWAKE_HOST_SRC}/edge_intercepts.c
    ${BLUEWAKE_HOST_SRC}/game_options.c
    ${BLUEWAKE_HOST_SRC}/gamepads.c
    ${BLUEWAKE_HOST_SRC}/input_bindings.c
    ${BLUEWAKE_HOST_SRC}/pad_remap.cpp
    ${BLUEWAKE_HOST_SRC}/forest_water.c
    ${BLUEWAKE_HOST_SRC}/fast_load.c
    ${BLUEWAKE_HOST_SRC}/fps_watch.c
    ${BLUEWAKE_HOST_SRC}/jump_button.c
    ${BLUEWAKE_HOST_SRC}/climb.c
    ${BLUEWAKE_HOST_SRC}/sprint.c
    ${BLUEWAKE_HOST_SRC}/quick_doors.c
    ${BLUEWAKE_HOST_SRC}/haptics.c
    ${BLUEWAKE_HOST_SRC}/draw_tags.c
    ${BLUEWAKE_HOST_SRC}/mouse_camera.c
    ${BLUEWAKE_HOST_SRC}/return_census.c
    ${BLUEWAKE_HOST_SRC}/interrupt_sources.c
    ${BLUEWAKE_HOST_SRC}/cold_fallback.c
    ${BLUEWAKE_HOST_SRC}/external_memory.c
    ${BLUEWAKE_HOST_SRC}/fpu_context.c
    ${BLUEWAKE_HOST_SRC}/pad_event_schedule.c
    ${BLUEWAKE_HOST_SRC}/pad_waypoint.c
    ${BLUEWAKE_HOST_SRC}/pad_wire.c
    ${BLUEWAKE_HOST_SRC}/rel_scratch_allocator.c
    ${BLUEWAKE_HOST_SRC}/scheduler_contract.c
    ${BLUEWAKE_HOST_SRC}/save_state.c)

add_executable(BlueWake MACOSX_BUNDLE
    src/ios_entry.m
    src/first_run.m
    src/disc_import.c
    src/dolphin_save_import.c
    src/BWGameOverlay.mm
    src/controller_settings.mm
    src/controller_apply.cpp
    src/touch_controls.cpp
    ${BLUEWAKE_HOST_SOURCES})
# The unchanged host keeps its own entry point under another name; the shim
# owns main() so SDL can start UIApplicationMain first.
set_source_files_properties(${BLUEWAKE_HOST_SRC}/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=bluewake_host_main")
target_link_libraries(BlueWake PRIVATE gxruntime GXRuntime::aurora z)
set_target_properties(BlueWake PROPERTIES
    LINKER_LANGUAGE CXX
    OUTPUT_NAME BlueWake
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.bluewake.BlueWake
    MACOSX_BUNDLE_BUNDLE_NAME BlueWake
    MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
target_include_directories(BlueWake PRIVATE
    ${GXRUNTIME_DIR}/include
    ${BLUEWAKE_REPO_ROOT}/ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp)
target_compile_definitions(BlueWake PRIVATE
    DOLRECOMP_CPU_HEADER="core/cpu.h"
    BLUEWAKE_ENABLE_DEVELOPER_TRACING=$<BOOL:${BLUEWAKE_IOS_DEVELOPER_TRACING}>
    BLUEWAKE_EDGE_CENSUS=0
    BLUEWAKE_IOS=1)
target_compile_options(BlueWake PRIVATE -fobjc-arc)
target_link_libraries(BlueWake PRIVATE
    "-framework Foundation" "-framework UIKit" "-framework GameController"
    "-framework QuartzCore" "-framework UniformTypeIdentifiers" "-framework AVFoundation")

if(BLUEWAKE_IOS_ENABLE_DSP)
    # The donor DSP (Dolphin's DSP LLE interpreter from RecompCore) compiled
    # from source for this platform, with only the Common files it needs.
    set(_donor ${BLUEWAKE_REPO_ROOT}/ref/recompcore)
    set(_dsp ${_donor}/Source/Core/Core/DSP)
    add_library(bluewake_dsp_donor STATIC
        ${_dsp}/DSPAccelerator.cpp ${_dsp}/DSPAnalyzer.cpp ${_dsp}/DSPAssembler.cpp
        ${_dsp}/DSPCaptureLogger.cpp ${_dsp}/DSPCodeUtil.cpp ${_dsp}/DSPCore.cpp
        ${_dsp}/DSPDisassembler.cpp ${_dsp}/DSPHWInterface.cpp ${_dsp}/DSPMemoryMap.cpp
        ${_dsp}/DSPStacks.cpp ${_dsp}/DSPTables.cpp ${_dsp}/LabelMap.cpp
        ${_dsp}/Interpreter/DSPIntArithmetic.cpp ${_dsp}/Interpreter/DSPIntBranch.cpp
        ${_dsp}/Interpreter/DSPIntExtOps.cpp ${_dsp}/Interpreter/DSPIntLoadStore.cpp
        ${_dsp}/Interpreter/DSPIntMisc.cpp ${_dsp}/Interpreter/DSPIntMultiplier.cpp
        ${_dsp}/Interpreter/DSPIntTables.cpp ${_dsp}/Interpreter/DSPInterpreter.cpp
        ${_dsp}/Jit/DSPEmitterBase.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/DSPHLE.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/MailHandler.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/AESnd.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/ASnd.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/AXWii.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/CARD.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/GBA.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/INIT.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/ROM.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/UCodes.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp
        ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/ZeldaUCodesTable.cpp
        src/dsp_common_shim.cpp)
    set(_dsp_includes
        ${_donor}/Source/Core ${_donor}/Source ${_donor}/Externals
        ${_donor}/Externals/xxhash/xxHash
        ${CMAKE_CURRENT_SOURCE_DIR}/dsp_generated)
    target_include_directories(bluewake_dsp_donor PUBLIC ${_dsp_includes})
    target_compile_definitions(bluewake_dsp_donor PUBLIC
        _ARCH_64=1 _M_ARM_64=1 FMT_EXCEPTIONS=0 _DEFAULT_SOURCE
        __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS)
    target_compile_features(bluewake_dsp_donor PUBLIC cxx_std_23)
    target_compile_options(bluewake_dsp_donor PRIVATE -fno-strict-aliasing -w)
    target_link_libraries(bluewake_dsp_donor PUBLIC fmt::fmt xxhash)

    add_library(bluewake_dsp_adapter STATIC
        ${BLUEWAKE_HOST_SRC}/dsp_adapter.cpp
        ${BLUEWAKE_HOST_SRC}/dsp_adapter_c.cpp
        ${BLUEWAKE_HOST_SRC}/dsp_adapter_donor_stubs.cpp
        ${BLUEWAKE_HOST_SRC}/dsp_hle_backend.cpp)
    target_link_libraries(bluewake_dsp_adapter PUBLIC bluewake_dsp_donor
        z iconv "-framework CoreFoundation" "-framework Foundation")
    target_compile_definitions(BlueWake PRIVATE BLUEWAKE_HAS_DSP_ADAPTER=1)
    target_link_libraries(BlueWake PRIVATE bluewake_dsp_adapter)
endif()

# Temporary blue-wave icon, shared by Ninja device and simulator builds.
file(GLOB BLUEWAKE_APP_ICONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon*.png")
target_sources(BlueWake PRIVATE ${BLUEWAKE_APP_ICONS})
set_source_files_properties(${BLUEWAKE_APP_ICONS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

# Aurora's bundled pipeline cache (initial_pipeline_cache.db at the bundle
# root): the pipeline descriptions a play session builds, merged into a new
# player's cache on first launch and compiled in the background. Without it
# a first launch skipped every draw whose pipeline was still compiling, and
# the HUD was missing for the first seconds of play.
set(BLUEWAKE_PIPELINE_SEED "${CMAKE_CURRENT_SOURCE_DIR}/resources/initial_pipeline_cache.db")
target_sources(BlueWake PRIVATE ${BLUEWAKE_PIPELINE_SEED})
set_source_files_properties(${BLUEWAKE_PIPELINE_SEED} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
