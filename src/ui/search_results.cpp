/** @file search_results.cpp @brief Search Results screen renderer. */
#include "search_results.h"
#include "../amiibo_db.h"
#include "ui_common.h"
#include "controller.h"

const AzSearchResultsScreen az_ui_search_results_screen{};

void AzSearchResultsScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    az_ui_draw_figure_list(canvas, model, true);
}


bool AzSearchResultsScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(event->key == InputKeyUp) {
        az_ui_move_selection(app, -1, app->list_count);
    } else if(event->key == InputKeyDown) {
        az_ui_move_selection(app, 1, app->list_count);
    } else if(short_press && event->key == InputKeyLeft) {
        az_ui_open_search(app);
    } else if(short_press && event->key == InputKeyOk && app->list_count) {
        AzFigure figure;
        if(az_db_search_get(app->storage, app->query, app->selection, &figure)) {
            app->screen_selection[AzScreenSearchResults] = app->selection;
            app->current_figure = figure;
            furi_string_set_str(app->current_figure_name, figure.name[0] ? figure.name : "Amiibo");
            app->current_is_saved = false;
            app->current_saved_filename[0] = '\0';
            app->current_lockon_valid = false;
            app->current_lockon_filename[0] = '\0';
            app->screen_selection[AzScreenFigure] = 0U;
            az_ui_navigate(app, AzScreenFigure, true);
        }
    }
    return true;

}
