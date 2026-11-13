// PC VR's check that main is the module its game sites were found in (runtime/pc_vr.c pc_vr_main_compatible; the
// fixtures call it with made-up tables). Header-only: no other runtime dependency.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "game_sites.h"
#include "gamelib.h"
#include "rt.h"

// PC VR's sites are all there (an optional group: all or none) and main has the layout they were found in.
static inline bool pc_vr_main_layout(const GameLibTables *tables, const Module *main_module) {
    const uint64_t size = g_site.main_end > MAIN_BASE ? g_site.main_end - MAIN_BASE : 0;
    return g_site.pcvr_matrix_builder && size && tables && tables->nmodules && *tables->nmodules && tables->modules &&
           tables->modules[0].name && !strcmp(tables->modules[0].name, "main") &&
           tables->modules[0].base == MAIN_BASE && tables->modules[0].size == size &&
           main_module && main_module->base == MAIN_BASE && main_module->size == size;
}
