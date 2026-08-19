/** @file figures.cpp @brief Figures screen renderer. */
#include "figures.h"
#include "../amiibo_db.h"
#include "ui_common.h"
#include "controller.h"

const AzFiguresScreen az_ui_figures_screen{};

void AzFiguresScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    az_ui_draw_figure_list(canvas, model, false);
}


bool AzFiguresScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyBack) {
        az_ui_navigate(app, AzScreenCategories, false);
    } else if(event->key == InputKeyUp) {
        az_ui_move_selection(app, -1, app->list_count);
    } else if(event->key == InputKeyDown) {
        az_ui_move_selection(app, 1, app->list_count);
    } else if(short_press && event->key == InputKeyLeft) {
        az_ui_open_search(app);
    } else if(short_press && event->key == InputKeyOk && app->list_count) {
        AzFigure figure;
        if(az_db_get_figure(app->storage, app->current_category.id, app->selection, &figure)) {
            app->return_screen = AzScreenFigures;
            app->return_selection = app->selection;
            app->screen_selection[AzScreenFigures] = app->selection;
            app->current_figure = figure;
            furi_string_set_str(app->current_figure_name, figure.name[0] ? figure.name : "Amiibo");
            app->current_is_saved = false;
            app->current_saved_filename[0] = '\0';
            app->current_lockon_valid = false;
            app->current_lockon_filename[0] = '\0';
            app->screen_selection[AzScreenFigure] = 0U;
            app->selection = 0U;
            az_ui_show(app, AzScreenFigure);
        }
    }
    return true;

}
