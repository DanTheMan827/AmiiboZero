/** @file categories.cpp @brief Categories screen renderer. */
#include "categories.h"
#include "../amiibo_db.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzCategoriesScreen az_ui_categories_screen{};

void AzCategoriesScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    az_ui_draw_header(canvas, model, "Library categories");
    for(uint8_t i = 0; i < model->row_count; i++) {
        uint16_t absolute = (uint16_t)(model->window_start + i);
        const char* name = model->category_rows[i].name;
        az_ui_draw_list_row(canvas, model, i, name, absolute == model->selection);
    }
    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "No categories");
    }
    az_ui_draw_list_scrollbar(canvas, model->selection, model->count);
    az_ui_draw_footer(canvas, "< Search", "OK", "");
}


bool AzCategoriesScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(event->key == InputKeyUp) {
        az_ui_move_selection(app, -1, app->list_count);
    } else if(event->key == InputKeyDown) {
        az_ui_move_selection(app, 1, app->list_count);
    } else if(short_press && event->key == InputKeyLeft) {
        az_ui_open_search(app);
    } else if(short_press && event->key == InputKeyOk && app->list_count) {
        if(az_db_get_category(app->storage, app->selection, &app->current_category)) {
            app->screen_selection[AzScreenCategories] = app->selection;
            app->screen_selection[AzScreenFigures] = 0U;
            az_ui_navigate(app, AzScreenFigures, true);
        }
    }
    return true;

}
