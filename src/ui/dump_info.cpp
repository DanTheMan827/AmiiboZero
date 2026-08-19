/** @file dump_info.cpp @brief Dump Info screen renderer. */
#include "dump_info.h"
#include "ui_common.h"
#include "controller.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

const AzDumpInfoScreen az_ui_dump_info_screen{};

void AzDumpInfoScreen::draw(Canvas* canvas, const AzViewModel* model) const {
    AmiiboZeroApp* app = model->app;
    const char* figure_name = model->figure_name ? furi_string_get_cstr(model->figure_name) : "";
    az_ui_draw_header(canvas, model, figure_name[0] ? figure_name : "Dump details");
    char text[640];
    const AzAmiiboDetails* details = &app->current_details;
    bool v3 = az_figure_is_v3(model->figure.id);
    char id_hex[17];
    az_ui_format_figure_id(&model->figure, id_hex);
    const char* lockon_line = v3 ? (app->current_lockon_valid ? "\nLock-on: attached" : "\nLock-on: missing") : "";
    if(!details->available) {
        snprintf(
            text,
            sizeof(text),
            "Type: %s%s\nID: %s\nFile: %s%s\nEncrypted details unavailable. A valid key_retail.bin is required and the dump must authenticate.",
            az_figure_type_name(model->figure.id[3]),
            v3 ? " / Lock-on" : "",
            id_hex,
            model->saved_filename,
            lockon_line);
    } else {
        unsigned long app_hi = (unsigned long)(details->application_id >> 32);
        unsigned long app_lo = (unsigned long)(details->application_id & 0xFFFFFFFFULL);
        snprintf(
            text,
            sizeof(text),
            "Type: %s%s\nID: %s\nFile: %s%s\nNickname: %s\nOwner Mii: %s\nInitialized: %s\nApp data: %s\nRegistered: %s\nLast write: %s\nWrite count: %u\nApplication: %08lX%08lX\nArea ID: %08lX\nApp writes: %u",
            az_figure_type_name(model->figure.id[3]),
            v3 ? " / Lock-on" : "",
            id_hex,
            model->saved_filename,
            lockon_line,
            details->nickname[0] ? details->nickname : "(none)",
            details->owner_mii[0] ? details->owner_mii : "(none)",
            details->initialized ? "yes" : "no",
            details->app_data_initialized ? "yes" : "no",
            details->init_date[0] ? details->init_date : "-",
            details->write_date[0] ? details->write_date : "-",
            details->write_counter,
            app_hi,
            app_lo,
            (unsigned long)details->application_area_id,
            details->application_write_counter);
    }
    az_ui_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
    az_ui_draw_footer(canvas, "Back", "Up/Dn", "");
}


bool AzDumpInfoScreen::input(AmiiboZeroApp* app, const InputEvent* event) const {

    const bool short_press = event->type == InputTypeShort;
    if(short_press && event->key == InputKeyBack) {
        az_ui_navigate(app, AzScreenFigure, false);
    } else if(event->key == InputKeyUp) {
        if(app->detail_scroll > 0U) app->detail_scroll--;
    } else if(event->key == InputKeyDown) {
        if(app->detail_scroll < UINT16_MAX) app->detail_scroll++;
    }
    return true;

}
