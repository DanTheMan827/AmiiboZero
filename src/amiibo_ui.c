/**
 * @file amiibo_ui.c
 * @brief Category-first 128x64 user interface, navigation, emulation controls, and readable text rendering.
 */

#include "./amiibo_zero.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>

#define AZ_VIEW_MAIN 0
#define AZ_VIEW_SEARCH 1
#define AZ_UI_CONTENT_TOP 12
#define AZ_UI_FOOTER_TOP 54
#define AZ_UI_LIST_WIDTH 116
#define AZ_UI_DETAIL_LINES 4
#define AZ_UI_DETAIL_LINE_HEIGHT 9

static void az_search_done(void* context);

/**
 * @brief Select the toast text or fallback title shown in the header.
 */
static const char* az_header_title(const AzViewModel* model, const char* fallback) {
    return model->status_line[0] ? model->status_line : fallback;
}

/**
 * @brief Reuse the app-owned FuriString for allocation-free drawing helpers.
 */
static void az_scratch_set(AmiiboZeroApp* app, const char* text) {
    if(!app || !app->ui_scratch) return;
    furi_string_set_str(app->ui_scratch, text ? text : "");
}

/**
 * @brief Draw the inverse header and horizontally scroll long titles.
 */
static void az_draw_header(Canvas* canvas, const AzViewModel* model, const char* title) {
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

/**
 * @brief Draw left, center, and right footer labels without entering content space.
 */
static void az_draw_footer(Canvas* canvas, const char* left, const char* center, const char* right) {
    canvas_set_font(canvas, FontSecondary);
    if(left && left[0]) canvas_draw_str_aligned(canvas, 1, 63, AlignLeft, AlignBottom, left);
    if(center && center[0]) canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, center);
    if(right && right[0]) canvas_draw_str_aligned(canvas, 127, 63, AlignRight, AlignBottom, right);
}

/**
 * @brief Draw one line after fitting it to a pixel width.
 */
static void az_draw_fitted(Canvas* canvas, AmiiboZeroApp* app, int x, int y, const char* text, size_t width) {
    az_scratch_set(app, text);
    elements_string_fit_width(canvas, app->ui_scratch, width);
    canvas_draw_str(canvas, x, y, furi_string_get_cstr(app->ui_scratch));
}

/**
 * @brief Draw one horizontally scrollable text line.
 */
static void az_draw_marquee(
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

/**
 * @brief Draw one normal or selected list row with readable overflow behavior.
 */
static void az_draw_list_row(
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
        az_draw_marquee(canvas, app, 4, baseline, text, AZ_UI_LIST_WIDTH, model->animation, false);
        canvas_set_color(canvas, ColorBlack);
    } else {
        az_draw_fitted(canvas, app, 4, baseline, text, AZ_UI_LIST_WIDTH);
    }
}

/**
 * @brief Draw the list scrollbar when more rows exist than fit on screen.
 */
static void az_draw_list_scrollbar(Canvas* canvas, uint16_t selection, uint16_t total) {
    if(total > AZ_LIST_ROWS) elements_scrollbar_pos(canvas, 125, AZ_UI_CONTENT_TOP, 40, selection, total);
}

/**
 * @brief Render the four-item main menu.
 */
static void az_draw_home(Canvas* canvas, const AzViewModel* model) {
    static const char* labels[] = {"Browse library", "Saved figures", "Setup & status", "About"};
    az_draw_header(canvas, model, AZ_APP_NAME);
    for(uint8_t i = 0; i < COUNT_OF(labels); i++) {
        az_draw_list_row(canvas, model, i, labels[i], model->selection == i);
    }
    az_draw_footer(canvas, "", "OK", "");
}

/**
 * @brief Render the alphabetical category-first library screen.
 */
static void az_draw_categories(Canvas* canvas, const AzViewModel* model) {
    az_draw_header(canvas, model, "Library categories");
    for(uint8_t i = 0; i < model->row_count; i++) {
        char label[AZ_NAME_MAX + 16];
        snprintf(
            label,
            sizeof(label),
            "%s  (%u)",
            model->category_rows[i].name,
            model->category_rows[i].count);
        uint16_t absolute = (uint16_t)(model->window_start + i);
        az_draw_list_row(canvas, model, i, label, absolute == model->selection);
    }
    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "No categories");
    }
    az_draw_list_scrollbar(canvas, model->selection, model->count);
    az_draw_footer(canvas, "< Search", "OK", "");
}

/**
 * @brief Render category figures or search results using the shared list layout.
 */
static void az_draw_figures(Canvas* canvas, const AzViewModel* model, bool search_results) {
    az_draw_header(canvas, model, search_results ? "Search results" : model->category.name);
    for(uint8_t i = 0; i < model->row_count; i++) {
        uint16_t absolute = (uint16_t)(model->window_start + i);
        az_draw_list_row(
            canvas,
            model,
            i,
            model->figure_rows[i].name,
            absolute == model->selection);
    }
    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "No entries");
    }
    az_draw_list_scrollbar(canvas, model->selection, model->count);
    az_draw_footer(canvas, "< Search", "OK", "");
}

/**
 * @brief Render the bounded saved-figure list.
 */
static void az_draw_saved(Canvas* canvas, const AzViewModel* model) {
    az_draw_header(canvas, model, "Saved figures");
    AmiiboZeroApp* app = model->app;
    uint16_t start = model->selection >= 2 ? (uint16_t)(model->selection - 2) : 0;
    if(app->saved_count > AZ_LIST_ROWS && start + AZ_LIST_ROWS > app->saved_count) {
        start = (uint16_t)(app->saved_count - AZ_LIST_ROWS);
    }
    for(uint8_t row = 0; row < AZ_LIST_ROWS && start + row < app->saved_count; row++) {
        const AzSavedEntry* entry = &app->saved_entries[start + row];
        az_draw_list_row(canvas, model, row, entry->display_name, start + row == model->selection);
    }
    if(model->count == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "No saved figures yet");
        canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignCenter, "Browse to create one");
    }
    az_draw_list_scrollbar(canvas, model->selection, model->count);
    az_draw_footer(canvas, "", "OK", "");
}

/**
 * @brief Return the number of available actions for saved or unsaved figures.
 */
static uint8_t az_figure_action_count(bool saved) {
    return saved ? 4 : 3;
}

/**
 * @brief Return the label for one figure action index.
 */
static const char* az_figure_action(bool saved, uint8_t index) {
    if(saved) {
        static const char* actions[] = {
            "Emulate + autosave",
            "Compatibility",
            "Fresh copy",
            "Delete",
        };
        return index < COUNT_OF(actions) ? actions[index] : "";
    }
    static const char* actions[] = {"Emulate once", "Save + emulate", "Compatibility"};
    return index < COUNT_OF(actions) ? actions[index] : "";
}

/**
 * @brief Render selected figure metadata and actions.
 */
static void az_draw_figure(Canvas* canvas, const AzViewModel* model) {
    AmiiboZeroApp* app = model->app;
    az_draw_header(canvas, model, "Figure");
    canvas_set_font(canvas, FontPrimary);
    az_draw_marquee(canvas, app, 2, 21, model->figure.name, 124, model->animation, true);
    canvas_set_font(canvas, FontSecondary);
    char idline[32];
    snprintf(idline, sizeof(idline), "ID %s", model->figure.id_hex);
    az_draw_fitted(canvas, app, 2, 30, idline, 124);

    uint8_t count = az_figure_action_count(model->figure_saved);
    uint8_t start = model->selection >= 2 ? (uint8_t)(model->selection - 2) : 0;
    if(count > 3 && start + 3 > count) start = (uint8_t)(count - 3);
    for(uint8_t row = 0; row < 3 && start + row < count; row++) {
        uint8_t index = (uint8_t)(start + row);
        int top = 32 + row * 10;
        int baseline = top + 8;
        bool selected = index == model->selection;
        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, 1, top, 124, 10, 2);
            canvas_set_color(canvas, ColorWhite);
            az_draw_marquee(
                canvas,
                app,
                4,
                baseline,
                az_figure_action(model->figure_saved, index),
                118,
                model->animation,
                false);
            canvas_set_color(canvas, ColorBlack);
        } else {
            az_draw_fitted(
                canvas,
                app,
                4,
                baseline,
                az_figure_action(model->figure_saved, index),
                118);
        }
    }
    if(count > 3) elements_scrollbar_pos(canvas, 125, 32, 30, model->selection, count);
}

/**
 * @brief Return the encoded byte length implied by a UTF-8 leading byte.
 */
static size_t az_utf8_char_len(unsigned char value) {
    if((value & 0x80U) == 0) return 1;
    if((value & 0xE0U) == 0xC0U) return 2;
    if((value & 0xF0U) == 0xE0U) return 3;
    if((value & 0xF8U) == 0xF0U) return 4;
    return 1;
}

/**
 * @brief Find the next pixel-bounded wrapped line without splitting UTF-8 code units.
 */
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

/**
 * @brief Count wrapped detail lines for scrollbar and vertical-scroll bounds.
 */
static uint16_t az_wrapped_line_count(Canvas* canvas, const char* text, size_t width) {
    if(!text || !text[0]) return 0;
    const char* cursor = text;
    char line[96];
    uint16_t count = 0;
    while(az_wrap_next_line(canvas, &cursor, width, line, sizeof(line))) count++;
    return count;
}

/**
 * @brief Draw vertically scrollable wrapped detail text in the content region.
 */
static void az_draw_wrapped_text(
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

/**
 * @brief Render the current compatibility record with wrapped usage text.
 */
static void az_draw_games(Canvas* canvas, const AzViewModel* model) {
    AmiiboZeroApp* app = model->app;
    char title[48];
    if(model->games_total) {
        snprintf(title, sizeof(title), "Compatibility %u/%u", model->selection + 1, model->games_total);
    } else {
        az_str_copy(title, sizeof(title), "Compatibility");
    }
    az_draw_header(canvas, model, title);

    if(model->games_total == 0) {
        az_draw_wrapped_text(
            canvas,
            app,
            "No compatibility records were found for this Amiibo in games_info.json.",
            20,
            AZ_UI_DETAIL_LINES,
            120);
    } else {
        char detail[AZ_NAME_MAX + AZ_USAGE_MAX + 64];
        snprintf(
            detail,
            sizeof(detail),
            "%s\n%s - %s\n%s",
            model->games[0].name,
            model->games[0].platform,
            model->games[0].writes ? "writes tag data" : "read only",
            model->games[0].usage);
        az_draw_wrapped_text(canvas, app, detail, 20, AZ_UI_DETAIL_LINES, 120);
    }
    az_draw_footer(canvas, "< Prev", "Up/Dn", "Next >");
}

/**
 * @brief Render the active NFC emulation status screen.
 */
static void az_draw_emulate(Canvas* canvas, const AzViewModel* model) {
    AmiiboZeroApp* app = model->app;
    az_draw_header(canvas, model, "Emulating");
    canvas_set_font(canvas, FontPrimary);
    az_draw_marquee(canvas, app, 2, 21, model->figure.name, 124, model->animation, true);
    int radius = 4 + ((model->animation / 2) % 2);
    canvas_draw_circle(canvas, 15, 43, radius);
    canvas_draw_circle(canvas, 15, 43, radius + 4);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 29, 46, model->figure_saved ? "Writes autosave" : "Temporary session");
    az_draw_footer(canvas, "Back", "", "");
}

/**
 * @brief Render key/database/index setup status and refresh guidance.
 */
static void az_draw_status(Canvas* canvas, const AzViewModel* model) {
    AmiiboZeroApp* app = model->app;
    az_draw_header(canvas, model, "Setup & status");
    char text[384];
    snprintf(
        text,
        sizeof(text),
        "%s\n%s\n%s\n%s\nRaw JSON is parsed on-device with lwJSON's fixed-memory streaming parser. The optional amiibo.idx cache is %s at build time. OK reloads keys and refreshes database state%s.",
        app->keys.valid ? "Keys: ready" : "Keys: missing/invalid",
        storage_file_exists(app->storage, AZ_AMIIBO_JSON) ? "Amiibo JSON: found" : "Amiibo JSON: missing",
        app->index_ready ? "Database: ready" : "Database: unavailable",
        storage_file_exists(app->storage, AZ_GAMES_JSON) ? "Games JSON: found" : "Games JSON: missing",
        AZ_FEATURE_JSON_INDEX ? "enabled" : "disabled",
        AZ_FEATURE_JSON_INDEX ? " and rebuilds the index" : "");
    az_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
    az_draw_footer(canvas, "Back", "OK", "");
}

/**
 * @brief Render version, architecture, and data-source information.
 */
static void az_draw_about(Canvas* canvas, const AzViewModel* model) {
    AmiiboZeroApp* app = model->app;
    az_draw_header(canvas, model, "About");
    char text[320];
    snprintf(
        text,
        sizeof(text),
        "Amiibo Zero %s\nC implementation with category-first browsing, pixel-safe scrolling UI, Amiibo generation, native NFC persistence, and compatibility lookup.\nAmiiboAPI JSON is streamed on-device with lwJSON. Optional .idx caching defaults off. Crypto: mbedTLS.\nKeys and Amiibo dumps are not bundled.",
        AZ_APP_VERSION);
    az_draw_wrapped_text(canvas, app, text, 20, AZ_UI_DETAIL_LINES, 120);
    az_draw_footer(canvas, "Back", "Up/Dn", "");
}

/**
 * @brief Render saved-figure deletion confirmation.
 */
static void az_draw_delete(Canvas* canvas, const AzViewModel* model) {
    AmiiboZeroApp* app = model->app;
    az_draw_header(canvas, model, "Delete saved figure?");
    canvas_set_font(canvas, FontPrimary);
    az_draw_marquee(canvas, app, 2, 24, model->figure.name, 124, model->animation, true);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "This cannot be undone.");
    az_draw_footer(canvas, "Back", "OK Del", "");
}

/**
 * @brief Dispatch view drawing to the renderer for the current screen.
 */
static void az_draw_callback(Canvas* canvas, void* context) {
    const AzViewModel* model = context;
    canvas_clear(canvas);
    switch(model->screen) {
    case AzScreenHome: az_draw_home(canvas, model); break;
    case AzScreenCategories: az_draw_categories(canvas, model); break;
    case AzScreenFigures: az_draw_figures(canvas, model, false); break;
    case AzScreenSearchResults: az_draw_figures(canvas, model, true); break;
    case AzScreenSaved: az_draw_saved(canvas, model); break;
    case AzScreenFigure: az_draw_figure(canvas, model); break;
    case AzScreenGames: az_draw_games(canvas, model); break;
    case AzScreenEmulate: az_draw_emulate(canvas, model); break;
    case AzScreenStatus: az_draw_status(canvas, model); break;
    case AzScreenAbout: az_draw_about(canvas, model); break;
    case AzScreenConfirmDelete: az_draw_delete(canvas, model); break;
    default: break;
    }
}

/**
 * @brief Store only the basename of a persistent NFC path in app state.
 */
static void az_set_saved_filename_from_path(AmiiboZeroApp* app, const char* path) {
    const char* slash = strrchr(path, '/');
    az_str_copy(app->current_saved_filename, sizeof(app->current_saved_filename), slash ? slash + 1 : path);
}

/**
 * @brief Prepare saved or fresh NFC data, start the listener, and enter emulation UI.
 */
static bool az_begin_emulation(AmiiboZeroApp* app, bool persistent, bool fresh) {
    if(!app->keys.valid && fresh) {
        az_ui_toast(app, "Add key_retail.bin first");
        return false;
    }

    nfc_device_clear(app->nfc_device);
    char path[AZ_PATH_MAX] = {0};
    bool ok = false;
    if(!fresh && app->current_is_saved) {
        snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, app->current_saved_filename);
        ok = az_nfc_load_device(app->nfc_device, path);
    } else {
        ok = az_nfc_generate_device(app->nfc_device, &app->current_figure, &app->keys);
        if(ok && persistent) {
            az_make_unique_save_path(app->storage, &app->current_figure, path, sizeof(path));
            ok = az_nfc_save_device(app->nfc_device, path);
            if(ok) {
                app->current_is_saved = true;
                az_set_saved_filename_from_path(app, path);
            }
        }
    }
    if(!ok) {
        az_ui_toast(app, "Could not prepare figure");
        return false;
    }

    const NfcDeviceData* data = nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
    app->listener = nfc_listener_alloc(app->nfc, NfcProtocolMfUltralight, data);
    if(!app->listener) {
        az_ui_toast(app, "NFC listener unavailable");
        return false;
    }
    nfc_listener_start(app->listener, NULL, NULL);
    app->emulating = true;
    app->emulation_persistent = persistent || app->current_is_saved;
    if(app->emulation_persistent) {
        if(path[0]) az_str_copy(app->emulation_path, sizeof(app->emulation_path), path);
        else snprintf(app->emulation_path, sizeof(app->emulation_path), "%s/%s", AZ_FIGURES_DIR, app->current_saved_filename);
    } else {
        app->emulation_path[0] = 0;
    }
    app->detail_scroll = 0;
    az_ui_show(app, AzScreenEmulate);
    return true;
}

/**
 * @brief Stop NFC emulation, synchronize listener writes, and persist when requested.
 */
void az_emulation_stop(AmiiboZeroApp* app) {
    if(!app || !app->emulating || !app->listener) return;
    nfc_listener_stop(app->listener);
    if(app->emulation_persistent) {
        const NfcDeviceData* data = nfc_listener_get_data(app->listener, NfcProtocolMfUltralight);
        nfc_device_set_data(app->nfc_device, NfcProtocolMfUltralight, data);
        if(!az_nfc_save_device(app->nfc_device, app->emulation_path)) {
            az_str_copy(app->toast, sizeof(app->toast), "Autosave failed");
            app->toast_ticks = 10;
        } else {
            az_str_copy(app->toast, sizeof(app->toast), "Saved changes");
            app->toast_ticks = 6;
        }
    }
    nfc_listener_free(app->listener);
    app->listener = NULL;
    app->emulating = false;
    app->emulation_persistent = false;
}

/**
 * @brief Configure and display the search TextInput module.
 */
static void az_open_search(AmiiboZeroApp* app) {
    az_str_copy(app->text_buffer, sizeof(app->text_buffer), app->query);
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Search name or Amiibo ID");
    text_input_set_minimum_length(app->text_input, 0);
    text_input_set_result_callback(
        app->text_input,
        az_search_done,
        app,
        app->text_buffer,
        sizeof(app->text_buffer),
        false);
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_SEARCH);
}

/**
 * @brief Handle completion of TextInput and open search results or categories.
 */
static void az_search_done(void* context) {
    AmiiboZeroApp* app = context;
    az_str_copy(app->query, sizeof(app->query), app->text_buffer);
    app->selection = 0;
    app->detail_scroll = 0;
    az_ui_show(app, app->query[0] ? AzScreenSearchResults : AzScreenCategories);
}

/**
 * @brief Handle dispatcher-level Back navigation requests.
 */
static bool az_navigation_callback(void* context) {
    AmiiboZeroApp* app = context;
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
    return true;
}

/**
 * @brief Load the currently selected saved entry into figure-detail state.
 */
static void az_open_current_saved(AmiiboZeroApp* app) {
    if(app->saved_count == 0 || app->selection >= app->saved_count) return;
    const AzSavedEntry* entry = &app->saved_entries[app->selection];
    memset(&app->current_figure, 0, sizeof(app->current_figure));
    memcpy(app->current_figure.id, entry->id, 8);
    app->current_figure.category = entry->id[6];
    if(!az_db_find_by_id(app->storage, entry->id, &app->current_figure)) {
        az_str_copy(app->current_figure.name, sizeof(app->current_figure.name), entry->display_name);
        static const char hex[] = "0123456789abcdef";
        for(size_t i = 0; i < 8; i++) {
            app->current_figure.id_hex[i * 2] = hex[entry->id[i] >> 4];
            app->current_figure.id_hex[i * 2 + 1] = hex[entry->id[i] & 0x0F];
        }
        app->current_figure.id_hex[16] = 0;
    }
    app->current_is_saved = true;
    az_str_copy(app->current_saved_filename, sizeof(app->current_saved_filename), entry->filename);
    app->selection = 0;
    app->detail_scroll = 0;
    az_ui_show(app, AzScreenFigure);
}

/**
 * @brief Stream compatibility data for the current figure and open the games screen.
 */
static void az_open_games(AmiiboZeroApp* app) {
    app->game_count = 0;
    if(!az_db_load_games(
           app->storage,
           app->current_figure.id,
           app->game_entries,
           AZ_MAX_GAMES,
           &app->game_count)) {
        az_ui_toast(app, "Could not read games DB");
    }
    app->selection = 0;
    app->detail_scroll = 0;
    az_ui_show(app, AzScreenGames);
}

/**
 * @brief Reload keys and refresh raw-JSON or optional-index database state.
 */
static void az_status_refresh(AmiiboZeroApp* app) {
    app->keys.valid = az_keys_load(app->storage, &app->keys);
    uint32_t count = 0;
    app->index_ready = az_db_ensure_index(app->storage, true, &count);
    app->index_count = count;
    app->detail_scroll = 0;
    az_ui_toast(
        app,
        app->index_ready ?
            (AZ_FEATURE_JSON_INDEX ? "Device index rebuilt" : "Raw JSON refreshed") :
            (AZ_FEATURE_JSON_INDEX ? "Index rebuild failed" : "JSON refresh failed"));
}

/**
 * @brief Move a bounded selection by one row while resetting marquee animation.
 */
static void az_move_selection(AmiiboZeroApp* app, int direction, uint16_t count) {
    if(count == 0) {
        app->selection = 0;
        return;
    }
    uint16_t old = app->selection;
    if(direction < 0 && app->selection > 0) app->selection--;
    else if(direction > 0 && app->selection + 1 < count) app->selection++;
    if(old != app->selection) app->animation = 0;
}

/**
 * @brief Handle directional, OK, Back, and repeat input for every app screen.
 */
static bool az_main_input(InputEvent* event, void* context) {
    AmiiboZeroApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    bool short_press = event->type == InputTypeShort;

    if(short_press && event->key == InputKeyBack) {
        if(app->screen == AzScreenHome) {
            view_dispatcher_stop(app->dispatcher);
        } else if(app->screen == AzScreenCategories) {
            app->selection = 0;
            az_ui_show(app, AzScreenHome);
        } else if(app->screen == AzScreenFigures || app->screen == AzScreenSearchResults) {
            app->selection = 0;
            az_ui_show(app, AzScreenCategories);
        } else if(app->screen == AzScreenSaved) {
            app->selection = 1;
            az_ui_show(app, AzScreenHome);
        } else if(app->screen == AzScreenFigure) {
            if(app->current_is_saved) {
                app->saved_count = az_saved_scan(app->storage, app->saved_entries, AZ_MAX_SAVED);
                app->selection = 0;
                az_ui_show(app, AzScreenSaved);
            } else {
                app->selection = app->return_selection;
                az_ui_show(app, app->return_screen);
            }
        } else if(app->screen == AzScreenGames) {
            app->selection = 0;
            app->detail_scroll = 0;
            az_ui_show(app, AzScreenFigure);
        } else if(app->screen == AzScreenEmulate) {
            az_emulation_stop(app);
            app->selection = 0;
            az_ui_show(app, AzScreenFigure);
        } else if(app->screen == AzScreenStatus) {
            app->selection = 2;
            app->detail_scroll = 0;
            az_ui_show(app, AzScreenHome);
        } else if(app->screen == AzScreenAbout) {
            app->selection = 3;
            app->detail_scroll = 0;
            az_ui_show(app, AzScreenHome);
        } else if(app->screen == AzScreenConfirmDelete) {
            app->selection = 0;
            az_ui_show(app, AzScreenFigure);
        }
        az_ui_refresh(app);
        return true;
    }

    if(app->screen == AzScreenHome) {
        if(event->key == InputKeyUp) az_move_selection(app, -1, 4);
        else if(event->key == InputKeyDown) az_move_selection(app, 1, 4);
        else if(short_press && event->key == InputKeyOk) {
            if(app->selection == 0) {
                app->selection = 0;
                az_ui_show(app, AzScreenCategories);
            } else if(app->selection == 1) {
                app->saved_count = az_saved_scan(app->storage, app->saved_entries, AZ_MAX_SAVED);
                app->selection = 0;
                az_ui_show(app, AzScreenSaved);
            } else if(app->selection == 2) {
                app->selection = 0;
                app->detail_scroll = 0;
                az_ui_show(app, AzScreenStatus);
            } else {
                app->selection = 0;
                app->detail_scroll = 0;
                az_ui_show(app, AzScreenAbout);
            }
            return true;
        }
    } else if(app->screen == AzScreenCategories) {
        if(event->key == InputKeyUp) az_move_selection(app, -1, app->list_count);
        else if(event->key == InputKeyDown) az_move_selection(app, 1, app->list_count);
        else if(short_press && event->key == InputKeyLeft) {
            az_open_search(app);
            return true;
        } else if(short_press && event->key == InputKeyOk && app->list_count) {
            if(az_db_get_category(app->storage, app->selection, &app->current_category)) {
                app->selection = 0;
                az_ui_show(app, AzScreenFigures);
                return true;
            }
        }
    } else if(app->screen == AzScreenFigures) {
        if(event->key == InputKeyUp) az_move_selection(app, -1, app->list_count);
        else if(event->key == InputKeyDown) az_move_selection(app, 1, app->list_count);
        else if(short_press && event->key == InputKeyLeft) {
            az_open_search(app);
            return true;
        } else if(short_press && event->key == InputKeyOk && app->list_count) {
            AzFigure figure;
            if(az_db_get_figure(app->storage, app->current_category.id, app->selection, &figure)) {
                app->return_screen = AzScreenFigures;
                app->return_selection = app->selection;
                app->current_figure = figure;
                app->current_is_saved = false;
                app->current_saved_filename[0] = 0;
                app->selection = 0;
                az_ui_show(app, AzScreenFigure);
                return true;
            }
        }
    } else if(app->screen == AzScreenSearchResults) {
        if(event->key == InputKeyUp) az_move_selection(app, -1, app->list_count);
        else if(event->key == InputKeyDown) az_move_selection(app, 1, app->list_count);
        else if(short_press && event->key == InputKeyLeft) {
            az_open_search(app);
            return true;
        } else if(short_press && event->key == InputKeyOk && app->list_count) {
            AzFigure figure;
            if(az_db_search_get(app->storage, app->query, app->selection, &figure)) {
                app->return_screen = AzScreenSearchResults;
                app->return_selection = app->selection;
                app->current_figure = figure;
                app->current_is_saved = false;
                app->current_saved_filename[0] = 0;
                app->selection = 0;
                az_ui_show(app, AzScreenFigure);
                return true;
            }
        }
    } else if(app->screen == AzScreenSaved) {
        if(event->key == InputKeyUp) az_move_selection(app, -1, app->saved_count);
        else if(event->key == InputKeyDown) az_move_selection(app, 1, app->saved_count);
        else if(short_press && event->key == InputKeyOk) {
            az_open_current_saved(app);
            return true;
        }
    } else if(app->screen == AzScreenFigure) {
        uint8_t action_count = az_figure_action_count(app->current_is_saved);
        if(event->key == InputKeyUp) az_move_selection(app, -1, action_count);
        else if(event->key == InputKeyDown) az_move_selection(app, 1, action_count);
        else if(short_press && event->key == InputKeyOk) {
            if(app->current_is_saved) {
                if(app->selection == 0) az_begin_emulation(app, true, false);
                else if(app->selection == 1) az_open_games(app);
                else if(app->selection == 2) az_begin_emulation(app, true, true);
                else if(app->selection == 3) {
                    app->selection = 0;
                    az_ui_show(app, AzScreenConfirmDelete);
                }
            } else {
                if(app->selection == 0) az_begin_emulation(app, false, true);
                else if(app->selection == 1) az_begin_emulation(app, true, true);
                else if(app->selection == 2) az_open_games(app);
            }
            return true;
        }
    } else if(app->screen == AzScreenGames) {
        if(event->key == InputKeyUp) {
            if(app->detail_scroll > 0) app->detail_scroll--;
        } else if(event->key == InputKeyDown) {
            if(app->detail_scroll < UINT16_MAX) app->detail_scroll++;
        } else if(event->key == InputKeyLeft && app->game_count) {
            if(app->selection > 0) app->selection--;
            app->detail_scroll = 0;
            app->animation = 0;
        } else if(event->key == InputKeyRight && app->game_count) {
            if(app->selection + 1 < app->game_count) app->selection++;
            app->detail_scroll = 0;
            app->animation = 0;
        }
    } else if(app->screen == AzScreenStatus || app->screen == AzScreenAbout) {
        if(event->key == InputKeyUp) {
            if(app->detail_scroll > 0) app->detail_scroll--;
        } else if(event->key == InputKeyDown) {
            if(app->detail_scroll < UINT16_MAX) app->detail_scroll++;
        } else if(app->screen == AzScreenStatus && short_press && event->key == InputKeyOk) {
            az_status_refresh(app);
            return true;
        }
    } else if(app->screen == AzScreenConfirmDelete) {
        if(short_press && event->key == InputKeyOk) {
            if(az_saved_delete(app->storage, app->current_saved_filename)) {
                app->saved_count = az_saved_scan(app->storage, app->saved_entries, AZ_MAX_SAVED);
                app->selection = 0;
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
 * @brief Advance toast and marquee animation on the dispatcher tick.
 */
static void az_tick_callback(void* context) {
    AmiiboZeroApp* app = context;
    if(app->toast_ticks) app->toast_ticks--;
    app->animation++;
    az_ui_refresh(app);
}

/**
 * @brief Show a temporary header message and request a redraw.
 */
void az_ui_toast(AmiiboZeroApp* app, const char* text) {
    if(!app) return;
    az_str_copy(app->toast, sizeof(app->toast), text ? text : "");
    app->toast_ticks = 8;
    app->animation = 0;
    az_ui_refresh(app);
}

/**
 * @brief Switch application screen state and display the main view.
 */
void az_ui_show(AmiiboZeroApp* app, AzScreen screen) {
    app->screen = screen;
    app->animation = 0;
    az_ui_refresh(app);
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
}

/**
 * @brief Rebuild the lock-protected view snapshot for the current screen.
 */
void az_ui_refresh(AmiiboZeroApp* app) {
    if(!app || !app->main_view) return;
    with_view_model(
        app->main_view,
        AzViewModel * model,
        {
            memset(model, 0, sizeof(*model));
            model->app = app;
            model->screen = app->screen;
            model->selection = app->selection;
            model->category = app->current_category;
            model->figure = app->current_figure;
            model->figure_saved = app->current_is_saved;
            az_str_copy(model->saved_filename, sizeof(model->saved_filename), app->current_saved_filename);
            az_str_copy(model->query, sizeof(model->query), app->query);
            model->animation = app->animation;

            uint16_t total = 0;
            if(app->screen == AzScreenCategories) {
                az_db_get_category_window(
                    app->storage,
                    app->selection,
                    model->category_rows,
                    &model->row_count,
                    &model->window_start,
                    &total);
                app->list_count = total;
                model->count = total;
            } else if(app->screen == AzScreenFigures) {
                az_db_get_figure_window(
                    app->storage,
                    app->current_category.id,
                    app->selection,
                    model->figure_rows,
                    &model->row_count,
                    &model->window_start,
                    &total);
                app->list_count = total;
                model->count = total;
            } else if(app->screen == AzScreenSearchResults) {
                az_db_search_window(
                    app->storage,
                    app->query,
                    app->selection,
                    model->figure_rows,
                    &model->row_count,
                    &model->window_start,
                    &total);
                app->list_count = total;
                model->count = total;
            } else if(app->screen == AzScreenSaved) {
                model->count = app->saved_count;
            } else if(app->screen == AzScreenGames) {
                model->games_total = app->game_count;
                if(app->game_count && app->selection < app->game_count) model->games[0] = app->game_entries[app->selection];
            }
            if(app->toast_ticks) az_str_copy(model->status_line, sizeof(model->status_line), app->toast);
        },
        true);
}

/**
 * @brief Allocate, configure, register, and attach all UI objects.
 */
void az_ui_init(AmiiboZeroApp* app) {
    app->ui_scratch = furi_string_alloc();
    app->dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, az_navigation_callback);
    view_dispatcher_set_tick_event_callback(app->dispatcher, az_tick_callback, 250);

    app->main_view = view_alloc();
    view_allocate_model(app->main_view, ViewModelTypeLocking, sizeof(AzViewModel));
    view_set_context(app->main_view, app);
    view_set_draw_callback(app->main_view, az_draw_callback);
    view_set_input_callback(app->main_view, az_main_input);
    view_dispatcher_add_view(app->dispatcher, AZ_VIEW_MAIN, app->main_view);

    app->text_input = text_input_alloc();
    text_input_set_result_callback(
        app->text_input,
        az_search_done,
        app,
        app->text_buffer,
        sizeof(app->text_buffer),
        false);
    view_dispatcher_add_view(app->dispatcher, AZ_VIEW_SEARCH, text_input_get_view(app->text_input));

    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->screen = AzScreenHome;
    app->selection = 0;
    app->detail_scroll = 0;
    az_ui_refresh(app);
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
}

/**
 * @brief Unregister and free all UI objects in reverse ownership order.
 */
void az_ui_deinit(AmiiboZeroApp* app) {
    if(!app || !app->dispatcher) return;
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
