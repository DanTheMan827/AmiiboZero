/** @file ui_common.cpp @brief Shared C++ drawing helpers for Amiibo Zero screens. */
#include "ui_common.h"

#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

/** Select toast text or the fallback title for the header. */
static const char* az_header_title(const AzViewModel* model, const char* fallback) {
    return model->status_line[0] ? model->status_line : fallback;
}

/** Reuse the app-owned FuriString for allocation-free drawing helpers. */
static void az_scratch_set(AmiiboZeroApp* app, const char* text) {
    if(!app || !app->ui_scratch) return;
    furi_string_set_str(app->ui_scratch, text ? text : "");
}

void az_ui_format_figure_id(const AzFigure* figure, char out[17]) {
    static const char hex[] = "0123456789abcdef";
    if(!out) return;
    if(!figure) { out[0] = '\0'; return; }
    for(size_t i = 0U; i < 8U; i++) {
        out[i * 2U] = hex[figure->id[i] >> 4];
        out[i * 2U + 1U] = hex[figure->id[i] & 0x0FU];
    }
    out[16] = '\0';
}

const char* az_ui_current_figure_name(const AmiiboZeroApp* app) {
    return app && app->current_figure_name ? furi_string_get_cstr(app->current_figure_name) : "";
}

void az_ui_draw_header(Canvas* canvas, const AzViewModel* model, const char* title) {
    AmiiboZeroApp* app = model->app;
    title = az_header_title(model, title);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 11);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    az_scratch_set(app, title);
    if(canvas_string_width(canvas, furi_string_get_cstr(app->ui_scratch)) <= 122) {
        canvas_draw_str(canvas, 3, 9, furi_string_get_cstr(app->ui_scratch));
    } else {
        elements_scrollable_text_line(canvas, 3, 9, 122, app->ui_scratch, model->animation, false);
    }
    canvas_set_color(canvas, ColorBlack);
}

void az_ui_draw_footer(Canvas* canvas, const char* left, const char* center, const char* right) {
    canvas_set_font(canvas, FontSecondary);
    if(left && left[0]) canvas_draw_str_aligned(canvas, 1, 63, AlignLeft, AlignBottom, left);
    if(center && center[0]) canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, center);
    if(right && right[0]) canvas_draw_str_aligned(canvas, 127, 63, AlignRight, AlignBottom, right);
}

void az_ui_draw_fitted(Canvas* canvas, AmiiboZeroApp* app, int x, int y, const char* text, size_t width) {
    az_scratch_set(app, text);
    elements_string_fit_width(canvas, app->ui_scratch, width);
    canvas_draw_str(canvas, x, y, furi_string_get_cstr(app->ui_scratch));
}

void az_ui_draw_marquee(
    Canvas* canvas,
    AmiiboZeroApp* app,
    int x,
    int y,
    const char* text,
    size_t width,
    uint8_t animation,
    bool ellipsis) {
    az_scratch_set(app, text);
    elements_scrollable_text_line(canvas, x, y, width, app->ui_scratch, animation, ellipsis);
}

void az_ui_draw_list_row(
    Canvas* canvas,
    const AzViewModel* model,
    uint8_t row,
    const char* text,
    bool selected) {
    AmiiboZeroApp* app = model->app;
    int top = AZ_UI_CONTENT_TOP + row * 10;
    int baseline = top + 8;
    canvas_set_font(canvas, FontSecondary);
    if(selected) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, 1, top, 122, 10, 2);
        canvas_set_color(canvas, ColorWhite);
        az_ui_draw_marquee(canvas, app, 4, baseline, text, AZ_UI_LIST_WIDTH, model->animation, false);
        canvas_set_color(canvas, ColorBlack);
    } else {
        az_ui_draw_fitted(canvas, app, 4, baseline, text, AZ_UI_LIST_WIDTH);
    }
}

void az_ui_draw_list_scrollbar(Canvas* canvas, uint16_t selection, uint16_t total) {
    if(total > AZ_LIST_ROWS) elements_scrollbar_pos(canvas, 125, AZ_UI_CONTENT_TOP, 40, selection, total);
}

uint8_t az_ui_figure_action_count(bool saved, bool v3) {
    if(saved) return 7U;
    return v3 ? 3U : 4U;
}

const char* az_ui_figure_action(bool saved, bool v3, uint8_t index) {
    if(saved) {
        static const char* standard_actions[] = {
            "Emulate + autosave",
            "Dump details",
            "Compatibility",
            "Write to blank tag",
            "Rename",
            "Fresh copy",
            "Delete",
        };
        static const char* v3_actions[] = {
            "Emulate + autosave",
            "Dump details",
            "Compatibility",
            "Change lock-on",
            "Rename",
            "Fresh copy",
            "Delete",
        };
        if(v3) return index < COUNT_OF(v3_actions) ? v3_actions[index] : "";
        return index < COUNT_OF(standard_actions) ? standard_actions[index] : "";
    }
    if(v3) {
        static const char* v3_actions[] = {"Emulate once", "Save + emulate", "Compatibility"};
        return index < COUNT_OF(v3_actions) ? v3_actions[index] : "";
    }
    static const char* actions[] = {
        "Emulate once", "Save + emulate", "Write to blank tag", "Compatibility"};
    return index < COUNT_OF(actions) ? actions[index] : "";
}

/** Return the encoded byte length implied by a UTF-8 leading byte. */
static size_t az_utf8_char_len(unsigned char value) {
    if((value & 0x80U) == 0) return 1;
    if((value & 0xE0U) == 0xC0U) return 2;
    if((value & 0xF0U) == 0xE0U) return 3;
    if((value & 0xF8U) == 0xF0U) return 4;
    return 1;
}

/** Find the next pixel-bounded wrapped line without splitting UTF-8 code units. */
static bool az_wrap_next_line(
    Canvas* canvas,
    const char** cursor,
    size_t width,
    char* out,
    size_t out_size) {
    const char* p = *cursor;
    if(!p || !*p || out_size < 2) return false;

    if(*p == '\n') {
        out[0] = 0;
        *cursor = p + 1;
        return true;
    }
    while(*p == ' ' || *p == '\t' || *p == '\r') p++;
    if(!*p) {
        *cursor = p;
        return false;
    }

    size_t len = 0;
    size_t last_space_len = 0;
    const char* last_space_next = NULL;
    const char* scan = p;
    const char* next = p;
    while(*scan && *scan != '\n') {
        if(*scan == ' ' || *scan == '\t') {
            last_space_len = len;
            last_space_next = scan + 1;
        }
        size_t cp = az_utf8_char_len((unsigned char)*scan);
        size_t available = strlen(scan);
        if(cp > available) cp = 1;
        if(len + cp + 1 >= out_size) break;
        memcpy(out + len, scan, cp);
        size_t candidate = len + cp;
        out[candidate] = 0;
        if(canvas_string_width(canvas, out) > width) {
            if(last_space_next && last_space_len > 0) {
                len = last_space_len;
                next = last_space_next;
            } else if(len > 0) {
                next = scan;
            } else {
                len = candidate;
                next = scan + cp;
            }
            break;
        }
        len = candidate;
        scan += cp;
        next = scan;
    }

    if(*scan == '\n' && next == scan) next = scan + 1;
    while(len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) len--;
    out[len] = 0;
    while(*next == ' ' || *next == '\t' || *next == '\r') next++;
    *cursor = next;
    return true;
}

/** Count wrapped detail lines for scrollbar and vertical-scroll bounds. */
static uint16_t az_wrapped_line_count(Canvas* canvas, const char* text, size_t width) {
    if(!text || !text[0]) return 0;
    const char* cursor = text;
    char line[96];
    uint16_t count = 0;
    while(az_wrap_next_line(canvas, &cursor, width, line, sizeof(line))) count++;
    return count;
}

void az_ui_draw_wrapped_text(
    Canvas* canvas,
    AmiiboZeroApp* app,
    const char* text,
    int top,
    uint8_t visible_lines,
    size_t width) {
    canvas_set_font(canvas, FontSecondary);
    uint16_t total = az_wrapped_line_count(canvas, text, width);
    uint16_t max_scroll = total > visible_lines ? (uint16_t)(total - visible_lines) : 0;
    if(app->detail_scroll > max_scroll) app->detail_scroll = max_scroll;

    const char* cursor = text;
    char line[96];
    uint16_t index = 0;
    uint8_t drawn = 0;
    while(az_wrap_next_line(canvas, &cursor, width, line, sizeof(line))) {
        if(index >= app->detail_scroll && drawn < visible_lines) {
            canvas_draw_str(canvas, 2, top + drawn * AZ_UI_DETAIL_LINE_HEIGHT, line);
            drawn++;
        }
        index++;
        if(drawn == visible_lines && index >= total) break;
    }
    if(total > visible_lines) {
        elements_scrollbar_pos(canvas, 125, top - 7, visible_lines * AZ_UI_DETAIL_LINE_HEIGHT, app->detail_scroll, max_scroll + 1);
    }
}


void az_ui_draw_figure_list(Canvas* canvas, const AzViewModel* model, bool search_results) {
    az_ui_draw_header(
        canvas,
        model,
        search_results ? "Search results" :
                         (model->category.name[0] ? model->category.name : "Figures"));
    for(uint8_t i = 0; i < model->row_count; i++) {
        uint16_t absolute = (uint16_t)(model->window_start + i);
        az_ui_draw_list_row(
            canvas,
            model,
            i,
            model->figure_row_names[i] ? model->figure_row_names[i] : "",
            absolute == model->selection);
    }
    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "No entries");
    }
    az_ui_draw_list_scrollbar(canvas, model->selection, model->count);
    az_ui_draw_footer(canvas, "< Search", "OK", "");
}
