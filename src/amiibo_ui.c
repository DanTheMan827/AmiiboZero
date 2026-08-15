/**
 * @file amiibo_ui.c
 * @brief User interface rendering and interaction flow.
 * @details Renders application screens, coordinates navigation and input, and
 * connects database, storage, and NFC operations to the UI.
 */

#include "amiibo_ui.h"
#include "amiibo_crypto.h"
#include "amiibo_db.h"
#include "amiibo_nfc.h"
#include "amiibo_storage.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Constant used for view main. */
#define AZ_VIEW_MAIN 0
/** @brief Constant used for view search. */
#define AZ_VIEW_SEARCH 1
/** @brief Constant used for view manual id. */
#define AZ_VIEW_MANUAL_ID 2
/** @brief Constant used for ui content top. */
#define AZ_UI_CONTENT_TOP 12
/** @brief Constant used for ui footer top. */
#define AZ_UI_FOOTER_TOP 54
/** @brief Constant used for ui list width. */
#define AZ_UI_LIST_WIDTH 116
/** @brief Constant used for ui detail lines. */
#define AZ_UI_DETAIL_LINES 4
/** @brief Constant used for ui detail line height. */
#define AZ_UI_DETAIL_LINE_HEIGHT 9

/**
 * @brief Apply submitted search text and open the search results screen.
 * @param context Caller-owned callback context.
 */
static void az_search_done(void *context);
/**
 * @brief Apply a submitted saved-figure rename and refresh the catalog.
 * @param context Caller-owned callback context.
 */
static void az_rename_done(void *context);
/**
 * @brief Resolve a manually entered figure identifier and open its detail
 * screen.
 * @param context Caller-owned callback context.
 */
static void az_manual_id_done(void *context);
/**
 * @brief Reload the saved-figure catalog from storage.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_refresh_saved_catalog(AmiiboZeroApp *app);
/**
 * @brief Release the in-memory saved-figure catalog.
 * @param app Application state.
 */
static void az_clear_saved_catalog(AmiiboZeroApp *app);
/**
 * @brief Reload the Lock-On catalog from storage.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_refresh_lockon_catalog(AmiiboZeroApp *app);
/**
 * @brief Release the in-memory Lock-On catalog.
 * @param app Application state.
 */
static void az_clear_lockon_catalog(AmiiboZeroApp *app);
/**
 * @brief Start emulation immediately or route through Lock-On selection when
 * required.
 * @param app Application state.
 * @param persistent Whether emulation should preserve mutable state.
 * @param fresh Whether to generate a fresh UID before emulation.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_request_emulation(AmiiboZeroApp *app, bool persistent,
                                 bool fresh);
/**
 * @brief Find the list selection corresponding to a saved filename.
 * @param app Application state.
 * @param filename Filename relative to the relevant application data directory.
 * @param fallback Fallback value used when no more specific value is available.
 * @return The matching selection ordinal, or the supplied fallback when the
 * filename is absent.
 */
static uint16_t az_saved_selection_for_filename(const AmiiboZeroApp *app,
                                                const char *filename,
                                                uint16_t fallback);

/**
 * @brief Select the transient status line or a fallback screen title.
 * @param model View model containing the state to render.
 * @param fallback Fallback value used when no more specific value is available.
 * @return Pointer to the selected text, or NULL when no text is available.
 */
static const char *az_header_title(const AzViewModel *model,
                                   const char *fallback) {
  return model->status_line[0] ? model->status_line : fallback;
}

/**
 * @brief Replace the shared UI scratch string with safe text.
 * @param app Application state.
 * @param text Text to process or display.
 */
static void az_scratch_set(AmiiboZeroApp *app, const char *text) {
  if (!app || !app->ui_scratch)
    return;
  furi_string_set_str(app->ui_scratch, text ? text : "");
}

/**
 * @brief Render the common screen header and scrolling title behavior.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 * @param title Preferred screen title.
 */
static void az_draw_header(Canvas *canvas, const AzViewModel *model,
                           const char *title) {
  AmiiboZeroApp *app = model->app;
  title = az_header_title(model, title);
  canvas_set_color(canvas, ColorBlack);
  canvas_draw_box(canvas, 0, 0, 128, 11);
  canvas_set_color(canvas, ColorWhite);
  canvas_set_font(canvas, FontPrimary);
  az_scratch_set(app, title);
  if (canvas_string_width(canvas, furi_string_get_cstr(app->ui_scratch)) <=
      122) {
    canvas_draw_str(canvas, 3, 9, furi_string_get_cstr(app->ui_scratch));
  } else {
    elements_scrollable_text_line(canvas, 3, 9, 122, app->ui_scratch,
                                  model->animation, false);
  }
  canvas_set_color(canvas, ColorBlack);
}

/**
 * @brief Render optional left, center, and right footer labels.
 * @param canvas Canvas to draw on.
 * @param left Left-aligned footer label.
 * @param center Center-aligned footer label.
 * @param right Right-aligned footer label.
 */
static void az_draw_footer(Canvas *canvas, const char *left, const char *center,
                           const char *right) {
  canvas_set_font(canvas, FontSecondary);
  if (left && left[0])
    canvas_draw_str_aligned(canvas, 1, 63, AlignLeft, AlignBottom, left);
  if (center && center[0])
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, center);
  if (right && right[0])
    canvas_draw_str_aligned(canvas, 127, 63, AlignRight, AlignBottom, right);
}

/**
 * @brief Render text after truncating it to a fixed pixel width.
 * @param canvas Canvas to draw on.
 * @param app Application state.
 * @param x Horizontal drawing coordinate.
 * @param y Vertical drawing coordinate.
 * @param text Text to process or display.
 * @param width Available drawing width in pixels.
 */
static void az_draw_fitted(Canvas *canvas, AmiiboZeroApp *app, int x, int y,
                           const char *text, size_t width) {
  az_scratch_set(app, text);
  elements_string_fit_width(canvas, app->ui_scratch, width);
  canvas_draw_str(canvas, x, y, furi_string_get_cstr(app->ui_scratch));
}

/**
 * @brief Render a horizontally scrollable text line.
 * @param canvas Canvas to draw on.
 * @param app Application state.
 * @param x Horizontal drawing coordinate.
 * @param y Vertical drawing coordinate.
 * @param text Text to process or display.
 * @param width Available drawing width in pixels.
 * @param animation Animation tick used for scrolling text.
 * @param ellipsis Whether truncated text should use an ellipsis.
 */
static void az_draw_marquee(Canvas *canvas, AmiiboZeroApp *app, int x, int y,
                            const char *text, size_t width, uint8_t animation,
                            bool ellipsis) {
  az_scratch_set(app, text);
  elements_scrollable_text_line(canvas, x, y, width, app->ui_scratch, animation,
                                ellipsis);
}

/**
 * @brief Render one selectable list row.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 * @param row Visible list-row index.
 * @param text Text to process or display.
 * @param selected Whether the row is currently selected.
 */
static void az_draw_list_row(Canvas *canvas, const AzViewModel *model,
                             uint8_t row, const char *text, bool selected) {
  AmiiboZeroApp *app = model->app;
  int top = AZ_UI_CONTENT_TOP + row * 10;
  int baseline = top + 8;
  canvas_set_font(canvas, FontSecondary);
  if (selected) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rbox(canvas, 1, top, 122, 10, 2);
    canvas_set_color(canvas, ColorWhite);
    az_draw_marquee(canvas, app, 4, baseline, text, AZ_UI_LIST_WIDTH,
                    model->animation, false);
    canvas_set_color(canvas, ColorBlack);
  } else {
    az_draw_fitted(canvas, app, 4, baseline, text, AZ_UI_LIST_WIDTH);
  }
}

/**
 * @brief Render a list scrollbar when the result set exceeds the visible rows.
 * @param canvas Canvas to draw on.
 * @param selection Selected result ordinal.
 * @param total Total number of records or items.
 */
static void az_draw_list_scrollbar(Canvas *canvas, uint16_t selection,
                                   uint16_t total) {
  if (total > AZ_LIST_ROWS)
    elements_scrollbar_pos(canvas, 125, AZ_UI_CONTENT_TOP, 40, selection,
                           total);
}

/**
 * @brief Render the home screen.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_home(Canvas *canvas, const AzViewModel *model) {
  static const char *labels[] = {"Browse library", "Saved figures", "Advanced",
                                 "Setup & status", "About"};
  az_draw_header(canvas, model, AZ_APP_NAME);
  uint8_t start = model->selection >= 2 ? (uint8_t)(model->selection - 2) : 0;
  if ((size_t)start + AZ_LIST_ROWS > COUNT_OF(labels))
    start = (uint8_t)(COUNT_OF(labels) - AZ_LIST_ROWS);
  for (uint8_t row = 0; row < AZ_LIST_ROWS; row++) {
    uint8_t index = (uint8_t)(start + row);
    az_draw_list_row(canvas, model, row, labels[index],
                     model->selection == index);
  }
  az_draw_list_scrollbar(canvas, model->selection, COUNT_OF(labels));
  az_draw_footer(canvas, "", "OK", "");
}

/**
 * @brief Render the advanced-actions screen.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_advanced(Canvas *canvas, const AzViewModel *model) {
  static const char *labels[] = {"Manual figure ID"};
  az_draw_header(canvas, model, "Advanced");
  for (uint8_t row = 0; row < COUNT_OF(labels); row++) {
    az_draw_list_row(canvas, model, row, labels[row], model->selection == row);
  }
  az_draw_footer(canvas, "Back", "OK", "");
}

/**
 * @brief Render the category browser.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_categories(Canvas *canvas, const AzViewModel *model) {
  az_draw_header(canvas, model, "Library categories");
  for (uint8_t i = 0; i < model->row_count; i++) {
    char label[AZ_NAME_MAX + 16];
    snprintf(label, sizeof(label), "%s  (%u)", model->category_rows[i].name,
             model->category_rows[i].count);
    uint16_t absolute = (uint16_t)(model->window_start + i);
    az_draw_list_row(canvas, model, i, label, absolute == model->selection);
  }
  if (model->count == 0) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter,
                            "No categories");
  }
  az_draw_list_scrollbar(canvas, model->selection, model->count);
  az_draw_footer(canvas, "< Search", "OK", "");
}

/**
 * @brief Render figure or search-result rows.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 * @param search_results Whether the rows represent search results instead of a
 * category listing.
 */
static void az_draw_figures(Canvas *canvas, const AzViewModel *model,
                            bool search_results) {
  az_draw_header(canvas, model,
                 search_results ? "Search results" : model->category.name);
  for (uint8_t i = 0; i < model->row_count; i++) {
    uint16_t absolute = (uint16_t)(model->window_start + i);
    az_draw_list_row(canvas, model, i, model->figure_rows[i].name,
                     absolute == model->selection);
  }
  if (model->count == 0) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter,
                            "No entries");
  }
  az_draw_list_scrollbar(canvas, model->selection, model->count);
  az_draw_footer(canvas, "< Search", "OK", "");
}

/**
 * @brief Render the saved-figure catalog.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_saved(Canvas *canvas, const AzViewModel *model) {
  az_draw_header(canvas, model, "Saved figures");
  AmiiboZeroApp *app = model->app;
  uint16_t start = model->selection >= 2 ? (uint16_t)(model->selection - 2) : 0;
  if (app->saved_count > AZ_LIST_ROWS &&
      start + AZ_LIST_ROWS > app->saved_count) {
    start = (uint16_t)(app->saved_count - AZ_LIST_ROWS);
  }
  for (uint8_t row = 0; row < AZ_LIST_ROWS && start + row < app->saved_count;
       row++) {
    if (!app->saved_entries)
      break;
    const AzSavedEntry *entry = &app->saved_entries[start + row];
    az_draw_list_row(canvas, model, row, entry->display_name,
                     start + row == model->selection);
  }
  if (model->count == 0) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter,
                            "No saved figures yet");
    canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignCenter,
                            "Browse to create one");
  }
  az_draw_list_scrollbar(canvas, model->selection, model->count);
  az_draw_footer(canvas, "", "OK", "");
}

/**
 * @brief Render the Lock-On payload selector.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_lockon(Canvas *canvas, const AzViewModel *model) {
  az_draw_header(canvas, model, "Select lock-on");
  AmiiboZeroApp *app = model->app;
  uint16_t start = model->selection >= 2 ? (uint16_t)(model->selection - 2) : 0;
  if (app->lockon_count > AZ_LIST_ROWS &&
      start + AZ_LIST_ROWS > app->lockon_count) {
    start = (uint16_t)(app->lockon_count - AZ_LIST_ROWS);
  }
  for (uint8_t row = 0; row < AZ_LIST_ROWS && start + row < app->lockon_count;
       row++) {
    if (!app->lockon_entries)
      break;
    const AzLockOnEntry *entry = &app->lockon_entries[start + row];
    az_draw_list_row(canvas, model, row, entry->display_name,
                     start + row == model->selection);
  }
  if (model->count == 0) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignCenter,
                            "No lock-on files");
    canvas_draw_str_aligned(canvas, 64, 39, AlignCenter, AlignCenter,
                            "Put files in /lock_on/");
  }
  az_draw_list_scrollbar(canvas, model->selection, model->count);
  az_draw_footer(canvas, "Back", "OK", "");
}

/**
 * @brief Return the number of actions available for the current figure state.
 * @param saved Whether the current figure is backed by a saved file.
 * @param v3 Whether the current figure uses the version-3 layout.
 * @return The number of contextual actions available.
 */
static uint8_t az_figure_action_count(bool saved, bool v3) {
  return saved ? (v3 ? 7U : 6U) : 3U;
}

/**
 * @brief Return the display label for a figure action index.
 * @param saved Whether the current figure is backed by a saved file.
 * @param v3 Whether the current figure uses the version-3 layout.
 * @param index Zero-based item or action index.
 * @return Pointer to the selected text, or NULL when no text is available.
 */
static const char *az_figure_action(bool saved, bool v3, uint8_t index) {
  if (saved) {
    static const char *standard_actions[] = {
        "Emulate + autosave", "Dump details", "Compatibility", "Rename",
        "Fresh copy",         "Delete",
    };
    static const char *v3_actions[] = {
        "Emulate + autosave",
        "Dump details",
        "Compatibility",
        "Change lock-on",
        "Rename",
        "Fresh copy",
        "Delete",
    };
    if (v3)
      return index < COUNT_OF(v3_actions) ? v3_actions[index] : "";
    return index < COUNT_OF(standard_actions) ? standard_actions[index] : "";
  }
  static const char *actions[] = {"Emulate once", "Save + emulate",
                                  "Compatibility"};
  return index < COUNT_OF(actions) ? actions[index] : "";
}

/**
 * @brief Render figure metadata and its contextual action menu.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_figure(Canvas *canvas, const AzViewModel *model) {
  AmiiboZeroApp *app = model->app;
  az_draw_header(canvas, model,
                 model->figure.name[0] ? model->figure.name : "Amiibo");
  canvas_set_font(canvas, FontSecondary);
  char type_line[48];
  snprintf(type_line, sizeof(type_line), "Type: %s%s",
           az_figure_type_name(model->figure.type),
           az_figure_is_v3(model->figure.id) ? " / Lock-on" : "");
  az_draw_fitted(canvas, app, 2, 20, type_line, 124);
  char idline[32];
  snprintf(idline, sizeof(idline), "ID %s", model->figure.id_hex);
  az_draw_fitted(canvas, app, 2, 29, idline, 124);

  bool v3 = az_figure_is_v3(model->figure.id);
  uint8_t count = az_figure_action_count(model->figure_saved, v3);
  uint8_t start_row =
      model->selection >= 2 ? (uint8_t)(model->selection - 2) : 0;
  if (count > 3 && start_row + 3 > count)
    start_row = (uint8_t)(count - 3);
  for (uint8_t row = 0; row < 3 && start_row + row < count; row++) {
    uint8_t index = (uint8_t)(start_row + row);
    int top = 31 + row * 8;
    int baseline = top + 7;
    bool selected = index == model->selection;
    if (selected) {
      canvas_set_color(canvas, ColorBlack);
      canvas_draw_rbox(canvas, 1, top, 124, 8, 2);
      canvas_set_color(canvas, ColorWhite);
      az_draw_marquee(canvas, app, 4, baseline,
                      az_figure_action(model->figure_saved, v3, index), 118,
                      model->animation, false);
      canvas_set_color(canvas, ColorBlack);
    } else {
      az_draw_fitted(canvas, app, 4, baseline,
                     az_figure_action(model->figure_saved, v3, index), 118);
    }
  }
  if (count > 3)
    elements_scrollbar_pos(canvas, 125, 31, 24, model->selection, count);
}

/**
 * @brief Return the byte length implied by a UTF-8 leading byte.
 * @param value Value to process or transmit.
 * @return The expected UTF-8 character length in bytes.
 */
static size_t az_utf8_char_len(unsigned char value) {
  if ((value & 0x80U) == 0)
    return 1;
  if ((value & 0xE0U) == 0xC0U)
    return 2;
  if ((value & 0xF0U) == 0xE0U)
    return 3;
  if ((value & 0xF8U) == 0xF0U)
    return 4;
  return 1;
}

/**
 * @brief Extract the next display-width-constrained line from UTF-8 text.
 * @param canvas Canvas to draw on.
 * @param cursor Pointer to the current position in the source text.
 * @param width Available drawing width in pixels.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_wrap_next_line(Canvas *canvas, const char **cursor, size_t width,
                              char *out, size_t out_size) {
  const char *p = *cursor;
  if (!p || !*p || out_size < 2)
    return false;

  if (*p == '\n') {
    out[0] = 0;
    *cursor = p + 1;
    return true;
  }
  while (*p == ' ' || *p == '\t' || *p == '\r')
    p++;
  if (!*p) {
    *cursor = p;
    return false;
  }

  size_t len = 0;
  size_t last_space_len = 0;
  const char *last_space_next = NULL;
  const char *scan = p;
  const char *next = p;
  while (*scan && *scan != '\n') {
    if (*scan == ' ' || *scan == '\t') {
      last_space_len = len;
      last_space_next = scan + 1;
    }
    size_t cp = az_utf8_char_len((unsigned char)*scan);
    size_t available = strlen(scan);
    if (cp > available)
      cp = 1;
    if (len + cp + 1 >= out_size)
      break;
    memcpy(out + len, scan, cp);
    size_t candidate = len + cp;
    out[candidate] = 0;
    if (canvas_string_width(canvas, out) > width) {
      if (last_space_next && last_space_len > 0) {
        len = last_space_len;
        next = last_space_next;
      } else if (len > 0) {
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

  if (*scan == '\n' && next == scan)
    next = scan + 1;
  while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t'))
    len--;
  out[len] = 0;
  while (*next == ' ' || *next == '\t' || *next == '\r')
    next++;
  *cursor = next;
  return true;
}

/**
 * @brief Count the number of display lines required for wrapped text.
 * @param canvas Canvas to draw on.
 * @param text Text to process or display.
 * @param width Available drawing width in pixels.
 * @return The number of wrapped display lines required by the text.
 */
static uint16_t az_wrapped_line_count(Canvas *canvas, const char *text,
                                      size_t width) {
  if (!text || !text[0])
    return 0;
  const char *cursor = text;
  char line[96];
  uint16_t count = 0;
  while (az_wrap_next_line(canvas, &cursor, width, line, sizeof(line)))
    count++;
  return count;
}

/**
 * @brief Render a scrollable window of wrapped detail text.
 * @param canvas Canvas to draw on.
 * @param app Application state.
 * @param text Text to process or display.
 * @param top Top drawing coordinate.
 * @param visible_lines Maximum number of wrapped lines to render.
 * @param width Available drawing width in pixels.
 */
static void az_draw_wrapped_text(Canvas *canvas, AmiiboZeroApp *app,
                                 const char *text, int top,
                                 uint8_t visible_lines, size_t width) {
  canvas_set_font(canvas, FontSecondary);
  uint16_t total = az_wrapped_line_count(canvas, text, width);
  uint16_t max_scroll =
      total > visible_lines ? (uint16_t)(total - visible_lines) : 0;
  if (app->detail_scroll > max_scroll)
    app->detail_scroll = max_scroll;

  const char *cursor = text;
  char line[96];
  uint16_t index = 0;
  uint8_t drawn = 0;
  while (az_wrap_next_line(canvas, &cursor, width, line, sizeof(line))) {
    if (index >= app->detail_scroll && drawn < visible_lines) {
      canvas_draw_str(canvas, 2, top + drawn * AZ_UI_DETAIL_LINE_HEIGHT, line);
      drawn++;
    }
    index++;
    if (drawn == visible_lines && index >= total)
      break;
  }
  if (total > visible_lines) {
    elements_scrollbar_pos(canvas, 125, top - 7,
                           visible_lines * AZ_UI_DETAIL_LINE_HEIGHT,
                           app->detail_scroll, max_scroll + 1);
  }
}

/**
 * @brief Render game compatibility details.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_games(Canvas *canvas, const AzViewModel *model) {
  AmiiboZeroApp *app = model->app;
  char title[48];
  if (model->games_total) {
    snprintf(title, sizeof(title), "Compatibility %u/%u", model->selection + 1,
             model->games_total);
  } else {
    az_str_copy(title, sizeof(title), "Compatibility");
  }
  az_draw_header(canvas, model, title);

  if (model->games_total == 0) {
    az_draw_wrapped_text(canvas, app,
                         "No compatibility records were found for this Amiibo "
                         "in games_info.json.",
                         20, AZ_UI_DETAIL_LINES, 120);
  } else {
    char detail[AZ_NAME_MAX + AZ_USAGE_MAX + 64];
    snprintf(detail, sizeof(detail), "%s\n%s - %s\n%s", model->games[0].name,
             model->games[0].platform,
             model->games[0].writes ? "writes tag data" : "read only",
             model->games[0].usage);
    az_draw_wrapped_text(canvas, app, detail, 20, AZ_UI_DETAIL_LINES, 120);
  }
  az_draw_footer(canvas, "< Prev", "Up/Dn", "Next >");
}

/**
 * @brief Render the active emulation screen.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_emulate(Canvas *canvas, const AzViewModel *model) {
  AmiiboZeroApp *app = model->app;
  az_draw_header(canvas, model, "Emulating");
  canvas_set_font(canvas, FontPrimary);
  az_draw_marquee(canvas, app, 2, 21, model->figure.name, 124, model->animation,
                  true);
  int radius = 4 + ((model->animation / 2) % 2);
  canvas_draw_circle(canvas, 15, 43, radius);
  canvas_draw_circle(canvas, 15, 43, radius + 4);
  canvas_set_font(canvas, FontSecondary);
  canvas_draw_str(canvas, 29, 42,
                  model->figure_saved ? "Writes autosave"
                                      : "Temporary session");
  canvas_draw_str(canvas, 29, 51, "OK: randomize UID");
  az_draw_footer(canvas, "Back", "OK UID", "");
}

/**
 * @brief Render setup and database status information.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_status(Canvas *canvas, const AzViewModel *model) {
  AmiiboZeroApp *app = model->app;
  az_draw_header(canvas, model, "Setup & status");
  char text[320];
  snprintf(text, sizeof(text),
           "%s\n%s\n%s\n%s\nIndex: %s (%lu figures)\nOK reloads keys and "
           "forces a background index rebuild.",
           app->keys.valid ? "Keys: ready" : "Keys: missing/invalid",
           storage_file_exists(app->storage, AZ_AMIIBO_JSON)
               ? "Amiibo JSON: found"
               : "Amiibo JSON: missing",
           storage_file_exists(app->storage, AZ_GAMES_JSON)
               ? "Games JSON: found"
               : "Games JSON: missing",
           "Index identity: size + samples",
           app->index_ready ? "ready" : "unavailable",
           (unsigned long)app->index_count);
  az_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
  az_draw_footer(canvas, "Back", "OK", "");
}

/**
 * @brief Render application version and data-source information.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_about(Canvas *canvas, const AzViewModel *model) {
  AmiiboZeroApp *app = model->app;
  az_draw_header(canvas, model, "About");
  char text[192];
  snprintf(
      text, sizeof(text),
      "Amiibo Zero %s\nBrowse, generate, emulate, and persist Amiibo.\nJSON: "
      "AmiiboAPI + lwJSON. Crypto: mbedTLS.\nKeys/dumps are never bundled.",
      AZ_APP_VERSION);
  az_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
  az_draw_footer(canvas, "Back", "Up/Dn", "");
}

/**
 * @brief Render decoded metadata from the current saved Amiibo dump.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_dump_info(Canvas *canvas, const AzViewModel *model) {
  AmiiboZeroApp *app = model->app;
  az_draw_header(canvas, model,
                 model->figure.name[0] ? model->figure.name : "Dump details");
  char text[640];
  const AzAmiiboDetails *details = &app->current_details;
  bool v3 = az_figure_is_v3(model->figure.id);
  const char *lockon_line =
      v3 ? (app->current_lockon_valid ? "\nLock-on: attached"
                                      : "\nLock-on: missing")
         : "";
  if (!details->available) {
    snprintf(text, sizeof(text),
             "Type: %s%s\nID: %s\nFile: %s%s\nEncrypted details unavailable. A "
             "valid key_retail.bin is required and the dump must authenticate.",
             az_figure_type_name(model->figure.type), v3 ? " / Lock-on" : "",
             model->figure.id_hex, model->saved_filename, lockon_line);
  } else {
    unsigned long app_hi = (unsigned long)(details->application_id >> 32);
    unsigned long app_lo =
        (unsigned long)(details->application_id & 0xFFFFFFFFULL);
    snprintf(text, sizeof(text),
             "Type: %s%s\nID: %s\nFile: %s%s\nNickname: %s\nOwner Mii: "
             "%s\nInitialized: %s\nApp data: %s\nRegistered: %s\nLast write: "
             "%s\nWrite count: %u\nApplication: %08lX%08lX\nArea ID: "
             "%08lX\nApp writes: %u",
             az_figure_type_name(model->figure.type), v3 ? " / Lock-on" : "",
             model->figure.id_hex, model->saved_filename, lockon_line,
             details->nickname[0] ? details->nickname : "(none)",
             details->owner_mii[0] ? details->owner_mii : "(none)",
             details->initialized ? "yes" : "no",
             details->app_data_initialized ? "yes" : "no",
             details->init_date[0] ? details->init_date : "-",
             details->write_date[0] ? details->write_date : "-",
             details->write_counter, app_hi, app_lo,
             (unsigned long)details->application_area_id,
             details->application_write_counter);
  }
  az_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
  az_draw_footer(canvas, "Back", "Up/Dn", "");
}

/**
 * @brief Return a display label for a database progress phase.
 * @param stage Database progress phase.
 * @return Pointer to the selected text, or NULL when no text is available.
 */
static const char *az_database_progress_label(AzDbProgressStage stage) {
  switch (stage) {
  case AzDbProgressChecking:
    return "Checking source files";
  case AzDbProgressAmiibo:
    return "Reading amiibo.json";
  case AzDbProgressSorting:
    return "Sorting figures";
  case AzDbProgressGames:
    return "Reading games_info.json";
  case AzDbProgressFinalizing:
    return "Finalizing index";
  case AzDbProgressDone:
    return "Database ready";
  default:
    return "Preparing database";
  }
}

/**
 * @brief Render the database preparation progress screen.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_working(Canvas *canvas, const AzViewModel *model) {
  az_draw_header(canvas, model, "Preparing database");
  uint8_t phase = (model->animation / 2U) & 1U;

  canvas_draw_frame(canvas, 7, 17, 14, 16);
  canvas_draw_line(canvas, 9, 19, 19, 19);
  canvas_draw_line(canvas, 9, 31, 19, 31);
  if (phase == 0U) {
    canvas_draw_line(canvas, 10, 21, 18, 21);
    canvas_draw_line(canvas, 12, 23, 16, 23);
  } else {
    canvas_draw_line(canvas, 12, 27, 16, 27);
    canvas_draw_line(canvas, 10, 29, 18, 29);
  }

  canvas_set_font(canvas, FontSecondary);
  az_draw_fitted(canvas, model->app, 26, 28,
                 az_database_progress_label(model->db_progress_stage), 98);

  canvas_draw_frame(canvas, 13, 38, 102, 8);
  uint8_t progress = model->db_progress > 100U ? 100U : model->db_progress;
  if (progress)
    canvas_draw_box(canvas, 14, 39, progress, 6);

  char percent[8];
  snprintf(percent, sizeof(percent), "%u%%", progress);
  canvas_draw_str_aligned(canvas, 64, 53, AlignCenter, AlignBottom, percent);
}

/**
 * @brief Render the saved-figure deletion confirmation screen.
 * @param canvas Canvas to draw on.
 * @param model View model containing the state to render.
 */
static void az_draw_delete(Canvas *canvas, const AzViewModel *model) {
  AmiiboZeroApp *app = model->app;
  az_draw_header(canvas, model, "Delete saved figure?");
  canvas_set_font(canvas, FontPrimary);
  az_draw_marquee(canvas, app, 2, 24, model->figure.name, 124, model->animation,
                  true);
  canvas_set_font(canvas, FontSecondary);
  canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter,
                          "This cannot be undone.");
  az_draw_footer(canvas, "Back", "OK Del", "");
}

/**
 * @brief Dispatch drawing to the renderer for the active screen.
 * @param canvas Canvas to draw on.
 * @param context Caller-owned callback context.
 */
static void az_draw_callback(Canvas *canvas, void *context) {
  const AzViewModel *model = context;
  canvas_clear(canvas);
  switch (model->screen) {
  case AzScreenHome:
    az_draw_home(canvas, model);
    break;
  case AzScreenCategories:
    az_draw_categories(canvas, model);
    break;
  case AzScreenFigures:
    az_draw_figures(canvas, model, false);
    break;
  case AzScreenSearchResults:
    az_draw_figures(canvas, model, true);
    break;
  case AzScreenSaved:
    az_draw_saved(canvas, model);
    break;
  case AzScreenAdvanced:
    az_draw_advanced(canvas, model);
    break;
  case AzScreenLockOn:
    az_draw_lockon(canvas, model);
    break;
  case AzScreenFigure:
    az_draw_figure(canvas, model);
    break;
  case AzScreenDumpInfo:
    az_draw_dump_info(canvas, model);
    break;
  case AzScreenGames:
    az_draw_games(canvas, model);
    break;
  case AzScreenEmulate:
    az_draw_emulate(canvas, model);
    break;
  case AzScreenStatus:
    az_draw_status(canvas, model);
    break;
  case AzScreenAbout:
    az_draw_about(canvas, model);
    break;
  case AzScreenConfirmDelete:
    az_draw_delete(canvas, model);
    break;
  case AzScreenWorking:
    az_draw_working(canvas, model);
    break;
  default:
    break;
  }
}

/**
 * @brief Change screens while preserving or resetting per-screen selection
 * state.
 * @param app Application state.
 * @param screen Screen to activate.
 * @param reset_target Whether the destination screen selection should be reset.
 */
static void az_navigate(AmiiboZeroApp *app, AzScreen screen,
                        bool reset_target) {
  if (!app || screen >= AzScreenCount)
    return;
  if (app->screen < AzScreenCount)
    app->screen_selection[app->screen] = app->selection;
  if (reset_target)
    app->screen_selection[screen] = 0;
  app->selection = app->screen_selection[screen];
  app->detail_scroll = 0;
  az_ui_show(app, screen);
}

/**
 * @brief Extract the saved filename component from a full path.
 * @param app Application state.
 * @param path Filesystem path.
 */
static void az_set_saved_filename_from_path(AmiiboZeroApp *app,
                                            const char *path) {
  const char *slash = strrchr(path, '/');
  az_str_copy(app->current_saved_filename, sizeof(app->current_saved_filename),
              slash ? slash + 1 : path);
}

/**
 * @brief Reload the Lock-On catalog from storage.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_refresh_lockon_catalog(AmiiboZeroApp *app) {
  if (!app)
    return false;
  if (!app->lockon_entries) {
    app->lockon_entries = calloc(AZ_MAX_LOCKONS, sizeof(AzLockOnEntry));
    if (!app->lockon_entries) {
      app->lockon_count = 0;
      return false;
    }
  }
  memset(app->lockon_entries, 0, AZ_MAX_LOCKONS * sizeof(AzLockOnEntry));
  app->lockon_count =
      az_lockon_scan(app->storage, app->lockon_entries, AZ_MAX_LOCKONS);
  return true;
}

/**
 * @brief Release the in-memory Lock-On catalog.
 * @param app Application state.
 */
static void az_clear_lockon_catalog(AmiiboZeroApp *app) {
  if (!app)
    return;
  free(app->lockon_entries);
  app->lockon_entries = NULL;
  app->lockon_count = 0;
}

/**
 * @brief Open the Lock-On selector for a deferred action.
 * @param app Application state.
 * @param action Deferred Lock-On action to perform.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_open_lockon_selector(AmiiboZeroApp *app, AzLockOnAction action) {
  if (!app || action == AzLockOnActionNone)
    return false;

  az_clear_saved_catalog(app);
  if (!az_refresh_lockon_catalog(app)) {
    az_ui_toast(app, "Lock-on catalog unavailable");
    return false;
  }
  app->lockon_action = action;
  if (app->screen < AzScreenCount)
    app->screen_selection[app->screen] = app->selection;
  if (app->screen_selection[AzScreenLockOn] >= app->lockon_count &&
      app->lockon_count) {
    app->screen_selection[AzScreenLockOn] = (uint16_t)(app->lockon_count - 1U);
  }
  app->selection = app->screen_selection[AzScreenLockOn];
  az_ui_show(app, AzScreenLockOn);
  return true;
}

/**
 * @brief Prepare and start NFC emulation using generated or saved tag data.
 * @param app Application state.
 * @param persistent Whether emulation should preserve mutable state.
 * @param fresh Whether to generate a fresh UID before emulation.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_begin_emulation(AmiiboZeroApp *app, bool persistent,
                               bool fresh) {
  if (!app->keys.valid && fresh) {
    az_ui_toast(app, "Add key_retail.bin first");
    return false;
  }
  if (fresh && az_figure_is_v3(app->current_figure.id) &&
      !app->current_lockon_valid) {
    az_ui_toast(app, "Select a lock-on first");
    return false;
  }

  nfc_device_clear(app->nfc_device);
  char path[AZ_PATH_MAX] = {0};
  bool ok = false;
  if (!fresh && app->current_is_saved) {
    snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR,
             app->current_saved_filename);
    ok = az_nfc_load_device(app->nfc_device, path);
    if (ok && az_nfc_device_is_v3(app->nfc_device)) {
      app->current_lockon_valid = az_saved_lockon_load(
          app->storage, app->current_saved_filename, app->current_lockon_sram);
      app->current_lockon_filename[0] = '\0';
      ok = app->current_lockon_valid;
    }
  } else {
    ok = az_nfc_generate_device(app->nfc_device, &app->current_figure,
                                &app->keys);
    if (ok && persistent) {
      az_make_unique_save_path(app->storage, &app->current_figure, path,
                               sizeof(path));
      ok = path[0] && az_nfc_save_device(app->nfc_device, path);
      if (ok) {
        az_set_saved_filename_from_path(app, path);
        if (az_figure_is_v3(app->current_figure.id)) {
          ok = app->current_lockon_valid &&
               az_saved_lockon_save(app->storage, app->current_saved_filename,
                                    app->current_lockon_sram);
          if (!ok)
            az_saved_delete(app->storage, app->current_saved_filename);
        }
        if (ok)
          app->current_is_saved = true;
      }
    }
  }
  if (!ok) {
    az_ui_toast(app, "Could not prepare figure");
    return false;
  }

  if (!az_nfc_listener_start(app)) {
    az_ui_toast(app, "NFC listener unavailable");
    return false;
  }
  app->emulation_persistent = persistent || app->current_is_saved;
  if (app->emulation_persistent) {
    if (path[0])
      az_str_copy(app->emulation_path, sizeof(app->emulation_path), path);
    else
      snprintf(app->emulation_path, sizeof(app->emulation_path), "%s/%s",
               AZ_FIGURES_DIR, app->current_saved_filename);
  } else {
    app->emulation_path[0] = '\0';
  }
  if (app->screen < AzScreenCount)
    app->screen_selection[app->screen] = app->selection;
  app->selection = 0;
  az_ui_show(app, AzScreenEmulate);
  return true;
}

/**
 * @brief Start emulation immediately or route through Lock-On selection when
 * required.
 * @param app Application state.
 * @param persistent Whether emulation should preserve mutable state.
 * @param fresh Whether to generate a fresh UID before emulation.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_request_emulation(AmiiboZeroApp *app, bool persistent,
                                 bool fresh) {
  if (!app)
    return false;
  if (fresh && !app->keys.valid) {
    az_ui_toast(app, "Add key_retail.bin first");
    return false;
  }
  if (fresh && az_figure_is_v3(app->current_figure.id)) {
    app->current_lockon_valid = false;
    app->current_lockon_filename[0] = '\0';
    return az_open_lockon_selector(app, persistent
                                            ? AzLockOnActionGeneratePersistent
                                            : AzLockOnActionGenerateTemporary);
  }
  if (!fresh && app->current_is_saved &&
      az_figure_is_v3(app->current_figure.id)) {
    app->current_lockon_valid = az_saved_lockon_load(
        app->storage, app->current_saved_filename, app->current_lockon_sram);
    app->current_lockon_filename[0] = '\0';
    if (!app->current_lockon_valid) {
      return az_open_lockon_selector(app, AzLockOnActionEmulateSaved);
    }
  }
  return az_begin_emulation(app, persistent, fresh);
}

/**
 * @brief Apply the selected Lock-On payload to the pending emulation action.
 * @param app Application state.
 */
static void az_apply_selected_lockon(AmiiboZeroApp *app) {
  if (!app || !app->lockon_entries || app->selection >= app->lockon_count)
    return;
  AzLockOnEntry selected = app->lockon_entries[app->selection];
  uint8_t sram[AZ_LOCKON_SRAM_SIZE];
  if (!az_lockon_load(app->storage, selected.filename, sram)) {
    az_ui_toast(app, "Could not read lock-on");
    return;
  }

  memcpy(app->current_lockon_sram, sram, sizeof(sram));
  app->current_lockon_valid = true;
  az_str_copy(app->current_lockon_filename,
              sizeof(app->current_lockon_filename), selected.filename);
  app->screen_selection[AzScreenLockOn] = app->selection;
  AzLockOnAction action = app->lockon_action;
  app->lockon_action = AzLockOnActionNone;
  az_clear_lockon_catalog(app);

  if (action == AzLockOnActionGenerateTemporary) {
    if (!az_begin_emulation(app, false, true))
      az_ui_show(app, AzScreenFigure);
    return;
  }
  if (action == AzLockOnActionGeneratePersistent) {
    if (!az_begin_emulation(app, true, true))
      az_ui_show(app, AzScreenFigure);
    return;
  }
  if (action == AzLockOnActionEmulateSaved && app->current_is_saved) {
    bool ok = az_saved_lockon_save(app->storage, app->current_saved_filename,
                                   app->current_lockon_sram);
    if (ok) {
      if (!az_begin_emulation(app, true, false))
        az_ui_show(app, AzScreenFigure);
    } else {
      app->selection = app->screen_selection[AzScreenFigure];
      az_ui_show(app, AzScreenFigure);
      az_ui_toast(app, "Could not attach lock-on");
    }
    return;
  }
  if (action == AzLockOnActionReplaceSaved && app->current_is_saved) {
    bool ok = az_saved_lockon_save(app->storage, app->current_saved_filename,
                                   app->current_lockon_sram);
    app->selection = app->screen_selection[AzScreenFigure];
    az_ui_show(app, AzScreenFigure);
    az_ui_toast(app, ok ? "Lock-on changed" : "Lock-on change failed");
    return;
  }

  az_ui_show(app, AzScreenFigure);
}

/**
 * @brief Stop NFC emulation and persist mutable state when required.
 * @param app Application state.
 */
void az_emulation_stop(AmiiboZeroApp *app) {
  if (!app || !app->emulating)
    return;
  bool synced = az_nfc_listener_pause_and_sync(app);
  if (app->emulation_persistent && synced) {
    if (!az_nfc_save_device(app->nfc_device, app->emulation_path)) {
      az_str_copy(app->toast, sizeof(app->toast), "Autosave failed");
      app->toast_ticks = 10;
    } else {
      az_str_copy(app->toast, sizeof(app->toast), "Saved changes");
      app->toast_ticks = 6;
    }
  }
  app->emulation_persistent = false;
}

/**
 * @brief Randomize the emulated Amiibo UID and restart the listener.
 * @param app Application state.
 */
static void az_emulation_randomize_uid(AmiiboZeroApp *app) {
  if (!app || !app->emulating)
    return;
  if (!app->keys.valid) {
    az_ui_toast(app, "key_retail.bin required");
    return;
  }
  if (!az_nfc_listener_pause_and_sync(app)) {
    az_ui_toast(app, "Could not pause NFC");
    return;
  }

  bool randomized = az_nfc_randomize_uid(app->nfc_device, &app->keys);
  bool saved = true;
  if (randomized && app->emulation_persistent) {
    saved = az_nfc_save_device(app->nfc_device, app->emulation_path);
  }
  bool restarted = az_nfc_listener_start(app);
  if (!restarted) {
    app->emulation_persistent = false;
    az_ui_toast(app, "NFC restart failed");
    az_navigate(app, AzScreenFigure, false);
    return;
  }
  if (!randomized)
    az_ui_toast(app, "UID randomize failed");
  else if (!saved)
    az_ui_toast(app, "UID changed; save failed");
  else
    az_ui_toast(app, "UID randomized");
}

/**
 * @brief Open the text-input view for a figure search.
 * @param app Application state.
 */
static void az_open_search(AmiiboZeroApp *app) {
  az_str_copy(app->text_buffer, sizeof(app->text_buffer), app->query);
  app->text_input_mode = AzTextInputSearch;
  text_input_reset(app->text_input);
  text_input_set_header_text(app->text_input, "Search name or Amiibo ID");
  text_input_set_minimum_length(app->text_input, 0);
  text_input_set_result_callback(app->text_input, az_search_done, app,
                                 app->text_buffer, sizeof(app->text_buffer),
                                 false);
  view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_SEARCH);
}

/**
 * @brief Apply submitted search text and open the search results screen.
 * @param context Caller-owned callback context.
 */
static void az_search_done(void *context) {
  AmiiboZeroApp *app = context;
  az_str_copy(app->query, sizeof(app->query), app->text_buffer);
  app->screen_selection[AzScreenSearchResults] = 0;
  app->selection = 0;
  app->detail_scroll = 0;
  az_ui_show(app, app->query[0] ? AzScreenSearchResults : AzScreenCategories);
}

/**
 * @brief Open the text-input view for renaming the current saved figure.
 * @param app Application state.
 */
static void az_open_rename(AmiiboZeroApp *app) {
  az_str_copy(app->text_buffer, sizeof(app->text_buffer),
              app->current_saved_filename);
  char *dot = strrchr(app->text_buffer, '.');
  if (dot)
    *dot = '\0';
  app->text_input_mode = AzTextInputRename;
  text_input_reset(app->text_input);
  text_input_set_header_text(app->text_input, "Rename saved Amiibo");
  text_input_set_minimum_length(app->text_input, 1);
  text_input_set_result_callback(app->text_input, az_rename_done, app,
                                 app->text_buffer, sizeof(app->text_buffer),
                                 false);
  view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_SEARCH);
}

/**
 * @brief Apply a submitted saved-figure rename and refresh the catalog.
 * @param context Caller-owned callback context.
 */
static void az_rename_done(void *context) {
  AmiiboZeroApp *app = context;
  char renamed[96];
  if (az_saved_rename(app->storage, app->current_saved_filename,
                      app->text_buffer, renamed, sizeof(renamed))) {
    az_str_copy(app->current_saved_filename,
                sizeof(app->current_saved_filename), renamed);
    if (!az_refresh_saved_catalog(app)) {
      az_ui_show(app, AzScreenFigure);
      az_ui_toast(app, "Renamed; catalog refresh failed");
      return;
    }
    app->screen_selection[AzScreenSaved] = az_saved_selection_for_filename(
        app, renamed, app->screen_selection[AzScreenSaved]);
    az_ui_show(app, AzScreenFigure);
    az_ui_toast(app, "Renamed");
  } else {
    az_ui_show(app, AzScreenFigure);
    az_ui_toast(app, "Rename failed");
  }
}

/**
 * @brief Open the byte-input view for entering a figure identifier.
 * @param app Application state.
 */
static void az_open_manual_id(AmiiboZeroApp *app) {
  memset(app->manual_id, 0, sizeof(app->manual_id));
  byte_input_set_header_text(app->byte_input, "Manual figure ID (8 bytes)");
  byte_input_set_result_callback(app->byte_input, az_manual_id_done, NULL, app,
                                 app->manual_id, sizeof(app->manual_id));
  view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MANUAL_ID);
}

/**
 * @brief Resolve a manually entered figure identifier and open its detail
 * screen.
 * @param context Caller-owned callback context.
 */
static void az_manual_id_done(void *context) {
  AmiiboZeroApp *app = context;
  memset(&app->current_figure, 0, sizeof(app->current_figure));
  if (!az_db_find_by_id(app->storage, app->manual_id, &app->current_figure)) {
    static const char hex[] = "0123456789abcdef";
    memcpy(app->current_figure.id, app->manual_id, 8);
    for (size_t i = 0; i < 8; i++) {
      app->current_figure.id_hex[i * 2] = hex[app->manual_id[i] >> 4];
      app->current_figure.id_hex[i * 2 + 1] = hex[app->manual_id[i] & 0x0F];
    }
    app->current_figure.id_hex[16] = '\0';
    app->current_figure.category = app->manual_id[6];
    app->current_figure.type = app->manual_id[3];
    snprintf(app->current_figure.name, sizeof(app->current_figure.name),
             "Manual %s", app->current_figure.id_hex);
  }
  app->current_is_saved = false;
  app->current_saved_filename[0] = '\0';
  app->current_lockon_valid = false;
  app->current_lockon_filename[0] = '\0';
  app->return_screen = AzScreenAdvanced;
  app->return_selection = app->screen_selection[AzScreenAdvanced];
  app->screen_selection[AzScreenFigure] = 0;
  app->selection = 0;
  az_ui_show(app, AzScreenFigure);
}

/**
 * @brief Handle back-navigation requests from auxiliary input views.
 * @param context Caller-owned callback context.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_navigation_callback(void *context) {
  AmiiboZeroApp *app = context;
  view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
  return true;
}

/**
 * @brief Reload the saved-figure catalog from storage.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_refresh_saved_catalog(AmiiboZeroApp *app) {
  if (!app)
    return false;
  if (!app->saved_entries) {
    app->saved_entries = calloc(AZ_MAX_SAVED, sizeof(AzSavedEntry));
    if (!app->saved_entries) {
      app->saved_count = 0;
      return false;
    }
  }
  memset(app->saved_entries, 0, AZ_MAX_SAVED * sizeof(AzSavedEntry));
  app->saved_count =
      az_saved_scan(app->storage, app->saved_entries, AZ_MAX_SAVED);
  return true;
}

/**
 * @brief Release the in-memory saved-figure catalog.
 * @param app Application state.
 */
static void az_clear_saved_catalog(AmiiboZeroApp *app) {
  if (!app)
    return;
  free(app->saved_entries);
  app->saved_entries = NULL;
  app->saved_count = 0;
}

/**
 * @brief Find the list selection corresponding to a saved filename.
 * @param app Application state.
 * @param filename Filename relative to the relevant application data directory.
 * @param fallback Fallback value used when no more specific value is available.
 * @return The matching selection ordinal, or the supplied fallback when the
 * filename is absent.
 */
static uint16_t az_saved_selection_for_filename(const AmiiboZeroApp *app,
                                                const char *filename,
                                                uint16_t fallback) {
  if (!app || !app->saved_entries || app->saved_count == 0)
    return 0;
  if (filename && filename[0]) {
    for (uint16_t i = 0; i < app->saved_count; i++) {
      if (strcmp(app->saved_entries[i].filename, filename) == 0)
        return i;
    }
  }
  return fallback < app->saved_count ? fallback
                                     : (uint16_t)(app->saved_count - 1);
}

/**
 * @brief Load the selected saved NFC file and open its figure details.
 * @param app Application state.
 */
static void az_open_current_saved(AmiiboZeroApp *app) {
  if (!app->saved_entries || app->saved_count == 0 ||
      app->selection >= app->saved_count)
    return;
  app->screen_selection[AzScreenSaved] = app->selection;
  const AzSavedEntry *entry = &app->saved_entries[app->selection];
  memset(&app->current_figure, 0, sizeof(app->current_figure));
  memcpy(app->current_figure.id, entry->id, 8);
  app->current_figure.category = entry->id[6];
  app->current_figure.type = entry->id[3];
  if (!az_db_find_by_id(app->storage, entry->id, &app->current_figure)) {
    az_str_copy(app->current_figure.name, sizeof(app->current_figure.name),
                entry->display_name);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 8; i++) {
      app->current_figure.id_hex[i * 2] = hex[entry->id[i] >> 4];
      app->current_figure.id_hex[i * 2 + 1] = hex[entry->id[i] & 0x0F];
    }
    app->current_figure.id_hex[16] = '\0';
  }
  app->current_is_saved = true;
  az_str_copy(app->current_saved_filename, sizeof(app->current_saved_filename),
              entry->filename);
  memset(&app->current_details, 0, sizeof(app->current_details));
  char path[AZ_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR,
           app->current_saved_filename);
  nfc_device_clear(app->nfc_device);
  if (az_nfc_load_device(app->nfc_device, path)) {
    az_nfc_read_details(app->nfc_device, &app->keys, &app->current_details);
    app->current_lockon_valid =
        az_nfc_device_is_v3(app->nfc_device) &&
        az_saved_lockon_load(app->storage, app->current_saved_filename,
                             app->current_lockon_sram);
    app->current_lockon_filename[0] = '\0';
  } else {
    app->current_lockon_valid = false;
    app->current_lockon_filename[0] = '\0';
  }
  app->screen_selection[AzScreenFigure] = 0;
  app->selection = 0;
  app->detail_scroll = 0;
  az_ui_show(app, AzScreenFigure);
}

/**
 * @brief Release loaded game compatibility records.
 * @param app Application state.
 */
static void az_clear_games(AmiiboZeroApp *app) {
  if (!app)
    return;
  free(app->game_entries);
  app->game_entries = NULL;
  app->game_count = 0;
}

/**
 * @brief Load and display game compatibility for the current figure.
 * @param app Application state.
 */
static void az_open_games(AmiiboZeroApp *app) {
  app->screen_selection[AzScreenFigure] = app->selection;
  az_clear_games(app);
  app->game_entries = calloc(AZ_MAX_GAMES, sizeof(AzGame));
  if (!app->game_entries) {
    az_ui_toast(app, "Not enough memory for games");
    return;
  }
  if (!az_db_load_games(app->storage, app->current_figure.id, app->game_entries,
                        AZ_MAX_GAMES, &app->game_count)) {
    az_ui_toast(app, "Could not read games DB");
  }
  app->screen_selection[AzScreenGames] = 0;
  app->selection = 0;
  app->detail_scroll = 0;
  az_ui_show(app, AzScreenGames);
}

/**
 * @brief Receive database progress updates from the worker thread.
 * @param context Caller-owned callback context.
 * @param stage Database progress phase.
 * @param percent Progress percentage in the range 0 through 100.
 */
static void az_database_progress(void *context, AzDbProgressStage stage,
                                 uint8_t percent) {
  AmiiboZeroApp *app = context;
  if (!app)
    return;
  app->db_progress_stage = stage;
  app->db_progress = percent > 100U ? 100U : percent;
}

/**
 * @brief Validate or rebuild the database index on a worker thread.
 * @param context Caller-owned callback context.
 * @return The computed result value.
 */
static int32_t az_database_worker(void *context) {
  AmiiboZeroApp *app = context;
  uint32_t count = 0;
  bool result = az_db_ensure_index(app->storage, app->db_thread_force, &count,
                                   az_database_progress, app);
  app->db_thread_count = count;
  app->db_thread_result = result;
  app->db_thread_done = true;
  return 0;
}

/**
 * @brief Start asynchronous database validation or rebuilding.
 * @param app Application state.
 * @param force Whether to rebuild even when the existing index is current.
 * @param return_screen Screen to restore when background preparation completes.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_ui_start_database_prepare(AmiiboZeroApp *app, bool force,
                                  AzScreen return_screen) {
  if (!app || app->db_thread)
    return false;
  if (app->screen < AzScreenCount)
    app->screen_selection[app->screen] = app->selection;
  app->db_thread_force = force;
  app->db_thread_return_screen = return_screen;
  app->db_thread_done = false;
  app->db_thread_result = false;
  app->db_thread_count = 0;
  app->db_progress = 0;
  app->db_progress_stage = AzDbProgressChecking;
  app->db_thread =
      furi_thread_alloc_ex("AmiiboIndex", 6144, az_database_worker, app);
  if (!app->db_thread)
    return false;
  app->selection = 0;
  app->detail_scroll = 0;
  az_ui_show(app, AzScreenWorking);
  furi_thread_start(app->db_thread);
  return true;
}

/**
 * @brief Release catalogs and detail data that could reference a database index
 * being rebuilt.
 * @param app Application state.
 */
static void az_release_runtime_records_for_index_refresh(AmiiboZeroApp *app) {
  if (!app)
    return;
  az_emulation_stop(app);
  az_clear_games(app);
  az_clear_saved_catalog(app);
  az_clear_lockon_catalog(app);
  if (app->nfc_device)
    nfc_device_clear(app->nfc_device);

  memset(&app->current_category, 0, sizeof(app->current_category));
  memset(&app->current_figure, 0, sizeof(app->current_figure));
  memset(&app->current_details, 0, sizeof(app->current_details));
  memset(app->current_lockon_sram, 0, sizeof(app->current_lockon_sram));
  app->current_is_saved = false;
  app->current_saved_filename[0] = '\0';
  app->current_lockon_valid = false;
  app->current_lockon_filename[0] = '\0';
  app->lockon_action = AzLockOnActionNone;
}

/**
 * @brief Refresh the status message describing keys and database availability.
 * @param app Application state.
 */
static void az_status_refresh(AmiiboZeroApp *app) {
  az_release_runtime_records_for_index_refresh(app);
  app->keys.valid = az_keys_load(app->storage, &app->keys);
  if (!az_ui_start_database_prepare(app, true, AzScreenStatus)) {
    az_ui_toast(app, "Refresh already running");
  }
}

/**
 * @brief Move a bounded list selection by one direction step.
 * @param app Application state.
 * @param direction Selection movement direction.
 * @param count Number of records or elements.
 */
static void az_move_selection(AmiiboZeroApp *app, int direction,
                              uint16_t count) {
  if (count == 0) {
    app->selection = 0;
    return;
  }
  uint16_t old = app->selection;
  if (direction < 0 && app->selection > 0)
    app->selection--;
  else if (direction > 0 && app->selection + 1 < count)
    app->selection++;
  if (old != app->selection)
    app->animation = 0;
}

/**
 * @brief Handle button input for the active main-screen state.
 * @param event NFC event to handle.
 * @param context Caller-owned callback context.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_main_input(InputEvent *event, void *context) {
  AmiiboZeroApp *app = context;
  if (event->type != InputTypeShort && event->type != InputTypeRepeat)
    return false;
  bool short_press = event->type == InputTypeShort;

  if (app->screen == AzScreenWorking)
    return true;

  if (short_press && event->key == InputKeyBack) {
    if (app->screen == AzScreenHome) {
      view_dispatcher_stop(app->dispatcher);
    } else if (app->screen == AzScreenCategories) {
      az_navigate(app, AzScreenHome, false);
    } else if (app->screen == AzScreenFigures ||
               app->screen == AzScreenSearchResults) {
      az_navigate(app, AzScreenCategories, false);
    } else if (app->screen == AzScreenSaved) {
      az_clear_saved_catalog(app);
      az_navigate(app, AzScreenHome, false);
    } else if (app->screen == AzScreenAdvanced) {
      az_navigate(app, AzScreenHome, false);
    } else if (app->screen == AzScreenLockOn) {
      az_clear_lockon_catalog(app);
      app->lockon_action = AzLockOnActionNone;
      az_navigate(app, AzScreenFigure, false);
    } else if (app->screen == AzScreenFigure) {
      if (app->current_is_saved) {
        if (!az_refresh_saved_catalog(app)) {
          az_ui_toast(app, "Saved catalog unavailable");
          az_navigate(app, AzScreenHome, false);
          return true;
        }
        app->screen_selection[AzScreenSaved] = az_saved_selection_for_filename(
            app, app->current_saved_filename,
            app->screen_selection[AzScreenSaved]);
        az_navigate(app, AzScreenSaved, false);
      } else {
        app->screen_selection[app->return_screen] = app->return_selection;
        az_navigate(app, app->return_screen, false);
      }
    } else if (app->screen == AzScreenDumpInfo ||
               app->screen == AzScreenGames ||
               app->screen == AzScreenConfirmDelete) {
      if (app->screen == AzScreenGames)
        az_clear_games(app);
      az_navigate(app, AzScreenFigure, false);
    } else if (app->screen == AzScreenEmulate) {
      az_emulation_stop(app);
      az_navigate(app, AzScreenFigure, false);
    } else if (app->screen == AzScreenStatus || app->screen == AzScreenAbout) {
      az_navigate(app, AzScreenHome, false);
    }
    az_ui_refresh(app);
    return true;
  }

  if (app->screen == AzScreenHome) {
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, 5);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, 5);
    else if (short_press && event->key == InputKeyOk) {
      app->screen_selection[AzScreenHome] = app->selection;
      if (app->selection == 0) {
        az_navigate(app, AzScreenCategories, false);
      } else if (app->selection == 1) {
        if (!az_refresh_saved_catalog(app)) {
          az_ui_toast(app, "Saved catalog unavailable");
          return true;
        }
        if (app->screen_selection[AzScreenSaved] >= app->saved_count &&
            app->saved_count) {
          app->screen_selection[AzScreenSaved] =
              (uint16_t)(app->saved_count - 1);
        }
        az_navigate(app, AzScreenSaved, false);
      } else if (app->selection == 2) {
        az_navigate(app, AzScreenAdvanced, false);
      } else if (app->selection == 3) {
        az_navigate(app, AzScreenStatus, false);
      } else if (app->selection == 4) {
        az_navigate(app, AzScreenAbout, false);
      }
      return true;
    }
  } else if (app->screen == AzScreenAdvanced) {
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, 1);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, 1);
    else if (short_press && event->key == InputKeyOk) {
      app->screen_selection[AzScreenAdvanced] = app->selection;
      az_open_manual_id(app);
      return true;
    }
  } else if (app->screen == AzScreenLockOn) {
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, app->lockon_count);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, app->lockon_count);
    else if (short_press && event->key == InputKeyOk && app->lockon_count) {
      az_apply_selected_lockon(app);
      return true;
    }
  } else if (app->screen == AzScreenCategories) {
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, app->list_count);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, app->list_count);
    else if (short_press && event->key == InputKeyLeft) {
      az_open_search(app);
      return true;
    } else if (short_press && event->key == InputKeyOk && app->list_count) {
      if (az_db_get_category(app->storage, app->selection,
                             &app->current_category)) {
        app->screen_selection[AzScreenCategories] = app->selection;
        app->screen_selection[AzScreenFigures] = 0;
        app->selection = 0;
        az_ui_show(app, AzScreenFigures);
        return true;
      }
    }
  } else if (app->screen == AzScreenFigures) {
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, app->list_count);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, app->list_count);
    else if (short_press && event->key == InputKeyLeft) {
      az_open_search(app);
      return true;
    } else if (short_press && event->key == InputKeyOk && app->list_count) {
      AzFigure figure;
      if (az_db_get_figure(app->storage, app->current_category.id,
                           app->selection, &figure)) {
        app->return_screen = AzScreenFigures;
        app->return_selection = app->selection;
        app->screen_selection[AzScreenFigures] = app->selection;
        app->current_figure = figure;
        app->current_is_saved = false;
        app->current_saved_filename[0] = '\0';
        app->current_lockon_valid = false;
        app->current_lockon_filename[0] = '\0';
        app->screen_selection[AzScreenFigure] = 0;
        app->selection = 0;
        az_ui_show(app, AzScreenFigure);
        return true;
      }
    }
  } else if (app->screen == AzScreenSearchResults) {
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, app->list_count);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, app->list_count);
    else if (short_press && event->key == InputKeyLeft) {
      az_open_search(app);
      return true;
    } else if (short_press && event->key == InputKeyOk && app->list_count) {
      AzFigure figure;
      if (az_db_search_get(app->storage, app->query, app->selection, &figure)) {
        app->return_screen = AzScreenSearchResults;
        app->return_selection = app->selection;
        app->screen_selection[AzScreenSearchResults] = app->selection;
        app->current_figure = figure;
        app->current_is_saved = false;
        app->current_saved_filename[0] = '\0';
        app->current_lockon_valid = false;
        app->current_lockon_filename[0] = '\0';
        app->screen_selection[AzScreenFigure] = 0;
        app->selection = 0;
        az_ui_show(app, AzScreenFigure);
        return true;
      }
    }
  } else if (app->screen == AzScreenSaved) {
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, app->saved_count);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, app->saved_count);
    else if (short_press && event->key == InputKeyOk) {
      az_open_current_saved(app);
      return true;
    }
  } else if (app->screen == AzScreenFigure) {
    bool v3 = az_figure_is_v3(app->current_figure.id);
    uint8_t action_count = az_figure_action_count(app->current_is_saved, v3);
    if (event->key == InputKeyUp)
      az_move_selection(app, -1, action_count);
    else if (event->key == InputKeyDown)
      az_move_selection(app, 1, action_count);
    else if (short_press && event->key == InputKeyOk) {
      if (app->current_is_saved) {
        if (app->selection == 0)
          az_request_emulation(app, true, false);
        else if (app->selection == 1)
          az_navigate(app, AzScreenDumpInfo, true);
        else if (app->selection == 2)
          az_open_games(app);
        else if (v3 && app->selection == 3) {
          az_open_lockon_selector(app, AzLockOnActionReplaceSaved);
        } else if ((v3 && app->selection == 4) ||
                   (!v3 && app->selection == 3)) {
          az_open_rename(app);
        } else if ((v3 && app->selection == 5) ||
                   (!v3 && app->selection == 4)) {
          az_request_emulation(app, true, true);
        } else if ((v3 && app->selection == 6) ||
                   (!v3 && app->selection == 5)) {
          az_navigate(app, AzScreenConfirmDelete, true);
        }
      } else {
        if (app->selection == 0)
          az_request_emulation(app, false, true);
        else if (app->selection == 1)
          az_request_emulation(app, true, true);
        else if (app->selection == 2)
          az_open_games(app);
      }
      return true;
    }
  } else if (app->screen == AzScreenDumpInfo || app->screen == AzScreenStatus ||
             app->screen == AzScreenAbout) {
    if (event->key == InputKeyUp) {
      if (app->detail_scroll > 0)
        app->detail_scroll--;
    } else if (event->key == InputKeyDown) {
      if (app->detail_scroll < UINT16_MAX)
        app->detail_scroll++;
    } else if (app->screen == AzScreenStatus && short_press &&
               event->key == InputKeyOk) {
      az_status_refresh(app);
      return true;
    }
  } else if (app->screen == AzScreenGames) {
    if (event->key == InputKeyUp) {
      if (app->detail_scroll > 0)
        app->detail_scroll--;
    } else if (event->key == InputKeyDown) {
      if (app->detail_scroll < UINT16_MAX)
        app->detail_scroll++;
    } else if (event->key == InputKeyLeft && app->game_count) {
      if (app->selection > 0)
        app->selection--;
      app->detail_scroll = 0;
      app->animation = 0;
    } else if (event->key == InputKeyRight && app->game_count) {
      if (app->selection + 1 < app->game_count)
        app->selection++;
      app->detail_scroll = 0;
      app->animation = 0;
    }
  } else if (app->screen == AzScreenEmulate) {
    if (short_press && event->key == InputKeyOk) {
      az_emulation_randomize_uid(app);
      return true;
    }
  } else if (app->screen == AzScreenConfirmDelete) {
    if (short_press && event->key == InputKeyOk) {
      uint16_t saved_selection = app->screen_selection[AzScreenSaved];
      if (az_saved_delete(app->storage, app->current_saved_filename)) {
        if (!az_refresh_saved_catalog(app)) {
          az_ui_toast(app, "Deleted; catalog refresh failed");
          az_navigate(app, AzScreenHome, false);
          return true;
        }
        if (saved_selection >= app->saved_count && app->saved_count)
          saved_selection = (uint16_t)(app->saved_count - 1);
        app->screen_selection[AzScreenSaved] = saved_selection;
        app->selection = saved_selection;
        az_ui_show(app, AzScreenSaved);
        az_ui_toast(app, "Figure deleted");
      } else {
        az_ui_toast(app, "Delete failed");
      }
      return true;
    }
  }

  az_ui_refresh(app);
  return true;
}

/**
 * @brief Advance UI animation, toast timing, and asynchronous database
 * completion handling.
 * @param context Caller-owned callback context.
 */
static void az_tick_callback(void *context) {
  AmiiboZeroApp *app = context;
  if (app->toast_ticks)
    app->toast_ticks--;
  app->animation++;
  if (app->db_thread && app->db_thread_done) {
    furi_thread_join(app->db_thread);
    furi_thread_free(app->db_thread);
    app->db_thread = NULL;
    app->index_ready = app->db_thread_result;
    app->index_count = app->db_thread_count;
    AzScreen target = app->db_thread_return_screen;
    app->selection = app->screen_selection[target];
    app->detail_scroll = 0;
    az_ui_show(app, target);
    az_ui_toast(app, app->index_ready ? "Database ready"
                                      : "Database prepare failed");
    return;
  }
  az_ui_refresh(app);
}

/**
 * @brief Display a temporary status message in the UI.
 * @param app Application state.
 * @param text Text to process or display.
 */
void az_ui_toast(AmiiboZeroApp *app, const char *text) {
  if (!app)
    return;
  az_str_copy(app->toast, sizeof(app->toast), text ? text : "");
  app->toast_ticks = 8;
  app->animation = 0;
  az_ui_refresh(app);
}

/**
 * @brief Navigate to a screen and request a redraw.
 * @param app Application state.
 * @param screen Screen to activate.
 */
void az_ui_show(AmiiboZeroApp *app, AzScreen screen) {
  app->screen = screen;
  app->animation = 0;
  az_ui_refresh(app);
  view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
}

/**
 * @brief Refresh the active view model from application state.
 * @param app Application state.
 */
void az_ui_refresh(AmiiboZeroApp *app) {
  if (!app || !app->main_view)
    return;
  with_view_model(
      app->main_view, AzViewModel * model,
      {
        memset(model, 0, sizeof(*model));
        model->app = app;
        model->screen = app->screen;
        model->selection = app->selection;
        model->category = app->current_category;
        model->figure = app->current_figure;
        model->figure_saved = app->current_is_saved;
        az_str_copy(model->saved_filename, sizeof(model->saved_filename),
                    app->current_saved_filename);
        az_str_copy(model->query, sizeof(model->query), app->query);
        model->animation = app->animation;
        model->db_progress = app->db_progress;
        model->db_progress_stage = app->db_progress_stage;

        uint16_t total = 0;
        if (app->screen == AzScreenCategories) {
          az_db_get_category_window(app->storage, app->selection,
                                    model->category_rows, &model->row_count,
                                    &model->window_start, &total);
          app->list_count = total;
          model->count = total;
        } else if (app->screen == AzScreenFigures) {
          az_db_get_figure_window(app->storage, app->current_category.id,
                                  app->selection, model->figure_rows,
                                  &model->row_count, &model->window_start,
                                  &total);
          app->list_count = total;
          model->count = total;
        } else if (app->screen == AzScreenSearchResults) {
          az_db_search_window(app->storage, app->query, app->selection,
                              model->figure_rows, &model->row_count,
                              &model->window_start, &total);
          app->list_count = total;
          model->count = total;
        } else if (app->screen == AzScreenSaved) {
          model->count = app->saved_count;
        } else if (app->screen == AzScreenLockOn) {
          model->count = app->lockon_count;
        } else if (app->screen == AzScreenGames) {
          model->games_total = app->game_count;
          if (app->game_entries && app->game_count &&
              app->selection < app->game_count) {
            model->games[0] = app->game_entries[app->selection];
          }
        }
        if (app->toast_ticks)
          az_str_copy(model->status_line, sizeof(model->status_line),
                      app->toast);
      },
      true);
}

/**
 * @brief Allocate and configure UI views, callbacks, and dispatcher state.
 * @param app Application state.
 */
void az_ui_init(AmiiboZeroApp *app) {
  app->ui_scratch = furi_string_alloc();
  app->dispatcher = view_dispatcher_alloc();
  view_dispatcher_set_event_callback_context(app->dispatcher, app);
  view_dispatcher_set_navigation_event_callback(app->dispatcher,
                                                az_navigation_callback);
  view_dispatcher_set_tick_event_callback(app->dispatcher, az_tick_callback,
                                          250);

  app->main_view = view_alloc();
  view_allocate_model(app->main_view, ViewModelTypeLocking,
                      sizeof(AzViewModel));
  view_set_context(app->main_view, app);
  view_set_draw_callback(app->main_view, az_draw_callback);
  view_set_input_callback(app->main_view, az_main_input);
  view_dispatcher_add_view(app->dispatcher, AZ_VIEW_MAIN, app->main_view);

  app->text_input = text_input_alloc();
  text_input_set_result_callback(app->text_input, az_search_done, app,
                                 app->text_buffer, sizeof(app->text_buffer),
                                 false);
  view_dispatcher_add_view(app->dispatcher, AZ_VIEW_SEARCH,
                           text_input_get_view(app->text_input));

  app->byte_input = byte_input_alloc();
  view_dispatcher_add_view(app->dispatcher, AZ_VIEW_MANUAL_ID,
                           byte_input_get_view(app->byte_input));

  view_dispatcher_attach_to_gui(app->dispatcher, app->gui,
                                ViewDispatcherTypeFullscreen);
  app->screen = AzScreenHome;
  app->selection = 0;
  app->detail_scroll = 0;
  az_ui_refresh(app);
  view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
}

/**
 * @brief Release UI resources owned by the application.
 * @param app Application state.
 */
void az_ui_deinit(AmiiboZeroApp *app) {
  if (!app || !app->dispatcher)
    return;
  az_clear_games(app);
  az_clear_saved_catalog(app);
  az_clear_lockon_catalog(app);
  if (app->db_thread) {
    furi_thread_join(app->db_thread);
    furi_thread_free(app->db_thread);
    app->db_thread = NULL;
  }
  view_dispatcher_remove_view(app->dispatcher, AZ_VIEW_MANUAL_ID);
  byte_input_free(app->byte_input);
  app->byte_input = NULL;
  view_dispatcher_remove_view(app->dispatcher, AZ_VIEW_SEARCH);
  text_input_free(app->text_input);
  app->text_input = NULL;
  view_dispatcher_remove_view(app->dispatcher, AZ_VIEW_MAIN);
  view_free(app->main_view);
  app->main_view = NULL;
  view_dispatcher_free(app->dispatcher);
  app->dispatcher = NULL;
  furi_string_free(app->ui_scratch);
  app->ui_scratch = NULL;
}
