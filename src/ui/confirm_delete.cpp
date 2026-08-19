/** @file confirm_delete.cpp @brief Confirm Delete screen renderer. */
#include "confirm_delete.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzConfirmDeleteScreen az_ui_confirm_delete_screen{};

void AzConfirmDeleteScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    az_ui_draw_header(canvas, model, "Delete saved figure?");
    canvas_set_font(canvas, FontPrimary);
    az_ui_draw_marquee(
        canvas,
        app,
        2,
        24,
        model->figure_name ? furi_string_get_cstr(model->figure_name) : "Amiibo",
        124,
        model->animation,
        true);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "This cannot be undone.");
    az_ui_draw_footer(canvas, "Back", "OK Del", "");
}


bool AzConfirmDeleteScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyOk) {
        uint16_t saved_selection = app->screen_selection[AzScreenSaved];
        if(az_saved_delete(app->storage, app->current_saved_filename)) {
            const bool catalog_ok = az_ui_refresh_saved_catalog(app);
            if(catalog_ok && saved_selection >= app->saved_count && app->saved_count) {
                saved_selection = (uint16_t)(app->saved_count - 1U);
            }
            app->screen_selection[AzScreenSaved] = saved_selection;

            /* Remove confirmation and figure details; the stack restores their actual caller. */
            az_ui_pop(app);
            az_ui_pop(app);
            if(app->screen == AzScreenSaved && catalog_ok) {
                app->selection = saved_selection;
                az_ui_refresh(app);
            }
            az_ui_toast(app, catalog_ok ? "Figure deleted" : "Deleted; catalog refresh failed");
        } else {
            az_ui_toast(app, "Delete failed");
        }
    }
    return true;

}
