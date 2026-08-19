/** @file about.cpp @brief About screen renderer. */
#include "about.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzAboutScreen az_ui_about_screen{};

void AzAboutScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    az_ui_draw_header(canvas, model, "About");
    char text[192];
    snprintf(
        text,
        sizeof(text),
        "Amiibo Zero %s\nBrowse, generate, emulate, and persist Amiibo.\nJSON: AmiiboAPI + lwJSON. Crypto: mbedTLS.\nKeys/dumps are never bundled.",
        AZ_APP_VERSION);
    az_ui_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
    az_ui_draw_footer(canvas, "Back", "Up/Dn", "");
}


bool AzAboutScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    if(event->key == InputKeyUp) {
        if(app->detail_scroll > 0U) app->detail_scroll--;
    } else if(event->key == InputKeyDown) {
        if(app->detail_scroll < UINT16_MAX) app->detail_scroll++;
    }
    return true;

}
