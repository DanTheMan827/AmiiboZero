/** @file lockon.cpp @brief Lockon screen renderer. */
#include "lockon.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzLockOnScreen az_ui_lockon_screen{};

void AzLockOnScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    az_ui_draw_header(canvas, model, "Select lock-on");
    AmiiboZeroApp* app = model->app;
    uint16_t start = model->selection >= 2 ? (uint16_t)(model->selection - 2) : 0;
    if(app->lockon_count > AZ_LIST_ROWS && start + AZ_LIST_ROWS > app->lockon_count) {
        start = (uint16_t)(app->lockon_count - AZ_LIST_ROWS);
    }
    for(uint8_t row = 0; row < AZ_LIST_ROWS && start + row < app->lockon_count; row++) {
        if(!app->lockon_entries) break;
        const AzLockOnEntry* entry = &app->lockon_entries[start + row];
        az_ui_draw_list_row(canvas, model, row, entry->display_name, start + row == model->selection);
    }
    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignCenter, "No lock-on files");
        canvas_draw_str_aligned(canvas, 64, 39, AlignCenter, AlignCenter, "Put files in /lock_on/");
    }
    az_ui_draw_list_scrollbar(canvas, model->selection, model->count);
    az_ui_draw_footer(canvas, "Back", "OK", "");
}


bool AzLockOnScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyBack) {
        az_ui_clear_lockon_catalog(app);
        app->lockon_action = AzLockOnActionNone;
        az_ui_navigate(app, AzScreenFigure, false);
    } else if(event->key == InputKeyUp) {
        az_ui_move_selection(app, -1, app->lockon_count);
    } else if(event->key == InputKeyDown) {
        az_ui_move_selection(app, 1, app->lockon_count);
    } else if(short_press && event->key == InputKeyOk && app->lockon_count) {
        az_ui_apply_selected_lockon(app);
    }
    return true;

}
