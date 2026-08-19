/** @file home.cpp @brief Home screen renderer. */
#include "home.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzHomeScreen az_ui_home_screen{};

void AzHomeScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    static const char* labels[] = {"Browse library", "Saved figures", "Advanced", "Setup & status", "About"};
    az_ui_draw_header(canvas, model, AZ_APP_NAME);
    uint8_t start = model->selection >= 2 ? (uint8_t)(model->selection - 2) : 0;
    if((size_t)start + AZ_LIST_ROWS > COUNT_OF(labels)) start = (uint8_t)(COUNT_OF(labels) - AZ_LIST_ROWS);
    for(uint8_t row = 0; row < AZ_LIST_ROWS; row++) {
        uint8_t index = (uint8_t)(start + row);
        az_ui_draw_list_row(canvas, model, row, labels[index], model->selection == index);
    }
    az_ui_draw_list_scrollbar(canvas, model->selection, COUNT_OF(labels));
    az_ui_draw_footer(canvas, "", "OK", "");
}


bool AzHomeScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyBack) {
        view_dispatcher_stop(app->dispatcher);
        return true;
    }
    if(event->key == InputKeyUp) az_ui_move_selection(app, -1, 5U);
    else if(event->key == InputKeyDown) az_ui_move_selection(app, 1, 5U);
    else if(short_press && event->key == InputKeyOk) {
        app->screen_selection[AzScreenHome] = app->selection;
        if(app->selection == 0U) {
            az_ui_navigate(app, AzScreenCategories, false);
        } else if(app->selection == 1U) {
            if(!az_ui_refresh_saved_catalog(app)) {
                az_ui_toast(app, "Saved catalog unavailable");
                return true;
            }
            if(app->screen_selection[AzScreenSaved] >= app->saved_count && app->saved_count) {
                app->screen_selection[AzScreenSaved] = (uint16_t)(app->saved_count - 1U);
            }
            az_ui_navigate(app, AzScreenSaved, false);
        } else if(app->selection == 2U) {
            az_ui_navigate(app, AzScreenAdvanced, false);
        } else if(app->selection == 3U) {
            az_ui_navigate(app, AzScreenStatus, false);
        } else if(app->selection == 4U) {
            az_ui_navigate(app, AzScreenAbout, false);
        }
    }
    return true;

}
