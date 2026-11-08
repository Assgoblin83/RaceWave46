#include "wr64_game_module.h"

#include "funcs.h"
#include "librecomp/rsp.hpp"
#include "recomp_overlays.inl"

extern RspUcodeFunc aspMain;
gpr get_entrypoint_address();

namespace {

// Generated RSP code is driven through this wrapper; the host only sees the
// plain int return type from the ABI.
int32_t call_asp_main(uint8_t* rdram, uint32_t ucode_addr) {
    return static_cast<int32_t>(aspMain(rdram, ucode_addr));
}

}  // namespace

extern "C" const Wr64GameModule* wr64_game_module_abi() {
    static const Wr64GameModule module{
        WR64_GAME_MODULE_ABI_VERSION,
        sizeof(Wr64GameModule),
        WR64_GAME_MODULE_RECIPE_HASH,
        get_entrypoint_address(),
        recomp_entrypoint,
        call_asp_main,
        section_table,
        static_cast<uint32_t>(ARRLEN(section_table)),
        static_cast<uint32_t>(num_sections),
        overlay_sections_by_index,
        static_cast<uint32_t>(ARRLEN(overlay_sections_by_index)),
        func_80051538,
        func_800694B8,
        func_80089C08,
        func_80089DEC,
        func_80089E24,
        func_80089F28,
        func_8008BD2C,
        func_8008CFEC,
        func_8008D454,
        func_8008D94C,
        func_800916B4,
        func_80096048,
        func_8009FCB0,
        func_800AC098,
        func_801E9B04,
        Mio0_Decompress,
    };
    return &module;
}
