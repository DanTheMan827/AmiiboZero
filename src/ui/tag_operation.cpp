/** @file tag_operation.cpp @brief Tag Operation screen renderer. */
#include "tag_operation.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

/** Convert a physical-tag result to compact user-facing text. */
static const char* az_tag_result_text(AzTagResult result) {
    switch(result) {
    case AzTagResultSuccess: return "Completed successfully";
    case AzTagResultWrongTag: return "Rejected: not NTAG215";
    case AzTagResultNotBlank: return "Rejected: tag not blank";
    case AzTagResultNotAmiibo: return "Not a valid Amiibo";
    case AzTagResultUidChanged: return "Tag changed during operation";
    case AzTagResultAuthFailed: return "Tag authentication failed";
    case AzTagResultReadFailed: return "Tag read failed";
    case AzTagResultCryptoFailed: return "Amiibo crypto failed";
    case AzTagResultWriteFailed: return "Tag write failed";
    case AzTagResultSaveFailed: return "Tag save failed";
    default: return "Working...";
    }
}

const AzTagOperationScreen az_ui_tag_operation_screen{};

void AzTagOperationScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    const char* title = "NTAG215";
    if(app->tag_operation == AzTagOperationWrite) title = "Write v2 Amiibo";
    else if(app->tag_operation == AzTagOperationReadSave) title = "Read & save Amiibo";
    else if(app->tag_operation == AzTagOperationClear) title = "Clear user data";
    az_ui_draw_header(canvas, model, title);
    canvas_set_font(canvas, FontSecondary);

    if(app->tag_stage == AzTagStageDone) {
        const char* result = az_tag_result_text(app->tag_result);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, result);
        if(app->tag_result == AzTagResultSuccess && app->tag_operation == AzTagOperationReadSave &&
           app->tag_saved_filename[0]) {
            az_ui_draw_marquee(canvas, app, 4, 42, app->tag_saved_filename, 120, model->animation, true);
        } else if(app->tag_result == AzTagResultSuccess && app->tag_operation == AzTagOperationWrite) {
            canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, "Locks written last");
        } else if(app->tag_result == AzTagResultSuccess && app->tag_operation == AzTagOperationClear) {
            canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignCenter, "Writable state reset");
        }
        az_ui_draw_footer(canvas, "Back", "OK", "");
        return;
    }

    const char* line1 = "Hold tag to Flipper";
    const char* line2 = "";
    if(app->tag_stage == AzTagStageScanBlank) {
        line1 = "Hold blank NTAG215";
        line2 = "Validating type + locks";
    } else if(app->tag_stage == AzTagStageReading) {
        line1 = "Hold Amiibo to Flipper";
        line2 = app->tag_operation == AzTagOperationClear ? "Read + authenticate" : "Reading NTAG215";
    } else if(app->tag_stage == AzTagStagePreparing) {
        line1 = "Preparing UID-bound dump";
        line2 = "Keep tag nearby";
    } else if(app->tag_stage == AzTagStageWriting) {
        line1 = "Keep tag in place";
        line2 = "Writing blank NTAG215";
    } else if(app->tag_stage == AzTagStageClearing) {
        line1 = "Keep tag in place";
        line2 = "Resetting user pages";
    }
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, line1);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, line2);
    if(app->tag_stage == AzTagStageWriting || app->tag_stage == AzTagStageClearing) {
        char progress[20];
        snprintf(progress, sizeof(progress), "%u%%", app->tag_progress);
        canvas_draw_frame(canvas, 15, 43, 98, 7);
        uint8_t width = (uint8_t)((94U * app->tag_progress) / 100U);
        if(width) canvas_draw_box(canvas, 17, 45, width, 3);
        canvas_draw_str_aligned(canvas, 119, 51, AlignRight, AlignBottom, progress);
    }
    az_ui_draw_footer(canvas, "Back", "", "");
}


bool AzTagOperationScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyOk && app->tag_stage == AzTagStageDone) {
        az_ui_pop(app);
    }
    return true;

}

void AzTagOperationScreen::onPopped(AmiiboZeroApp* app) const {
    /* Release only the operation poller/transient buffers, not the app-owned NFC service. */
    az_tag_operation_cancel(app);
}
