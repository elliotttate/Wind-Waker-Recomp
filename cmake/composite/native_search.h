#ifndef BLUEWAKE_NATIVE_SEARCH_H
#define BLUEWAKE_NATIVE_SEARCH_H

/* The actor search by name natively (native_search.c): strcmp,
 * dStage_searchName and cTgIt_JudgeFilter with fopAcM_findObjectCB as its
 * judge, hooked at their entries; and the judge's walk batched, for the
 * host. */

#include "core/cpu.h"

#define BLUEWAKE_SEARCH_STRCMP 0x8032DB44u      /* strcmp */
#define BLUEWAKE_SEARCH_STAGE_NAME 0x80041544u  /* dStage_searchName__FPCc */
#define BLUEWAKE_SEARCH_JUDGE_FILTER 0x80245640u /* cTgIt_JudgeFilter__FP16create_tag_classP12judge_filter */

extern int bluewake_native_search_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_search(CPUState* cpu, u32 address);
void bluewake_native_search_report(void);

/* The judge loop of fopAcM_searchFromName's walk, batched (native_search.c:
 * for the host's edge service at the boundary into cTgIt_JudgeFilter, where
 * it finds nothing to do, as host_actor_search_native in
 * runtime/host/src/main.c): the nodes it ran, each judged NULL by
 * fopAcM_findObjectCB, with the state the translation leaves at the same
 * boundary that many nodes later; 0 with nothing changed. Exported from the
 * module as bluewake_native_search_judge. */
#if defined(_WIN32)
#define BLUEWAKE_SEARCH_EXPORT __declspec(dllexport)
#else
#define BLUEWAKE_SEARCH_EXPORT __attribute__((visibility("default")))
#endif
BLUEWAKE_SEARCH_EXPORT unsigned bluewake_native_search_judge(CPUState* cpu);
/* Set when the certified JudgeFilter hook first runs: the walk runs only
 * then, in a module whose translation of it is the tested one. */
extern int bluewake_native_search_judge_ready;
void bluewake_native_search_judge_report(void);

#endif
