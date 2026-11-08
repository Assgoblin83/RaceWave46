#include "wr64_game_prepare.hpp"

#ifdef WR64_GAME_MODULE

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "librecomp/game.hpp"

#include "wr64_game_builder.hpp"
#include "wr64_game_module.hpp"

extern std::vector<recomp::GameEntry> supported_games;

namespace wr64::game_prepare {
namespace {

using namespace recompui;

std::atomic<bool> build_running{false};
Modal* progress_modal = nullptr;
Label* percent_label = nullptr;
Label* step_label = nullptr;
Element* bar_fill = nullptr;
Button* close_button = nullptr;
std::function<void(bool)> pending_completion;
ContextId caller_context = ContextId::null();

std::filesystem::path stored_rom_path() {
    return recomp::get_config_path() / supported_games.front().stored_filename();
}

game_builder::Paths build_paths() {
    return game_builder::default_paths(stored_rom_path(),
        recomp::get_config_path() / "game-module");
}

void ensure_modal() {
    if (progress_modal != nullptr) {
        return;
    }
    progress_modal = Modal::create_modal(ModalType::Fullscreen);
    ContextId context = progress_modal->modal_root_context;
    context.open();

    auto header = progress_modal->get_header();
    if (header != nullptr) {
        auto title = context.create_element<Label>(header->get_left(),
            "Preparing Wave Race 64", LabelStyle::Large);
        title->set_margin(16.0f);
        title->set_color(theme::color::Text);
    }

    Element* body = progress_modal->get_body();
    body->set_display(Display::Flex);
    body->set_flex_direction(FlexDirection::Column);
    body->set_justify_content(JustifyContent::Center);
    body->set_padding(48.0f);
    body->set_gap(24.0f);

    // Plan: the percentage sits above the progress line, the step below it.
    percent_label = context.create_element<Label>(body, "0%", LabelStyle::Large);
    percent_label->set_font_size(72.0f);
    percent_label->set_line_height(84.0f);
    percent_label->set_color(theme::color::Text);

    auto bar = context.create_element<Element>(body);
    bar->set_width(100.0f, Unit::Percent);
    bar->set_height(28.0f);
    bar->set_flex_shrink(0.0f);
    bar->set_border_radius(14.0f);
    bar->set_background_color(Color{26, 30, 38, 255});
    bar->set_border_width(2.0f);
    bar->set_border_color(Color{90, 98, 115, 255});

    bar_fill = context.create_element<Element>(bar);
    bar_fill->set_height(100.0f, Unit::Percent);
    bar_fill->set_width(0.0f, Unit::Percent);
    bar_fill->set_border_radius(14.0f);
    bar_fill->set_background_color(Color{64, 192, 120, 255});

    step_label = context.create_element<Label>(body, "", LabelStyle::Normal);
    step_label->set_font_size(28.0f);
    step_label->set_line_height(36.0f);
    step_label->set_color(theme::color::Text);

    close_button = context.create_element<Button>(body, "Close", ButtonStyle::Secondary);
    close_button->set_display(Display::None);
    close_button->set_min_width(240.0f);
    close_button->add_pressed_callback([] {
        if (progress_modal != nullptr) {
            progress_modal->close();
        }
    });
    context.close();
}

void set_progress(int percent, const std::string& step) {
    const bool opened = progress_modal != nullptr &&
        progress_modal->modal_root_context.open_if_not_already();
    if (percent_label != nullptr) {
        percent_label->set_text(std::to_string(percent) + "%");
    }
    if (bar_fill != nullptr) {
        // recompui percentages are 0..100, not 0..1.
        bar_fill->set_width(static_cast<float>(std::clamp(percent, 0, 100)), Unit::Percent);
    }
    if (step_label != nullptr) {
        step_label->set_text(step);
    }
    if (opened) {
        progress_modal->modal_root_context.close();
    }
}

void finish(bool ok, const std::string& error, const std::filesystem::path& module) {
    std::string message = error;
    if (ok) {
        // Load the freshly built module into this process before Start Game
        // becomes available again. This also registers its overlays.
        if (!game_module::load(module)) {
            ok = false;
            message = game_module::last_error();
        }
    }

    const bool opened = progress_modal != nullptr &&
        progress_modal->modal_root_context.open_if_not_already();
    if (ok) {
        if (percent_label != nullptr) {
            percent_label->set_text("100%");
        }
        if (bar_fill != nullptr) {
            bar_fill->set_width(100.0f, Unit::Percent);
        }
        if (step_label != nullptr) {
            step_label->set_text("Ready");
        }
    } else {
        std::fprintf(stderr, "[prepare] %s\n", message.c_str());
        if (percent_label != nullptr) {
            percent_label->set_text("Preparation failed");
        }
        if (step_label != nullptr) {
            step_label->set_text(message);
        }
        if (close_button != nullptr) {
            close_button->set_display(Display::Flex);
        }
    }
    if (opened) {
        progress_modal->modal_root_context.close();
    }
    if (ok && progress_modal != nullptr) {
        progress_modal->close();
    }

    build_running.store(false);
    auto completion = std::move(pending_completion);
    pending_completion = nullptr;
    if (completion) {
        const bool reopened = caller_context != ContextId::null() &&
            caller_context.open_if_not_already();
        completion(ok);
        if (reopened) {
            caller_context.close();
        }
    }
}

void prepare(std::function<void(bool)> completion) {
    if (build_running.exchange(true)) {
        return;
    }
    pending_completion = std::move(completion);

    // show_context() cannot open the modal while the launcher context is
    // open; element creation needs a context of its own as well.
    ContextId previous = recompui::get_current_context();
    caller_context = previous;
    previous.close();
    ensure_modal();
    set_progress(0, "Reading ROM");
    {
        const bool opened = progress_modal->modal_root_context.open_if_not_already();
        if (close_button != nullptr) {
            close_button->set_display(Display::None);
        }
        if (opened) {
            progress_modal->modal_root_context.close();
        }
    }
    progress_modal->open();
    previous.open();

    const game_builder::Paths paths = build_paths();
    std::thread worker([paths] {
        game_builder::Progress progress;
        progress.report = [](int percent, std::string_view step) {
            std::string text{step};
            recompui::queue_ui_task([percent, text = std::move(text)] {
                set_progress(percent, text);
            });
        };
        progress.cancelled = [] { return false; };
        const game_builder::Result result = game_builder::build(paths, progress);
        recompui::queue_ui_task([result] {
            finish(result.ok, result.error, result.module);
        });
    });
    worker.detach();
}

bool is_ready() {
    std::error_code ignored;
    if (!std::filesystem::is_regular_file(stored_rom_path(), ignored)) {
        return false;
    }
    std::filesystem::path module;
    return game_builder::prepared(build_paths(), module);
}

}  // namespace

void initialize() {
    recompui::register_game_preparation_callbacks({
        .is_ready = is_ready,
        .prepare = prepare,
    });

    // Lets automated UI checks exercise the progress dialog without a
    // synthetic click. Normal runs never set this.
    if (std::getenv("WR64_TEST_AUTO_PREPARE") != nullptr) {
        recompui::register_launcher_update_callback([](recompui::LauncherMenu*) {
            static bool started = false;
            if (started) {
                return;
            }
            std::error_code ignored;
            if (!std::filesystem::is_regular_file(stored_rom_path(), ignored) || is_ready()) {
                return;
            }
            started = true;
            prepare([](bool) {});
        });
    }
}

}  // namespace wr64::game_prepare

#endif
