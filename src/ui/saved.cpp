/** @file saved.cpp @brief Saved screen renderer. */
#include "saved.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzSavedScreen az_ui_saved_screen{};

void AzSavedScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    az_ui_draw_header(canvas, model, "Saved figures");
    AmiiboZeroApp* app = model->app;
    uint16_t start = model->selection >= 2 ? (uint16_t)(model->selection - 2) : 0;
    if(app->saved_count > AZ_LIST_ROWS && start + AZ_LIST_ROWS > app->saved_count) {
        start = (uint16_t)(app->saved_count - AZ_LIST_ROWS);
    }
    for(uint8_t row = 0; row < AZ_LIST_ROWS && start + row < app->saved_count; row++) {
        if(!app->saved_entries) break;
        const AzSavedEntry* entry = &app->saved_entries[start + row];
        az_ui_draw_list_row(canvas, model, row, entry->display_name, start + row == model->selection);
    }
    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "No saved figures yet");
        canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignCenter, "Browse to create one");
    }
    az_ui_draw_list_scrollbar(canvas, model->selection, model->count);
    az_ui_draw_footer(canvas, "", "OK", "");
}


bool AzSavedScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(event->key == InputKeyUp) {
        az_ui_move_selection(app, -1, app->saved_count);
    } else if(event->key == InputKeyDown) {
        az_ui_move_selection(app, 1, app->saved_count);
    } else if(short_press && event->key == InputKeyOk) {
        az_ui_open_current_saved(app);
    }
    return true;

}

void AzSavedScreen::onResume(AmiiboZeroApp* app) const {
    if(!app->saved_entries && !az_ui_refresh_saved_catalog(app)) {
        az_ui_toast(app, "Saved catalog unavailable");
        return;
    }
    if(app->saved_count == 0U) {
        app->selection = 0U;
    } else if(app->selection >= app->saved_count) {
        app->selection = (uint16_t)(app->saved_count - 1U);
    }
}

void AzSavedScreen::onPopped(AmiiboZeroApp* app) const {
    az_ui_clear_saved_catalog(app);
}
