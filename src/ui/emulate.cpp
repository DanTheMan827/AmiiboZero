/** @file emulate.cpp @brief Emulate screen renderer. */
#include "emulate.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzEmulateScreen az_ui_emulate_screen{};

void AzEmulateScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    az_ui_draw_header(canvas, model, "Emulating");
    canvas_set_font(canvas, FontPrimary);
    az_ui_draw_marquee(
        canvas,
        app,
        2,
        21,
        model->figure_name ? furi_string_get_cstr(model->figure_name) : "Amiibo",
        124,
        model->animation,
        true);
    int radius = 4 + ((model->animation / 2) % 2);
    canvas_draw_circle(canvas, 15, 43, radius);
    canvas_draw_circle(canvas, 15, 43, radius + 4);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 29, 42, model->figure_saved ? "Writes autosave" : "Temporary session");
    canvas_draw_str(canvas, 29, 51, "OK: randomize UID");
    az_ui_draw_footer(canvas, "Back", "OK UID", "");
}


bool AzEmulateScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyBack) {
        az_emulation_stop(app);
        az_ui_navigate(app, AzScreenFigure, false);
    } else if(short_press && event->key == InputKeyOk) {
        az_ui_emulation_randomize_uid(app);
    }
    return true;

}
