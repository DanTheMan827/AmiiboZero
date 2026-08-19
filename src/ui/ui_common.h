/** @file ui_common.h @brief Shared C++ UI drawing declarations. */
#pragma once

#include "../amiibo_zero.h"
#include <gui/canvas.h>

#define AZ_UI_CONTENT_TOP 12
#define AZ_UI_FOOTER_TOP 54
#define AZ_UI_LIST_WIDTH 116
#define AZ_UI_DETAIL_LINES 4
#define AZ_UI_DETAIL_LINE_HEIGHT 9

/** Format one figure ID as 16 lowercase hexadecimal digits. */
void az_ui_format_figure_id(const AzFigure* figure, char out[17]);
/** Return the selected figure display name or an empty string. */
const char* az_ui_current_figure_name(const AmiiboZeroApp* app);
/** Draw the inverse header, including temporary toast text when present. */
void az_ui_draw_header(Canvas* canvas, const AzViewModel* model, const char* title);
/** Draw optional left/center/right footer labels. */
void az_ui_draw_footer(Canvas* canvas, const char* left, const char* center, const char* right);
/** Draw one line fitted to the requested pixel width. */
void az_ui_draw_fitted(Canvas* canvas, AmiiboZeroApp* app, int x, int y, const char* text, size_t width);
/** Draw one horizontally scrollable text line. */
void az_ui_draw_marquee(Canvas* canvas, AmiiboZeroApp* app, int x, int y, const char* text, size_t width, uint8_t animation, bool selected);
/** Draw one normal or selected list row. */
void az_ui_draw_list_row(Canvas* canvas, const AzViewModel* model, uint8_t row, const char* text, bool selected);
/** Draw the standard list scrollbar when required. */
void az_ui_draw_list_scrollbar(Canvas* canvas, uint16_t selection, uint16_t total);
/** Return the number of actions available for a figure state. */
uint8_t az_ui_figure_action_count(bool saved, bool v3);
/** Return the label for one figure action. */
const char* az_ui_figure_action(bool saved, bool v3, uint8_t index);
/** Draw vertically scrollable wrapped detail text. */
void az_ui_draw_wrapped_text(Canvas* canvas, AmiiboZeroApp* app, const char* text, int y, uint8_t visible_lines, size_t width);

/** Shared renderer used by the Figures and Search Results screen wrappers. */
void az_ui_draw_figure_list(Canvas* canvas, const AzViewModel* model, bool search_results);
