# BlueWake for Windows (x86-64): the unchanged host, GXRuntime/Aurora (Dawn on
# Direct3D 12 or Vulkan, SDL3) and the donor DSP, as apple/ios/CMakeLists.txt
# builds them for iOS. The only Windows-specific sources are the entry shim,
# which fills in default paths beside the executable, and the POSIX
# compatibility layer in compat/.
#
# Built by scripts/windows/build.py with clang (GNU driver, MSVC ABI); see
# docs/WINDOWS.md.
cmake_minimum_required(VERSION 3.25)
project(bluewake_windows LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "windows/ builds BlueWake for Windows only")
endif()

get_filename_component(BLUEWAKE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(GXRUNTIME_DIR ${BLUEWAKE_REPO_ROOT}/ref/recompcore/GXRuntime)
set(BLUEWAKE_HOST_SRC ${BLUEWAKE_REPO_ROOT}/runtime/host/src)
set(BLUEWAKE_COMPAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/compat)

option(BLUEWAKE_WINDOWS_CONSOLE "Build a console-subsystem executable (logs on the terminal)" OFF)
option(BLUEWAKE_WINDOWS_DEVELOPER_TRACING "Compile BLUEWAKE_TRACE_* probes into the host" OFF)

# Source paths compiled into the app (assert and log messages) are written
# relative to the checkout, so a build does not carry the builder's home folder.
add_compile_options("-ffile-prefix-map=${BLUEWAKE_REPO_ROOT}/=")
add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _USE_MATH_DEFINES)

add_subdirectory(${GXRUNTIME_DIR} gxruntime_build EXCLUDE_FROM_ALL)

# POSIX calls the host makes (threads, clocks, dlopen, dirent): force-included
# into BlueWake's own sources only, never into third-party code.
add_library(bluewake_posix_compat STATIC ${BLUEWAKE_COMPAT_DIR}/bw_posix_compat.c)
target_include_directories(bluewake_posix_compat PRIVATE ${BLUEWAKE_COMPAT_DIR})
set(BLUEWAKE_COMPAT_INCLUDES ${BLUEWAKE_COMPAT_DIR}/include ${BLUEWAKE_COMPAT_DIR})
set(BLUEWAKE_COMPAT_FORCE_INCLUDE "SHELL:-include ${BLUEWAKE_COMPAT_DIR}/bw_posix_compat.h")
# GXRuntime's own C runtime (the disc reader uses fseeko and off_t) needs the
# same few declarations, and remembers its getenv answers (the Windows C
# runtime's getenv is slow on hot paths); its C++ graphics code builds
# unchanged.
target_compile_options(gxruntime PRIVATE ${BLUEWAKE_COMPAT_FORCE_INCLUDE}
    "SHELL:-include ${BLUEWAKE_COMPAT_DIR}/bw_getenv_cache.h")

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
    ${BLUEWAKE_HOST_SRC}/haptics.c
    ${BLUEWAKE_HOST_SRC}/fps_watch.c
    ${BLUEWAKE_HOST_SRC}/draw_tags.c
    ${BLUEWAKE_HOST_SRC}/mouse_camera.c
    ${BLUEWAKE_HOST_SRC}/save_state.c)

# The manifest (DPI awareness, long paths, UTF-8 code page) is merged into the
# one the linker embeds.
add_executable(BlueWake
    src/win_entry.c
    src/win_disc.c
    src/win_settings.cpp
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src/disc_import.c
    src/bluewake.rc
    resources/BlueWake.manifest
    ${BLUEWAKE_HOST_SOURCES})
# The unchanged host keeps its own entry point under another name; the shim
# owns main() and hands over after setting the defaults.
set_source_files_properties(${BLUEWAKE_HOST_SRC}/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=bluewake_host_main")
# win_disc.c: the first launch's disc check and preparation (the iOS app's
# importer), its progress window and the file picker. Save states
# (save_state.c) are gzip files: Aurora's zlib (zlib-ng in its zlib-compatible
# build, zlib1.dll beside the executable).
target_link_libraries(BlueWake PRIVATE gxruntime GXRuntime::aurora bluewake_posix_compat winmm
    bcrypt comctl32 comdlg32 ZLIB::ZLIB)
set_target_properties(BlueWake PROPERTIES LINKER_LANGUAGE CXX OUTPUT_NAME BlueWake)
target_include_directories(BlueWake PRIVATE
    ${BLUEWAKE_COMPAT_INCLUDES}
    ${GXRUNTIME_DIR}/include
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src
    ${BLUEWAKE_REPO_ROOT}/ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp)
target_compile_options(BlueWake PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:${BLUEWAKE_COMPAT_FORCE_INCLUDE}>)
target_compile_definitions(BlueWake PRIVATE
    DOLRECOMP_CPU_HEADER="core/cpu.h"
    BLUEWAKE_ENABLE_DEVELOPER_TRACING=$<BOOL:${BLUEWAKE_WINDOWS_DEVELOPER_TRACING}>
    BLUEWAKE_EDGE_CENSUS=0
    BLUEWAKE_WINDOWS=1)
# The game's translated code recurses on the host stack as the guest does on
# the GameCube's; macOS gives the main thread 8 MB, Windows only 1 MB.
target_link_options(BlueWake PRIVATE "LINKER:/STACK:67108864")
if(BLUEWAKE_WINDOWS_CONSOLE)
    target_compile_definitions(BlueWake PRIVATE BLUEWAKE_WINDOWS_CONSOLE=1)
else()
    # A windowed game: no console of its own. It still prints to the terminal
    # it was started from, and always to the session log (src/win_entry.c).
    target_link_options(BlueWake PRIVATE "LINKER:/SUBSYSTEM:WINDOWS" "LINKER:/ENTRY:mainCRTStartup")
endif()

# The donor DSP (Dolphin's DSP interpreter and its high-level Zelda ucode, from
# RecompCore) compiled from source, with only the Common services it needs
# (apple/ios/src/dsp_common_shim.cpp), as on iOS.
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
# No _M_X86_64: that selects Dolphin's x64 DSP JIT, and BlueWake runs the DSP
# interpreter everywhere (DSPEmitterNull), as on iOS.
target_compile_definitions(bluewake_dsp_donor PUBLIC
    _ARCH_64=1 FMT_EXCEPTIONS=0
    __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_features(bluewake_dsp_donor PUBLIC cxx_std_23)
target_compile_options(bluewake_dsp_donor PRIVATE -fno-strict-aliasing -w)
# Dolphin builds with a conforming preprocessor (MSVC /Zc:preprocessor); its
# assert macros forward __VA_ARGS__ through __VA_OPT__, which clang's MSVC
# compatibility mode expands the old MSVC way, miscounting format arguments.
target_compile_options(bluewake_dsp_donor PRIVATE -fno-ms-compatibility)
target_link_libraries(bluewake_dsp_donor PUBLIC fmt::fmt xxhash ZLIB::ZLIB)

add_library(bluewake_dsp_adapter STATIC
    ${BLUEWAKE_HOST_SRC}/dsp_adapter.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_c.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_donor_stubs.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_hle_backend.cpp)
target_include_directories(bluewake_dsp_adapter PRIVATE ${BLUEWAKE_COMPAT_INCLUDES})
target_compile_options(bluewake_dsp_adapter PRIVATE -fno-ms-compatibility)
target_link_libraries(bluewake_dsp_adapter PUBLIC bluewake_dsp_donor bluewake_posix_compat)
target_compile_definitions(BlueWake PRIVATE BLUEWAKE_HAS_DSP_ADAPTER=1)
target_link_libraries(BlueWake PRIVATE bluewake_dsp_adapter)

# Tools the builder runs on the player's disc.
add_executable(bluewake_disc_extract
    ${BLUEWAKE_REPO_ROOT}/scripts/ios/disc_extract.c
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src/disc_import.c)
target_include_directories(bluewake_disc_extract PRIVATE
    ${BLUEWAKE_COMPAT_INCLUDES} ${BLUEWAKE_REPO_ROOT}/apple/ios/src)
target_compile_options(bluewake_disc_extract PRIVATE ${BLUEWAKE_COMPAT_FORCE_INCLUDE})
target_link_libraries(bluewake_disc_extract PRIVATE bluewake_posix_compat bcrypt)

# The app folder: the executable, its runtime DLLs (SDL3, Dawn and the DXC
# shader compiler Dawn loads for Direct3D 12) and Aurora's bundled pipeline
# cache. The builder adds the game module and the prepared game files.
# The Windows seed (windows/resources) is the Mac's 213 pipelines and those a
# tour of the game asked for (towns, islands, dungeons and their bosses,
# interiors, the sea): 513, compiled on several threads at start, so a place's
# first visit draws everything from its first frame. Its rows are pipeline
# state keys, not game data.
include(${GXRUNTIME_DIR}/graphics/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
aurora_copy_runtime_dlls(BlueWake)
set(BLUEWAKE_PIPELINE_SEED "${BLUEWAKE_REPO_ROOT}/windows/resources/initial_pipeline_cache.db")
add_custom_command(TARGET BlueWake POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${BLUEWAKE_PIPELINE_SEED}" $<TARGET_FILE_DIR:BlueWake>
    VERBATIM)
