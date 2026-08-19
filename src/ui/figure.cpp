/** @file figure.cpp @brief Figure screen renderer. */
#include "figure.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzFigureScreen az_ui_figure_screen{};

void AzFigureScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    const char* figure_name = model->figure_name ? furi_string_get_cstr(model->figure_name) : "";
    az_ui_draw_header(canvas, model, figure_name[0] ? figure_name : "Amiibo");
    canvas_set_font(canvas, FontSecondary);
    char type_line[48];
    snprintf(
        type_line,
        sizeof(type_line),
        "Type: %s%s",
        az_figure_type_name(model->figure.id[3]),
        az_figure_is_v3(model->figure.id) ? " / Lock-on" : "");
    az_ui_draw_fitted(canvas, app, 2, 20, type_line, 124);
    char id_hex[17];
    az_ui_format_figure_id(&model->figure, id_hex);
    char idline[32];
    snprintf(idline, sizeof(idline), "ID %s", id_hex);
    az_ui_draw_fitted(canvas, app, 2, 29, idline, 124);

    bool v3 = az_figure_is_v3(model->figure.id);
    uint8_t count = az_ui_figure_action_count(model->figure_saved, v3);
    uint8_t start_row = model->selection >= 2 ? (uint8_t)(model->selection - 2) : 0;
    if(count > 3 && start_row + 3 > count) start_row = (uint8_t)(count - 3);
    for(uint8_t row = 0; row < 3 && start_row + row < count; row++) {
        uint8_t index = (uint8_t)(start_row + row);
        int top = 31 + row * 8;
        int baseline = top + 7;
        bool selected = index == model->selection;
        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, 1, top, 124, 8, 2);
            canvas_set_color(canvas, ColorWhite);
            az_ui_draw_marquee(
                canvas,
                app,
                4,
                baseline,
                az_ui_figure_action(model->figure_saved, v3, index),
                118,
                model->animation,
                false);
            canvas_set_color(canvas, ColorBlack);
        } else {
            az_ui_draw_fitted(
                canvas,
                app,
                4,
                baseline,
                az_ui_figure_action(model->figure_saved, v3, index),
                118);
        }
    }
    if(count > 3) elements_scrollbar_pos(canvas, 125, 31, 24, model->selection, count);
}


bool AzFigureScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyBack) {
        if(app->current_is_saved) {
            if(!az_ui_refresh_saved_catalog(app)) {
                az_ui_toast(app, "Saved catalog unavailable");
                az_ui_navigate(app, AzScreenHome, false);
                return true;
            }
            app->screen_selection[AzScreenSaved] = az_ui_saved_selection_for_filename(
                app, app->current_saved_filename, app->screen_selection[AzScreenSaved]);
            az_ui_navigate(app, AzScreenSaved, false);
        } else {
            app->screen_selection[app->return_screen] = app->return_selection;
            az_ui_navigate(app, app->return_screen, false);
        }
        return true;
    }

    const bool v3 = az_figure_is_v3(app->current_figure.id);
    const uint8_t action_count = az_ui_figure_action_count(app->current_is_saved, v3);
    if(event->key == InputKeyUp) az_ui_move_selection(app, -1, action_count);
    else if(event->key == InputKeyDown) az_ui_move_selection(app, 1, action_count);
    else if(short_press && event->key == InputKeyOk) {
        if(app->current_is_saved) {
            if(app->selection == 0U) az_ui_request_emulation(app, true, false);
            else if(app->selection == 1U) az_ui_navigate(app, AzScreenDumpInfo, true);
            else if(app->selection == 2U) az_ui_open_games(app);
            else if(v3 && app->selection == 3U) az_ui_open_lockon_selector(app, AzLockOnActionReplaceSaved);
            else if(!v3 && app->selection == 3U) az_ui_open_tag_write(app);
            else if(app->selection == 4U) az_ui_open_rename(app);
            else if(app->selection == 5U) az_ui_request_emulation(app, true, true);
            else if(app->selection == 6U) az_ui_navigate(app, AzScreenConfirmDelete, true);
        } else {
            if(app->selection == 0U) az_ui_request_emulation(app, false, true);
            else if(app->selection == 1U) az_ui_request_emulation(app, true, true);
            else if(!v3 && app->selection == 2U) az_ui_open_tag_write(app);
            else if((v3 && app->selection == 2U) || (!v3 && app->selection == 3U)) az_ui_open_games(app);
        }
    }
    return true;

}
