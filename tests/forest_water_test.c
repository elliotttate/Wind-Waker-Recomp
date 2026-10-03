#include "forest_water.h"
#include "StaticRecompABI.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kItemRecord = 0x803C4C6Eu,
    kEvents = 0x803C522Cu,
    kTrees = 0x803C52CAu,
    kTimerRunning = 0x803CA7FEu,
    kReturn = 0x7FFF0000u,
};

static void configure(bool keep, bool longer) {
    setenv("BLUEWAKE_FOREST_WATER_KEEP_TREES", keep ? "1" : "0", 1);
    setenv("BLUEWAKE_FOREST_WATER_30_MINUTES", longer ? "1" : "0", 1);
    bluewake_forest_water_reload();
}

// Optional integration: execute the actual statically translated game helpers,
// rather than a test implementation of their instructions. No disc/card writes.
static void run_guest(const StaticRecompModuleDesc* mod, CPUState* cpu, u32 pc) {
    cpu->pc = pc;
    cpu->lr = kReturn;
    cpu->downcount = 1000000;
    for (unsigned i = 0; cpu->pc != kReturn; ++i) {
        if (i == 99) fprintf(stderr, "guest helper stuck: start=%08x pc=%08x lr=%08x\n", pc, cpu->pc, cpu->lr);
        assert(i < 100);
        assert(mod->dispatch(cpu, cpu->pc));
    }
}

static void progress_write(const StaticRecompModuleDesc* mod, CPUState* cpu,
                           u8 value, u32 caller) {
    cpu->gpr[3] = kEvents;
    cpu->gpr[4] = 0x9EFFu;
    cpu->gpr[5] = value;
    cpu->lr = caller;
    bluewake_forest_water_dispatch(cpu, 0x8005CB58u);
    if (mod != NULL)
        run_guest(mod, cpu, 0x8005CB58u);
}

int main(int argc, char** argv) {
    CPUState cpu = {0};
    cpu.ram_size = GC_MAIN_RAM_SIZE;
    cpu.ram = calloc(1, cpu.ram_size);
    assert(cpu.ram != NULL);
    const StaticRecompModuleDesc* mod = NULL;
    void* lib = NULL;
    if (argc == 2) {
        lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (lib == NULL) fprintf(stderr, "%s\n", dlerror());
        assert(lib != NULL);
        StaticRecompGetModuleFn get_module =
            (StaticRecompGetModuleFn)dlsym(lib, STATICRECOMP_GET_MODULE_SYMBOL);
        assert(get_module != NULL);
        mod = get_module();
        assert(mod->cpu_abi_version == GXRUNTIME_CPU_ABI_VERSION);
        assert(mod->cpu_state_size == sizeof cpu);
    }
    u32 tree_text = 0x81000000u;
    if (mod != NULL) {
        tree_text = 0;
        for (u32 i = 0; i < mod->num_rel_modules; ++i) {
            const StaticRecompRelModule* rel = &mod->rel_modules[i];
            if (rel->module_id != 317u) continue;
            for (u32 j = 0; j < rel->num_sections; ++j)
                if (rel->sections[j].section_index == 1u)
                    tree_text = rel->sections[j].linked_start;
        }
        assert(tree_text != 0);
    }
    bluewake_forest_water_set_ftree_text(tree_text);

    unsetenv("BLUEWAKE_FOREST_WATER_KEEP_TREES");
    unsetenv("BLUEWAKE_FOREST_WATER_30_MINUTES");
    bluewake_forest_water_reload();
    assert(!bluewake_forest_water_enabled);

    for (unsigned flags = 0; flags < 4; ++flags) {
        const bool keep = (flags & 1) != 0, longer = (flags & 2) != 0;
        configure(keep, longer);
        cpu.gpr[3] = kItemRecord;
        cpu.gpr[4] = 36000u;
        cpu.lr = 0x80153134u;
        bluewake_forest_water_dispatch(&cpu, 0x8005987Cu);
        assert(cpu.gpr[4] == (longer ? 54000u : 36000u));
        if (mod != NULL) {
            run_guest(mod, &cpu, 0x8005987Cu);
            assert(mem_read16(&cpu, kItemRecord) == (longer ? 54000u : 36000u));
            assert(mem_read8(&cpu, kTimerRunning) == 0);
            // The game's timer keeps ticking and stops at zero under every mode.
            mem_write16(&cpu, kItemRecord, 2);
            mem_write8(&cpu, kTimerRunning, 1);
            for (unsigned i = 0; i < 3; ++i) {
                cpu.gpr[3] = kItemRecord;
                run_guest(mod, &cpu, 0x80059894u);
            }
            assert(mem_read16(&cpu, kItemRecord) == 0);
            assert(mem_read8(&cpu, kTimerRunning) == 0);
        }
        mem_write8(&cpu, kTrees, 0x55u);
        progress_write(mod, &cpu, 0, 0x801218F0u);
        // The helper clobbers r5; check RAM after real game execution instead.
        if (mod != NULL) assert(mem_read8(&cpu, kTrees) == (keep ? 0x55u : 0));
        else assert(cpu.gpr[5] == (keep ? 0x55u : 0));
        // A currently loaded watered tree must also skip its wilt path.
        const u32 check = tree_text + 0x176Cu;
        cpu.gpr[3] = 0;
        bluewake_forest_water_dispatch(&cpu, check);
        assert(cpu.gpr[3] == (keep ? 1u : 0u));
        if (mod != NULL) {
            cpu.pc = check;
            cpu.downcount = 1;
            assert(mod->dispatch(&cpu, check));
            assert(cpu.pc == check + (keep ? 0x38u : 8u));
        }
        cpu.gpr[3] = 42;
        bluewake_forest_water_dispatch(&cpu, check);
        assert(cpu.gpr[3] == 42);
        cpu.gpr[3] = 0;
        bluewake_forest_water_dispatch(&cpu, check + 4u);
        assert(cpu.gpr[3] == 0);
    }

    configure(true, true);
    // Enabling midway must not reset the running timer or add watered trees.
    mem_write16(&cpu, kItemRecord, 1234u);
    mem_write8(&cpu, kTrees, 0x80u);
    bluewake_forest_water_reload();
    assert(mem_read16(&cpu, kItemRecord) == 1234u);
    assert(mem_read8(&cpu, kTrees) == 0x80u);
    // Rewinds/new saves use their own progress, never a remembered host mask.
    mem_write8(&cpu, kTrees, 1u);
    progress_write(mod, &cpu, 0, 0x801218F0u);
    if (mod != NULL) assert(mem_read8(&cpu, kTrees) == 1);
    else assert(cpu.gpr[5] == 1);
    // Only the expiration caller is protected. Normal changes remain legal.
    progress_write(mod, &cpu, 0, kReturn);
    if (mod != NULL) assert(mem_read8(&cpu, kTrees) == 0);
    else assert(cpu.gpr[5] == 0);
    // Watering the remaining trees and completion/reward writes still work.
    for (unsigned n = 1; n <= 8; ++n) {
        progress_write(mod, &cpu, (u8)((1u << n) - 1), kReturn);
        if (mod != NULL) assert(mem_read8(&cpu, kTrees) == (u8)((1u << n) - 1));
    }
    cpu.gpr[3] = kEvents;
    cpu.gpr[4] = 0x0102u;
    cpu.gpr[5] = 0;
    cpu.lr = 0x801218F0u;
    bluewake_forest_water_dispatch(&cpu, 0x8005CB58u);
    assert(cpu.gpr[5] == 0);
    // Another register, another save object, another timer value/caller: inert.
    cpu.gpr[4] = 0x9AFFu;
    bluewake_forest_water_dispatch(&cpu, 0x8005CB58u);
    assert(cpu.gpr[5] == 0);
    cpu.gpr[3] = kEvents + 0x100;
    cpu.gpr[4] = 0x9EFFu;
    bluewake_forest_water_dispatch(&cpu, 0x8005CB58u);
    assert(cpu.gpr[5] == 0);
    cpu.gpr[3] = kItemRecord;
    cpu.gpr[4] = 36000u;
    cpu.lr = kReturn;
    bluewake_forest_water_dispatch(&cpu, 0x8005987Cu);
    assert(cpu.gpr[4] == 36000u);
    cpu.lr = 0x80153134u;
    cpu.gpr[4] = 1;
    bluewake_forest_water_dispatch(&cpu, 0x8005987Cu);
    assert(cpu.gpr[4] == 1);
    cpu.gpr[3] = kItemRecord + 0x100;
    cpu.gpr[4] = 36000u;
    bluewake_forest_water_dispatch(&cpu, 0x8005987Cu);
    assert(cpu.gpr[4] == 36000u);
    configure(false, false);
    progress_write(mod, &cpu, 0, 0x801218F0u);
    if (mod != NULL) assert(mem_read8(&cpu, kTrees) == 0);
    else assert(cpu.gpr[5] == 0);
    bluewake_forest_water_set_ftree_text(0);
    configure(true, true);
    cpu.gpr[3] = 0;
    bluewake_forest_water_dispatch(&cpu, tree_text + 0x176Cu);
    assert(cpu.gpr[3] == 0);
    bluewake_forest_water_enter(NULL, 0x8005987Cu);
    free(cpu.ram);
    if (lib != NULL) dlclose(lib);
    puts(mod != NULL ? "Forest Water: translated-game integration passed" : "Forest Water: option guards passed");
    return 0;
}
