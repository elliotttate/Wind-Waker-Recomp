cmake_minimum_required(VERSION 3.25)
project(bluewake_training LANGUAGES C CXX)
# Reuse the normal desktop host and the same direct DSP source build as iOS.
# This requires no private donor build tree or precompiled game objects.
get_filename_component(BLUEWAKE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(BLUEWAKE_HOST_SRC "${BLUEWAKE_REPO_ROOT}/runtime/host/src")
set(BLUEWAKE_ENABLE_DSP_ADAPTER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${BLUEWAKE_REPO_ROOT}/runtime/host" host)
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
set(_dsp_includes
    ${_donor}/Source/Core ${_donor}/Source ${_donor}/Externals
    ${_donor}/Externals/xxhash/xxHash
    ${BLUEWAKE_REPO_ROOT}/apple/ios/dsp_generated)
target_include_directories(bluewake_dsp_donor PUBLIC ${_dsp_includes})
target_compile_definitions(bluewake_dsp_donor PUBLIC
    _ARCH_64=1 FMT_EXCEPTIONS=0 _DEFAULT_SOURCE
    __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
  target_compile_definitions(bluewake_dsp_donor PUBLIC _M_ARM_64=1)
endif()
target_compile_features(bluewake_dsp_donor PUBLIC cxx_std_23)
target_compile_options(bluewake_dsp_donor PRIVATE -fno-strict-aliasing -w)
target_link_libraries(bluewake_dsp_donor PUBLIC fmt::fmt xxhash)

add_library(bluewake_dsp_adapter STATIC
    ${BLUEWAKE_HOST_SRC}/dsp_adapter.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_c.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_donor_stubs.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_hle_backend.cpp)
target_link_libraries(bluewake_dsp_adapter PUBLIC bluewake_dsp_donor z)
if(APPLE)
  target_link_libraries(bluewake_dsp_adapter PUBLIC
    iconv "-framework CoreFoundation" "-framework Foundation")
endif()
target_compile_definitions(bluewake_host PRIVATE BLUEWAKE_HAS_DSP_ADAPTER=1)
target_link_libraries(bluewake_host PRIVATE bluewake_dsp_adapter)
