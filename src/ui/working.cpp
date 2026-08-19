/** @file working.cpp @brief Working screen renderer. */
#include "working.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

/** Return the short user-visible label for a database preparation stage. */
static const char* az_database_progress_label(AzDbProgressStage stage) {
    switch(stage) {
    case AzDbProgressChecking: return "Checking source files";
    case AzDbProgressAmiibo: return "Reading amiibo.json";
    case AzDbProgressSorting: return "Sorting figures";
    case AzDbProgressGames: return "Reading games_info.json";
    case AzDbProgressFinalizing: return "Finalizing index";
    case AzDbProgressDone: return "Database ready";
    default: return "Preparing database";
    }
}

const AzWorkingScreen az_ui_working_screen{};

void AzWorkingScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    az_ui_draw_header(canvas, model, "Preparing database");
    uint8_t phase = (model->animation / 2U) & 1U;

    canvas_draw_frame(canvas, 7, 17, 14, 16);
    canvas_draw_line(canvas, 9, 19, 19, 19);
    canvas_draw_line(canvas, 9, 31, 19, 31);
    if(phase == 0U) {
        canvas_draw_line(canvas, 10, 21, 18, 21);
        canvas_draw_line(canvas, 12, 23, 16, 23);
    } else {
        canvas_draw_line(canvas, 12, 27, 16, 27);
        canvas_draw_line(canvas, 10, 29, 18, 29);
    }

    canvas_set_font(canvas, FontSecondary);
    az_ui_draw_fitted(
        canvas,
        model->app,
        26,
        28,
        az_database_progress_label(model->db_progress_stage),
        98);

    canvas_draw_frame(canvas, 13, 38, 102, 8);
    uint8_t progress = model->db_progress > 100U ? 100U : model->db_progress;
    if(progress) canvas_draw_box(canvas, 14, 39, progress, 6);

    char percent[8];
    snprintf(percent, sizeof(percent), "%u%%", progress);
    canvas_draw_str_aligned(canvas, 64, 53, AlignCenter, AlignBottom, percent);
}


bool AzWorkingScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    UNUSED(app);
    UNUSED(event);
    return true;

}

bool AzWorkingScreen::backRequested(AmiiboZeroApp* app, const InputEvent* event) const {
    UNUSED(event);
    return app && app->db_thread == NULL;
}
