/** @file status.cpp @brief Status screen renderer. */
#include "status.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzStatusScreen az_ui_status_screen{};

void AzStatusScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    az_ui_draw_header(canvas, model, "Setup & status");
    char text[320];
    snprintf(
        text,
        sizeof(text),
        "%s\n%s\n%s\n%s\nIndex: %s (%lu figures)\nOK reloads keys and forces a background index rebuild.",
        app->keys.valid ? "Keys: ready" : "Keys: missing/invalid",
        storage_file_exists(app->storage, AZ_AMIIBO_JSON) ? "Amiibo JSON: found" : "Amiibo JSON: missing",
        storage_file_exists(app->storage, AZ_GAMES_JSON) ? "Games JSON: found" : "Games JSON: missing",
        "Index identity: size + samples",
        app->index_ready ? "ready" : "unavailable",
        (unsigned long)app->index_count);
    az_ui_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
    az_ui_draw_footer(canvas, "Back", "OK", "");
}


bool AzStatusScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(event->key == InputKeyUp) {
        if(app->detail_scroll > 0U) app->detail_scroll--;
    } else if(event->key == InputKeyDown) {
        if(app->detail_scroll < UINT16_MAX) app->detail_scroll++;
    } else if(short_press && event->key == InputKeyOk) {
        az_ui_status_refresh(app);
    }
    return true;

}
