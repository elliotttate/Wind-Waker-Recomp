cmake_minimum_required(VERSION 3.19)
project(bluewake_composite C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "GZLE01" CACHE STRING "Six-character disc ID")
set(COMPOSITE_DIR "" CACHE PATH "Composite output dir (generated.h, module_tables.inc, rel_modules.inc)")
set(GXRUNTIME_DIR "" CACHE PATH "GXRuntime checkout")
set(ABI_DIR "" CACHE PATH "Directory containing StaticRecompABI.h")
set(COMPOSITE_OPTIMIZATION_LEVEL "2" CACHE STRING "Generated composite optimization level (0, 1, or 2)")
set_property(CACHE COMPOSITE_OPTIMIZATION_LEVEL PROPERTY STRINGS 0 1 2)
if(NOT COMPOSITE_OPTIMIZATION_LEVEL MATCHES "^[012]$")
  message(FATAL_ERROR "COMPOSITE_OPTIMIZATION_LEVEL must be 0, 1, or 2")
endif()
set(COMPOSITE_O1_FALLBACK_SOURCES
  "chunks_d_a_npc_so_148/chunk_0000_rel1_C10800F4.c;chunks_dol/chunk_0140_text1_802316E0.c;chunks_dol/chunk_0202_text1_803296E0.c"
  CACHE STRING "Composite-relative generated sources compiled at O1 when the target uses O2")

foreach(REQUIRED_FILE generated.h generated_composite.h module_tables.inc rel_modules.inc rel_data.inc)
  if(NOT EXISTS "${COMPOSITE_DIR}/${REQUIRED_FILE}")
    message(FATAL_ERROR "Missing ${REQUIRED_FILE} in COMPOSITE_DIR=${COMPOSITE_DIR}")
  endif()
endforeach()
if(NOT EXISTS "${GXRUNTIME_DIR}")
  message(FATAL_ERROR "GXRuntime not found at ${GXRUNTIME_DIR}")
endif()
if(NOT EXISTS "${ABI_DIR}/StaticRecompABI.h")
  message(FATAL_ERROR "StaticRecompABI.h not found in ${ABI_DIR}")
endif()

set(MODULE_NAME "g${GAME_ID}_recomp")

# Collect the DOL's C or native-object chunks plus C REL chunks.
file(GLOB DOL_CHUNK_SOURCES "${COMPOSITE_DIR}/chunks_dol/*.c")
file(GLOB DOL_CHUNK_OBJECTS "${COMPOSITE_DIR}/chunks_dol/*.o")
if(DOL_CHUNK_SOURCES AND DOL_CHUNK_OBJECTS)
  message(FATAL_ERROR "DOL composite contains both C and native object chunks")
endif()
if(NOT DOL_CHUNK_SOURCES AND NOT DOL_CHUNK_OBJECTS)
  message(FATAL_ERROR "DOL composite contains no translated chunks")
endif()
set_source_files_properties(${DOL_CHUNK_OBJECTS}
  PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
file(GLOB REL_CHUNK_GLOBS LIST_DIRECTORIES TRUE "${COMPOSITE_DIR}/chunks_*")
set(CHUNK_INPUTS "")
foreach(CHUNK_DIR ${REL_CHUNK_GLOBS})
  if(IS_DIRECTORY "${CHUNK_DIR}" AND NOT CHUNK_DIR MATCHES "chunks_dol")
    file(GLOB RC "${CHUNK_DIR}/*.c")
    list(APPEND CHUNK_INPUTS ${RC})
  endif()
endforeach()
list(APPEND CHUNK_INPUTS ${DOL_CHUNK_SOURCES})
list(APPEND CHUNK_INPUTS ${DOL_CHUNK_OBJECTS})
list(LENGTH CHUNK_INPUTS NUM_CHUNKS)
message(STATUS "${NUM_CHUNKS} chunk translation units")

add_library(${MODULE_NAME} SHARED
  module_export.c
  native_math.c
  native_work_pool.c
  dispatch_loop.c
  guest_cpu.c
  direct_calls.c
  gather_pipe.c
  native_skin.c
  native_vec.c
  native_j3d.c
  # Certified game math entry hooks (scripts/windows/native_game_math.py).
  native_game_math.c
  ${GXRUNTIME_DIR}/src/core/cpu.c
  ${GXRUNTIME_DIR}/src/core/cpu_exception.c
  ${GXRUNTIME_DIR}/src/core/cpu_interpreter.c
  ${GXRUNTIME_DIR}/src/core/cpu_interpreter_table.c
  ${GXRUNTIME_DIR}/src/core/cpu_interpreter_float.c
  ${GXRUNTIME_DIR}/src/core/cpu_interpreter_integer.c
  ${CHUNK_INPUTS})

set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${MODULE_NAME}" UNITY_BUILD OFF C_VISIBILITY_PRESET hidden)
target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}" DOLRECOMP_CPU_HEADER="core/cpu.h")
target_include_directories(${MODULE_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${COMPOSITE_DIR}" "${GXRUNTIME_DIR}/include" "${ABI_DIR}")
if(EXISTS "${COMPOSITE_DIR}/simulation_60hz.json")
  file(READ "${COMPOSITE_DIR}/simulation_60hz.json" SIMULATION_MANIFEST)
  string(JSON SIMULATION_ABI GET "${SIMULATION_MANIFEST}" abi)
  if(NOT SIMULATION_ABI EQUAL 1)
    message(FATAL_ERROR "Unsupported simulation timing manifest")
  endif()
  string(JSON TIMING_FILE_COUNT LENGTH "${SIMULATION_MANIFEST}" files)
  if(TIMING_FILE_COUNT LESS 1)
    message(FATAL_ERROR "Empty simulation timing manifest")
  endif()
  math(EXPR TIMING_LAST_FILE "${TIMING_FILE_COUNT} - 1")
  foreach(INDEX RANGE 0 ${TIMING_LAST_FILE})
    string(JSON TIMING_FILE MEMBER "${SIMULATION_MANIFEST}" files ${INDEX})
    string(JSON TIMING_EXPECTED GET "${SIMULATION_MANIFEST}" files "${TIMING_FILE}")
    file(SHA256 "${COMPOSITE_DIR}/${TIMING_FILE}" TIMING_ACTUAL)
    if(NOT TIMING_ACTUAL STREQUAL TIMING_EXPECTED)
      message(FATAL_ERROR "Timing source changed: rerun prepare_simulation_60hz.py (${TIMING_FILE})")
    endif()
  endforeach()
  target_sources(${MODULE_NAME} PRIVATE simulation_timing.c)
  target_compile_definitions(${MODULE_NAME} PRIVATE BLUEWAKE_SIMULATION_PATCHED=1)
  if(WIN32)
    # The host finds the timing API by name (simulation_mode.c).
    foreach(EXPORTED_SYMBOL bluewake_composite_simulation_abi bluewake_composite_simulation_enable
        bluewake_composite_simulation_stats)
      target_link_options(${MODULE_NAME} PRIVATE "LINKER:/EXPORT:${EXPORTED_SYMBOL}")
    endforeach()
  endif()
endif()
if(EXISTS "${COMPOSITE_DIR}/native_math.json")
  file(READ "${COMPOSITE_DIR}/native_math.json" NATIVE_MATH_MANIFEST)
  string(JSON NATIVE_MATH_ABI GET "${NATIVE_MATH_MANIFEST}" abi)
  string(JSON NATIVE_MATH_FILES LENGTH "${NATIVE_MATH_MANIFEST}" files)
  if(NOT NATIVE_MATH_ABI EQUAL 1 OR NATIVE_MATH_FILES LESS 2)
    message(FATAL_ERROR "Invalid native math certification manifest")
  endif()
  math(EXPR NATIVE_MATH_LAST "${NATIVE_MATH_FILES} - 1")
  foreach(INDEX RANGE 0 ${NATIVE_MATH_LAST})
    string(JSON SOURCE_FILE MEMBER "${NATIVE_MATH_MANIFEST}" files ${INDEX})
    string(JSON EXPECTED_HASH GET "${NATIVE_MATH_MANIFEST}" files "${SOURCE_FILE}")
    file(SHA256 "${COMPOSITE_DIR}/${SOURCE_FILE}" ACTUAL_HASH)
    if(NOT ACTUAL_HASH STREQUAL EXPECTED_HASH)
      message(FATAL_ERROR "Native math source changed: rerun prepare_native_math.py (${SOURCE_FILE})")
    endif()
  endforeach()
  target_compile_definitions(${MODULE_NAME} PRIVATE BLUEWAKE_NATIVE_MATH_VERIFIED=1)
endif()
if(APPLE)
  set_target_properties(${MODULE_NAME} PROPERTIES SUFFIX ".dylib")
endif()
if(EXISTS "${COMPOSITE_DIR}/native_j3d.json")
  file(READ "${COMPOSITE_DIR}/native_j3d.json" J3D_MANIFEST)
  string(JSON J3D_ABI GET "${J3D_MANIFEST}" abi)
  string(JSON J3D_FILES LENGTH "${J3D_MANIFEST}" files)
  if(NOT J3D_ABI EQUAL 1 OR J3D_FILES LESS 1)
    message(FATAL_ERROR "Invalid native J3D certification manifest")
  endif()
  math(EXPR J3D_LAST "${J3D_FILES} - 1")
  foreach(INDEX RANGE 0 ${J3D_LAST})
    string(JSON SOURCE_FILE MEMBER "${J3D_MANIFEST}" files ${INDEX})
    string(JSON EXPECTED_HASH GET "${J3D_MANIFEST}" files "${SOURCE_FILE}")
    file(SHA256 "${COMPOSITE_DIR}/${SOURCE_FILE}" ACTUAL_HASH)
    if(NOT ACTUAL_HASH STREQUAL EXPECTED_HASH)
      message(FATAL_ERROR "Native J3D source changed: rerun prepare_native_j3d.py (${SOURCE_FILE})")
    endif()
  endforeach()
  set_property(SOURCE module_export.c APPEND PROPERTY COMPILE_DEFINITIONS BLUEWAKE_NATIVE_J3D_VERIFIED=1)
endif()
if(EXISTS "${COMPOSITE_DIR}/native_gpr.json")
  file(READ "${COMPOSITE_DIR}/native_gpr.json" GPR_MANIFEST)
  string(JSON GPR_ABI GET "${GPR_MANIFEST}" abi)
  string(JSON GPR_FILES LENGTH "${GPR_MANIFEST}" files)
  if(NOT GPR_ABI EQUAL 1 OR GPR_FILES LESS 1)
    message(FATAL_ERROR "Invalid native GPR certification manifest")
  endif()
  math(EXPR GPR_LAST "${GPR_FILES} - 1")
  foreach(INDEX RANGE 0 ${GPR_LAST})
    string(JSON SOURCE_FILE MEMBER "${GPR_MANIFEST}" files ${INDEX})
    string(JSON EXPECTED_HASH GET "${GPR_MANIFEST}" files "${SOURCE_FILE}")
    file(SHA256 "${COMPOSITE_DIR}/${SOURCE_FILE}" ACTUAL_HASH)
    if(NOT ACTUAL_HASH STREQUAL EXPECTED_HASH)
      message(FATAL_ERROR "Native GPR source changed: rerun prepare_native_gpr.py (${SOURCE_FILE})")
    endif()
  endforeach()
  target_compile_definitions(${MODULE_NAME} PRIVATE BLUEWAKE_NATIVE_GPR_VERIFIED=1)
endif()
if(EXISTS "${COMPOSITE_DIR}/bw_edge_watch.inc")
  # The Windows builder's list of the guest addresses the host's edge service
  # acts at (scripts/windows/direct_calls.py): the dispatch loop skips the
  # service elsewhere, and indirect calls can run directly (direct_calls.h).
  target_compile_definitions(${MODULE_NAME} PRIVATE BLUEWAKE_EDGE_FILTER=1)
endif()
# The chassis loop hands the chunks' gather-pipe batch (gather_pipe.c) to the
# host wherever the host looks (dispatch_loop.h).
target_compile_definitions(${MODULE_NAME} PRIVATE BLUEWAKE_GATHER_PIPE_BATCH=1)
if(WIN32)
  # MEM1 as a global array beside the guest CPU's state (guest_cpu.c,
  # core/cpu.h's BW_GUEST_MEM1): guest registers stay in host registers
  # across guest loads and stores. 32 MiB, the host's linked RAM size.
  target_compile_definitions(${MODULE_NAME} PRIVATE BW_GUEST_MEM1=bw_guest_mem1
      BW_GUEST_MEM1_SIZE=0x02000000u)
endif()
find_package(Threads REQUIRED)
target_link_libraries(${MODULE_NAME} PRIVATE Threads::Threads)
if(WIN32)
  # A DLL exports only what is marked dllexport. module_export.c marks its own
  # entry points; the guest-alias and write-journal hooks the host resolves in
  # the runtime embedded here (GXRuntime cpu.c) are named for the linker.
  foreach(EXPORTED_SYMBOL
      ppc_set_mem_write_journal ppc_guest_alias_clear ppc_guest_alias_add
      ppc_guest_alias_add_shared ppc_guest_alias_get_storage
      ppc_guest_alias_remove ppc_guest_alias_resolve)
    target_link_options(${MODULE_NAME} PRIVATE "LINKER:/EXPORT:${EXPORTED_SYMBOL}")
  endforeach()
  target_compile_definitions(${MODULE_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
  # The Windows C runtime's getenv is slow, and the embedded runtime checks a
  # trace switch on every guest exception: remember each call site's answer
  # (windows/compat/bw_getenv_cache.h). Only these sources, whose every getenv
  # names a literal; the translated chunks never call it.
  set_property(SOURCE module_export.c dispatch_loop.c
      ${GXRUNTIME_DIR}/src/core/cpu.c ${GXRUNTIME_DIR}/src/core/cpu_exception.c
      ${GXRUNTIME_DIR}/src/core/cpu_interpreter.c ${GXRUNTIME_DIR}/src/core/cpu_interpreter_table.c
      ${GXRUNTIME_DIR}/src/core/cpu_interpreter_float.c ${GXRUNTIME_DIR}/src/core/cpu_interpreter_integer.c
      APPEND PROPERTY COMPILE_OPTIONS
      -include "${CMAKE_CURRENT_SOURCE_DIR}/../../windows/compat/bw_getenv_cache.h")
else()
  target_link_libraries(${MODULE_NAME} PRIVATE m)
endif()
target_compile_options(${MODULE_NAME} PRIVATE -O${COMPOSITE_OPTIMIZATION_LEVEL} -ffp-contract=off)

# The builder's --tiered: chunks the optimization training never ran, one
# composite-relative path a line, compiled at -O1 without GVN's memory
# dependence analysis instead of -O2 (scripts/windows/build.py cold_sources).
set(COMPOSITE_COLD_SOURCES_FILE "" CACHE FILEPATH "Chunks compiled at O1 when the target uses O2")
if(COMPOSITE_OPTIMIZATION_LEVEL STREQUAL "2" AND COMPOSITE_COLD_SOURCES_FILE)
  file(STRINGS "${COMPOSITE_COLD_SOURCES_FILE}" COLD_SOURCES)
  list(LENGTH COLD_SOURCES COLD_COUNT)
  foreach(COLD_SOURCE IN LISTS COLD_SOURCES)
    set(COLD_PATH "${COMPOSITE_DIR}/${COLD_SOURCE}")
    if(EXISTS "${COLD_PATH}")
      set_property(SOURCE "${COLD_PATH}" APPEND PROPERTY COMPILE_OPTIONS -O1 -mllvm -enable-gvn-memdep=false)
    endif()
  endforeach()
  message(STATUS "Tiered: ${COLD_COUNT} cold chunks at O1")
endif()

if(COMPOSITE_OPTIMIZATION_LEVEL STREQUAL "2")
  foreach(FALLBACK_SOURCE IN LISTS COMPOSITE_O1_FALLBACK_SOURCES)
    set(FALLBACK_PATH "${COMPOSITE_DIR}/${FALLBACK_SOURCE}")
    if(NOT EXISTS "${FALLBACK_PATH}")
      message(FATAL_ERROR "O1 fallback source not found: ${FALLBACK_PATH}")
    endif()
    list(FIND CHUNK_INPUTS "${FALLBACK_PATH}" FALLBACK_INDEX)
    if(FALLBACK_INDEX EQUAL -1)
      message(FATAL_ERROR "O1 fallback is not a generated chunk: ${FALLBACK_PATH}")
    endif()
    set_property(SOURCE "${FALLBACK_PATH}" APPEND PROPERTY COMPILE_OPTIONS -O1)
    message(STATUS "O1 optimizer fallback: ${FALLBACK_SOURCE}")
  endforeach()
endif()
