/** @file advanced.cpp @brief Advanced screen renderer. */
#include "advanced.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzAdvancedScreen az_ui_advanced_screen{};

void AzAdvancedScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    static const char* labels[] = {"Manual figure ID", "Read & save Amiibo", "Clear tag user data"};
    az_ui_draw_header(canvas, model, "Advanced");
    for(uint8_t row = 0; row < COUNT_OF(labels); row++) {
        az_ui_draw_list_row(canvas, model, row, labels[row], model->selection == row);
    }
    az_ui_draw_footer(canvas, "Back", "OK", "");
}


bool AzAdvancedScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyBack) {
        az_ui_navigate(app, AzScreenHome, false);
    } else if(event->key == InputKeyUp) {
        az_ui_move_selection(app, -1, 3U);
    } else if(event->key == InputKeyDown) {
        az_ui_move_selection(app, 1, 3U);
    } else if(short_press && event->key == InputKeyOk) {
        app->screen_selection[AzScreenAdvanced] = app->selection;
        if(app->selection == 0U) az_ui_open_manual_id(app);
        else if(app->selection == 1U) az_ui_open_tag_read_save(app);
        else if(app->selection == 2U) az_ui_open_tag_clear(app);
    }
    return true;

}
