# Keep direct desktop/iOS CMake builds on the same verified runtime delta as
# bootstrap and the builder. The helper refuses unrelated dependency edits.
get_filename_component(_bluewake_patch_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_bluewake_patch_root}/scripts/apply_recompcore_patches.py"
    RESULT_VARIABLE _bluewake_patch_result
    OUTPUT_VARIABLE _bluewake_patch_output
    ERROR_VARIABLE _bluewake_patch_error)
if(NOT _bluewake_patch_result EQUAL 0)
    message(FATAL_ERROR "${_bluewake_patch_error}")
endif()
string(STRIP "${_bluewake_patch_output}" _bluewake_patch_output)
message(STATUS "${_bluewake_patch_output}")
