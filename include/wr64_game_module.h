#pragma once

#include <stdint.h>

#include "recomp.h"
#include "librecomp/sections.h"

// Interface between the host executable, which ships without ROM-derived code,
// and libwr64game.so, which is generated from the player's own cartridge.
// Both sides are built from the same source tree, so the layout below only
// changes together with WR64_GAME_MODULE_ABI_VERSION.
#define WR64_GAME_MODULE_ABI_VERSION 1u
#define WR64_GAME_MODULE_ABI_FUNCTION "wr64_game_module_abi"

#ifdef __cplusplus
extern "C" {
#endif

// RSP microcode entry points return RspExitReason, an enum class with an int
// underlying type. The ABI spells it out so this header stays plain C.
typedef int32_t (*wr64_rsp_ucode_func)(uint8_t* rdram, uint32_t ucode_addr);

// The game code calls back into the host through its normal wr64_* hooks,
// which are resolved from the exported symbols of the executable. This struct
// carries the calls that go the other way: host code that drives recompiled
// functions directly.
typedef struct Wr64GameModule {
    uint32_t abi_version;
    uint32_t struct_size;
    // SHA-256 of config/generated-code.json at build time.
    const char* recipe_hash;
    // Sign-extended KSEG0 address, exactly as get_entrypoint_address()
    // returns it; librecomp's MEM_* macros rely on that form.
    gpr entrypoint_address;
    recomp_func_t* entrypoint;
    wr64_rsp_ucode_func asp_main;
    SectionTableEntry* code_sections;
    uint32_t num_code_sections;
    uint32_t total_num_sections;
    int* overlay_table;
    uint32_t num_overlays;
    recomp_func_t* func_80051538;
    recomp_func_t* func_800694B8;
    recomp_func_t* func_80089C08;
    recomp_func_t* func_80089DEC;
    recomp_func_t* func_80089E24;
    recomp_func_t* func_80089F28;
    recomp_func_t* func_8008BD2C;
    recomp_func_t* func_8008CFEC;
    recomp_func_t* func_8008D454;
    recomp_func_t* func_8008D94C;
    recomp_func_t* func_800916B4;
    recomp_func_t* func_80096048;
    recomp_func_t* func_8009FCB0;
    recomp_func_t* func_800AC098;
    recomp_func_t* func_801E9B04;
    recomp_func_t* Mio0_Decompress;
} Wr64GameModule;

typedef const Wr64GameModule* (*Wr64GameModuleEntry)(void);

#ifdef __cplusplus
}
#endif
