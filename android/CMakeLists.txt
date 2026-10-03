# BlueWake for Android (arm64): the unchanged host, GXRuntime/Aurora (Dawn on
# Vulkan, SDL3) and the donor DSP, as windows/CMakeLists.txt and
# apple/ios/CMakeLists.txt build them. The only Android-specific source is the
# entry shim (src/android_entry.c), which fills in default paths in the app's
# storage and hands over to the host; SDL's Java activity (android/java) starts
# it from libmain.so.
#
# Built by scripts/android/build.py with the NDK's CMake toolchain.
cmake_minimum_required(VERSION 3.25)
project(bluewake_android LANGUAGES C CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "android/ builds BlueWake for Android only")
endif()

get_filename_component(BLUEWAKE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(GXRUNTIME_DIR ${BLUEWAKE_REPO_ROOT}/ref/recompcore/GXRuntime)
set(BLUEWAKE_HOST_SRC ${BLUEWAKE_REPO_ROOT}/runtime/host/src)
set(BLUEWAKE_ANDROID_CPU "cortex-a78" CACHE STRING "-mcpu for the app")

add_compile_options("-ffile-prefix-map=${BLUEWAKE_REPO_ROOT}/=" "-mcpu=${BLUEWAKE_ANDROID_CPU}")
# The app's own training (scripts/android/build.py --train-app):
# BLUEWAKE_APP_PROFILE_GENERATE instruments it, BLUEWAKE_APP_PROFILE_USE names
# the trained profile to compile it with (one argument, spaces and all). Set
# before GXRuntime is added, so that its graphics code is trained too.
option(BLUEWAKE_APP_PROFILE_GENERATE "instrument the app for its training" OFF)
set(BLUEWAKE_APP_PROFILE_USE "" CACHE FILEPATH "the app's trained profile")
set(_bluewake_profile_flags "")
if(BLUEWAKE_APP_PROFILE_GENERATE)
    set(_bluewake_profile_flags -fprofile-instr-generate)
elseif(BLUEWAKE_APP_PROFILE_USE)
    set(_bluewake_profile_flags "-fprofile-instr-use=${BLUEWAKE_APP_PROFILE_USE}"
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin)
endif()
if(_bluewake_profile_flags)
    add_compile_options(${_bluewake_profile_flags})
endif()
# 16 KB page alignment for Android 15+ devices with 16 KB pages.
add_link_options("LINKER:-z,max-page-size=16384")

# SDL3 and Dawn statically linked into libmain.so (Aurora's defaults off
# Windows): SDL's Java activity then loads the one library.
set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
set(AURORA_DAWN_LINKAGE static CACHE STRING "" FORCE)
# Aurora's fetched dependencies (libpng, zlib-ng, freetype, ...) default to
# shared libraries when BUILD_SHARED_LIBS is unset; the APK carries only
# libmain.so and the game module, so everything is static.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

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
    ${BLUEWAKE_HOST_SRC}/game_options.c
    ${BLUEWAKE_HOST_SRC}/fast_load.c
    ${BLUEWAKE_HOST_SRC}/jump_button.c
    ${BLUEWAKE_HOST_SRC}/climb.c
    ${BLUEWAKE_HOST_SRC}/simulation_mode.c
    ${BLUEWAKE_HOST_SRC}/sprint.c
    ${BLUEWAKE_HOST_SRC}/quick_doors.c
    ${BLUEWAKE_HOST_SRC}/fps_watch.c
    ${BLUEWAKE_HOST_SRC}/draw_tags.c
    ${BLUEWAKE_HOST_SRC}/mouse_camera.c
    ${BLUEWAKE_HOST_SRC}/settings_menu.cpp
    ${BLUEWAKE_HOST_SRC}/save_state.c)

# libmain.so: the name SDL's activity loads (android/java overrides
# getLibraries() to load only this one, with SDL3 inside).
add_library(main SHARED
    src/android_entry.c
    src/android_touch.c
    ${BLUEWAKE_HOST_SOURCES})
# The unchanged host keeps its own entry point under another name; the shim
# owns SDL_main and hands over after setting the defaults.
set_source_files_properties(${BLUEWAKE_HOST_SRC}/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=bluewake_host_main")
# Save states (save_state.c) are gzip files: the system's zlib, as on iOS.
target_link_libraries(main PRIVATE gxruntime GXRuntime::aurora android log dl m z)
if(TARGET imgui)
    target_link_libraries(main PRIVATE imgui)
endif()
set_target_properties(main PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(main PRIVATE
    ${GXRUNTIME_DIR}/include
    ${BLUEWAKE_HOST_SRC}
    ${BLUEWAKE_REPO_ROOT}/ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp)
target_compile_definitions(main PRIVATE
    DOLRECOMP_CPU_HEADER="core/cpu.h"
    BLUEWAKE_ENABLE_DEVELOPER_TRACING=0
    BLUEWAKE_EDGE_CENSUS=0
    BLUEWAKE_ANDROID=1)
# SDL's JNI entry points (SDL_main, JNI_OnLoad, which registers the Java
# natives) must be linked in from the static SDL and stay visible.
target_link_options(main PRIVATE "LINKER:-u,JNI_OnLoad"
    "LINKER:--export-dynamic-symbol=SDL_main" "LINKER:--export-dynamic-symbol=JNI_OnLoad"
    "LINKER:--export-dynamic-symbol=Java_*")
# Calls between libmain's own functions bind inside libmain. Its exported
# functions (the host's, for the game module, and Dawn's and SDL's C entry
# points) were otherwise called through the PLT even from within: 2 percent of
# the GX worker and 4 percent of the render thread on the Fold 7. Nothing
# loaded before libmain defines them, so nothing could interpose them anyway.
target_link_options(main PRIVATE "LINKER:-Bsymbolic-functions")
if(_bluewake_profile_flags)
    target_link_options(main PRIVATE ${_bluewake_profile_flags})
endif()

# The donor DSP (Dolphin's DSP interpreter and its high-level Zelda ucode, from
# RecompCore) compiled from source, as on iOS and Windows.
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
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src/dsp_common_shim.cpp)
target_include_directories(bluewake_dsp_donor PUBLIC
    ${_donor}/Source/Core ${_donor}/Source ${_donor}/Externals
    ${_donor}/Externals/xxhash/xxHash
    ${BLUEWAKE_REPO_ROOT}/apple/ios/dsp_generated)
# The DSP interpreter everywhere (DSPEmitterNull), as on iOS.
target_compile_definitions(bluewake_dsp_donor PUBLIC
    _ARCH_64=1 _M_ARM_64=1 FMT_EXCEPTIONS=0 _DEFAULT_SOURCE
    __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS)
target_compile_features(bluewake_dsp_donor PUBLIC cxx_std_23)
# Dolphin's own Android paths (content URIs through its JNI layer) are not
# part of the DSP: the donor builds as it does on Linux.
target_compile_options(bluewake_dsp_donor PRIVATE -fno-strict-aliasing -w -UANDROID)
target_link_libraries(bluewake_dsp_donor PUBLIC fmt::fmt xxhash)

add_library(bluewake_dsp_adapter STATIC
    ${BLUEWAKE_HOST_SRC}/dsp_adapter.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_c.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_donor_stubs.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_hle_backend.cpp)
target_compile_options(bluewake_dsp_adapter PRIVATE -UANDROID)
target_link_libraries(bluewake_dsp_adapter PUBLIC bluewake_dsp_donor z)
target_compile_definitions(main PRIVATE BLUEWAKE_HAS_DSP_ADAPTER=1)
target_link_libraries(main PRIVATE bluewake_dsp_adapter)
