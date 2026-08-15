/**
 * @file ui_controls.cpp
 * @brief Reusable header, footer, list, marquee, and wrapped-text controls.
 */

#include "ui_controls.h"
#include "ui_manager.h"

#include <gui/elements.h>
#include <string.h>

namespace {
/**
 * @brief Return the byte width of a UTF-8 code point from its lead byte.
 * @param value Candidate lead byte.
 * @return Encoded byte length from one through four.
 */
size_t utf8CharLen(unsigned char value) {
    if((value & 0x80U) == 0) return 1;
    if((value & 0xE0U) == 0xC0U) return 2;
    if((value & 0xF0U) == 0xE0U) return 3;
    if((value & 0xF8U) == 0xF0U) return 4;
    return 1;
}

/**
 * @brief Extract the next display-width-bounded wrapped line from UTF-8 text.
 * @param canvas Canvas used for pixel-width measurement.
 * @param cursor In/out cursor into the source text.
 * @param width Maximum line width in pixels.
 * @param out Destination line buffer.
 * @param out_size Capacity of out including its terminator.
 * @return true when one logical/wrapped line was produced.
 */
bool wrapNextLine(Canvas* canvas, const char** cursor, size_t width, char* out, size_t out_size) {
    const char* p = *cursor;
    if(!p || !*p || out_size < 2) return false;

    if(*p == '\n') {
        out[0] = '\0';
        *cursor = p + 1;
        return true;
    }
    while(*p == ' ' || *p == '\t' || *p == '\r') ++p;
    if(!*p) {
        *cursor = p;
        return false;
    }

    size_t len = 0;
    size_t last_space_len = 0;
    const char* last_space_next = nullptr;
    const char* scan = p;
    const char* next = p;
    while(*scan && *scan != '\n') {
        if(*scan == ' ' || *scan == '\t') {
            last_space_len = len;
            last_space_next = scan + 1;
        }
        size_t cp = utf8CharLen(static_cast<unsigned char>(*scan));
        const size_t available = strlen(scan);
        if(cp > available) cp = 1;
        if(len + cp + 1 >= out_size) break;
        memcpy(out + len, scan, cp);
        const size_t candidate = len + cp;
        out[candidate] = '\0';
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
    while(len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) --len;
    out[len] = '\0';
    while(*next == ' ' || *next == '\t' || *next == '\r') ++next;
    *cursor = next;
    return true;
}

/**
 * @brief Count wrapped display lines without retaining them.
 * @param canvas Canvas used for pixel-width measurement.
 * @param text Source text.
 * @param width Maximum line width in pixels.
 * @return Number of wrapped lines.
 */
uint16_t wrappedLineCount(Canvas* canvas, const char* text, size_t width) {
    if(!text || !text[0]) return 0;
    const char* cursor = text;
    char line[96];
    uint16_t count = 0;
    while(wrapNextLine(canvas, &cursor, width, line, sizeof(line))) ++count;
    return count;
}
} // namespace

void UiControls::header(Canvas* canvas, UiManager& ui, const char* title) {
    const char* displayed = ui.toastActive() ? ui.toastText() : title;
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 11);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    ui.setScratch(displayed ? displayed : "");
    if(canvas_string_width(canvas, ui.scratchText()) <= 122) {
        canvas_draw_str(canvas, 3, 9, ui.scratchText());
    } else {
        elements_scrollable_text_line(canvas, 3, 9, 122, ui.scratch(), ui.animation(), false);
    }
    canvas_set_color(canvas, ColorBlack);
}

void UiControls::footer(Canvas* canvas, const char* left, const char* center, const char* right) {
    canvas_set_font(canvas, FontSecondary);
    if(left && left[0]) canvas_draw_str_aligned(canvas, 1, 63, AlignLeft, AlignBottom, left);
    if(center && center[0]) canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, center);
    if(right && right[0]) canvas_draw_str_aligned(canvas, 127, 63, AlignRight, AlignBottom, right);
}

void UiControls::fitted(
    Canvas* canvas,
    UiManager& ui,
    int x,
    int y,
    const char* text,
    size_t width) {
    ui.setScratch(text ? text : "");
    elements_string_fit_width(canvas, ui.scratch(), width);
    canvas_draw_str(canvas, x, y, ui.scratchText());
}

void UiControls::marquee(
    Canvas* canvas,
    UiManager& ui,
    int x,
    int y,
    const char* text,
    size_t width,
    bool ellipsis) {
    ui.setScratch(text ? text : "");
    elements_scrollable_text_line(canvas, x, y, width, ui.scratch(), ui.animation(), ellipsis);
}

void UiControls::listRow(
    Canvas* canvas,
    UiManager& ui,
    uint8_t row,
    const char* text,
    bool selected) {
    const int top = ContentTop + row * 10;
    const int baseline = top + 8;
    canvas_set_font(canvas, FontSecondary);
    if(selected) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, 1, top, 122, 10, 2);
        canvas_set_color(canvas, ColorWhite);
        marquee(canvas, ui, 4, baseline, text, ListWidth, false);
        canvas_set_color(canvas, ColorBlack);
    } else {
        fitted(canvas, ui, 4, baseline, text, ListWidth);
    }
}

void UiControls::listScrollbar(Canvas* canvas, uint16_t selection, uint16_t total) {
    if(total > AZ_LIST_ROWS) {
        elements_scrollbar_pos(canvas, 125, ContentTop, 40, selection, total);
    }
}

void UiControls::wrappedText(
    Canvas* canvas,
    const char* text,
    int top,
    uint8_t visible_lines,
    size_t width,
    uint16_t& scroll) {
    canvas_set_font(canvas, FontSecondary);
    const uint16_t total = wrappedLineCount(canvas, text, width);
    const uint16_t max_scroll = total > visible_lines ? static_cast<uint16_t>(total - visible_lines) : 0;
    if(scroll > max_scroll) scroll = max_scroll;

    const char* cursor = text;
    char line[96];
    uint16_t index = 0;
    uint8_t drawn = 0;
    while(wrapNextLine(canvas, &cursor, width, line, sizeof(line))) {
        if(index >= scroll && drawn < visible_lines) {
            canvas_draw_str(canvas, 2, top + drawn * DetailLineHeight, line);
            ++drawn;
        }
        ++index;
        if(drawn == visible_lines && index >= total) break;
    }
    if(total > visible_lines) {
        elements_scrollbar_pos(
            canvas,
            125,
            top - 7,
            visible_lines * DetailLineHeight,
            scroll,
            max_scroll + 1);
    }
}
