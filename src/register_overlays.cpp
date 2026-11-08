#include "wr64_runtime.hpp"

#include "librecomp/overlays.hpp"

#ifdef WR64_GAME_MODULE
#include "wr64_game_module.hpp"

void wr64::register_overlays() {
    // The host has no section tables of its own in this build; they arrive
    // with the module after the player's ROM has been compiled.
    if (!game_module::loaded()) {
        return;
    }

    const Wr64GameModule& module = game_module::abi();
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = module.code_sections,
        .num_code_sections = module.num_code_sections,
        .total_num_sections = module.total_num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays{
        .table = module.overlay_table,
        .len = module.num_overlays,
    };

    recomp::overlays::register_overlays(sections, overlays);
}
#else
#include "../RecompiledFuncsPoint0/recomp_overlays.inl"

void wr64::register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}
#endif
