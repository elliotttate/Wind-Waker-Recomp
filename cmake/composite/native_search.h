#ifndef BLUEWAKE_NATIVE_SEARCH_H
#define BLUEWAKE_NATIVE_SEARCH_H

/* The actor search by name natively (native_search.c): strcmp and
 * dStage_searchName, hooked at their entries. */

#include "core/cpu.h"

#define BLUEWAKE_SEARCH_STRCMP 0x8032DB44u      /* strcmp */
#define BLUEWAKE_SEARCH_STAGE_NAME 0x80041544u  /* dStage_searchName__FPCc */

extern int bluewake_native_search_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_search(CPUState* cpu, u32 address);
void bluewake_native_search_report(void);

#endif
