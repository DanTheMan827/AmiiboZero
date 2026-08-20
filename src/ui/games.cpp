/** @file games.cpp @brief Games screen renderer. */
#include "games.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzGamesScreen az_ui_games_screen{};

void AzGamesScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    char title[48];
    if(model->games_total) {
        snprintf(title, sizeof(title), "Compatibility %u/%u", model->selection + 1, model->games_total);
    } else {
        az_str_copy(title, sizeof(title), "Compatibility");
    }
    az_ui_draw_header(canvas, model, title);

    if(model->games_total == 0) {
        az_ui_draw_wrapped_text(
            canvas,
            app,
            "No compatibility records were found for this Amiibo in games_info.json.",
            20,
            AZ_UI_DETAIL_LINES,
            120);
    } else {
        char detail[AZ_NAME_MAX + AZ_USAGE_MAX + 64];
        snprintf(
            detail,
            sizeof(detail),
            "%s\n%s - %s\n%s",
            model->games[0].name,
            model->games[0].platform,
            model->games[0].writes ? "writes tag data" : "read only",
            model->games[0].usage);
        az_ui_draw_wrapped_text(canvas, app, detail, 20, AZ_UI_DETAIL_LINES, 120);
    }
    az_ui_draw_footer(canvas, "< Prev", "Up/Dn", "Next >");
}


bool AzGamesScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    if(event->key == InputKeyUp) {
        if(app->detail_scroll > 0U) app->detail_scroll--;
    } else if(event->key == InputKeyDown) {
        if(app->detail_scroll < UINT16_MAX) app->detail_scroll++;
    } else if(event->key == InputKeyLeft && app->game_count) {
        az_ui_move_selection(app, -1, app->game_count);
        app->detail_scroll = 0U;
    } else if(event->key == InputKeyRight && app->game_count) {
        az_ui_move_selection(app, 1, app->game_count);
        app->detail_scroll = 0U;
    }
    return true;

}

void AzGamesScreen::onPopped(AmiiboZeroApp* app) const {
    az_ui_clear_games(app);
}
