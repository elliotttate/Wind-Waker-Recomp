// Compiled into the Android builder's instrumented training module only
// (scripts/android/build.py, COMPOSITE_EXTRA_SOURCES): the profiling runtime's
// writer is hidden inside the module, so android_entry.c calls it through this.
int __llvm_profile_write_file(void);

__attribute__((visibility("default"))) int bluewake_profile_write(void) {
    return __llvm_profile_write_file();
}
