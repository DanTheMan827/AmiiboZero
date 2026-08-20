/**
 * @file amiibo_ui.cpp
 * @brief Category-first 128x64 user interface, navigation, emulation controls, and readable text rendering.
 */
#include <furi/core/memmgr.h>
#include <furi/core/memmgr_heap.h>

#include "./amiibo_zero.h"
#include "./amiibo_db.h"
#include "./ui/ui_common.h"
#include "./ui/controller.h"
#include "./ui/screen.h"
#include "./ui/home.h"
#include "./ui/categories.h"
#include "./ui/figures.h"
#include "./ui/search_results.h"
#include "./ui/saved.h"
#include "./ui/advanced.h"
#include "./ui/tag_operation.h"
#include "./ui/lockon.h"
#include "./ui/figure.h"
#include "./ui/dump_info.h"
#include "./ui/games.h"
#include "./ui/emulate.h"
#include "./ui/status.h"
#include "./ui/about.h"
#include "./ui/confirm_delete.h"
#include "./ui/working.h"

#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define AZ_VIEW_MAIN 0
#define AZ_VIEW_SEARCH 1
#define AZ_VIEW_MANUAL_ID 2

static void az_search_done(void* context);
static void az_rename_done(void* context);
static void az_manual_id_done(void* context);

#ifdef AZ_DEBUG_MEMORY_OVERLAY
/** Draw the heap counter last into the active app canvas without creating another GUI viewport. */
static void az_memory_overlay_draw(Canvas* canvas) {
    if(!canvas) return;
    size_t free_heap = memmgr_get_free_heap();
    size_t max_block = memmgr_heap_get_max_free_block();
    char text[24];
    snprintf(
        text,
        sizeof(text),
        "%lu/%luk",
        (unsigned long)(free_heap / 1024U),
        (unsigned long)(max_block / 1024U));
    canvas_set_font(canvas, FontSecondary);
    uint8_t width = canvas_string_width(canvas, text);
    uint8_t x = width + 2U < 128U ? (uint8_t)(128U - width - 1U) : 0U;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, x > 0U ? x - 1U : 0U, 55, width + 2U, 9);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str_aligned(canvas, 127, 63, AlignRight, AlignBottom, text);
}
#endif

static const AzUiScreen& az_ui_screen_for(AzScreen screen) {
    switch(screen) {
    case AzScreenHome: return az_ui_home_screen;
    case AzScreenCategories: return az_ui_categories_screen;
    case AzScreenFigures: return az_ui_figures_screen;
    case AzScreenSearchResults: return az_ui_search_results_screen;
    case AzScreenSaved: return az_ui_saved_screen;
    case AzScreenAdvanced: return az_ui_advanced_screen;
    case AzScreenTagOperation: return az_ui_tag_operation_screen;
    case AzScreenLockOn: return az_ui_lockon_screen;
    case AzScreenFigure: return az_ui_figure_screen;
    case AzScreenDumpInfo: return az_ui_dump_info_screen;
    case AzScreenGames: return az_ui_games_screen;
    case AzScreenEmulate: return az_ui_emulate_screen;
    case AzScreenStatus: return az_ui_status_screen;
    case AzScreenAbout: return az_ui_about_screen;
    case AzScreenConfirmDelete: return az_ui_confirm_delete_screen;
    case AzScreenWorking: return az_ui_working_screen;
    default: return az_ui_home_screen;
    }
}

/**
 * @brief Dispatch view drawing to the renderer for the current screen.
 */
static void az_draw_callback(Canvas* canvas, void* context) {
    const AzViewModel* model = static_cast<const AzViewModel*>(context);
    canvas_clear(canvas);
    az_ui_screen_for(model->screen).draw(canvas, model);
#ifdef AZ_DEBUG_MEMORY_OVERLAY
    az_memory_overlay_draw(canvas);
#endif
}

/** Save mutable cursor/scroll state into the currently visible stack entry. */
static void az_ui_stack_sync_top(AmiiboZeroApp* app) {
    if(!app || app->ui_stack_depth == 0U) return;
    AzUiStackEntry* entry = &app->ui_stack[app->ui_stack_depth - 1U];
    entry->screen = app->screen;
    entry->selection = app->selection;
    entry->detail_scroll = app->detail_scroll;
    if(app->screen < AzScreenCount) app->screen_selection[app->screen] = app->selection;
}

/** Restore and display the top stack entry without changing NFC hardware ownership. */
static void az_ui_stack_activate_top(AmiiboZeroApp* app) {
    if(!app || app->ui_stack_depth == 0U) return;
    AzUiStackEntry* entry = &app->ui_stack[app->ui_stack_depth - 1U];
    app->screen = entry->screen;
    app->selection = entry->selection;
    app->detail_scroll = entry->detail_scroll;
    app->animation = 0U;

    az_ui_screen_for(app->screen).onResume(app);
    entry->selection = app->selection;
    entry->detail_scroll = app->detail_scroll;
    if(app->screen < AzScreenCount) app->screen_selection[app->screen] = app->selection;

    az_ui_refresh(app);
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
}

/**
 * @brief Push a target screen onto the framework navigation stack.
 * @param app Application state.
 * @param screen Target screen.
 * @param reset_target Reset the target's remembered selection to zero before entering it.
 */
void az_ui_navigate(AmiiboZeroApp* app, AzScreen screen, bool reset_target) {
    if(!app || screen >= AzScreenCount) return;
    if(app->ui_stack_depth >= AZ_UI_STACK_MAX) {
        az_ui_toast(app, "Navigation stack full");
        return;
    }

    az_ui_stack_sync_top(app);
    if(reset_target) app->screen_selection[screen] = 0U;
    AzUiStackEntry* entry = &app->ui_stack[app->ui_stack_depth++];
    entry->screen = screen;
    entry->selection = app->screen_selection[screen];
    entry->detail_scroll = 0U;
    az_ui_stack_activate_top(app);
}

/** Replace only the current stack entry, invoking its pop cleanup first. */
void az_ui_replace(AmiiboZeroApp* app, AzScreen screen, bool reset_target) {
    if(!app || screen >= AzScreenCount) return;
    if(app->ui_stack_depth == 0U) {
        az_ui_navigate(app, screen, reset_target);
        return;
    }

    az_ui_stack_sync_top(app);
    const AzScreen old_screen = app->ui_stack[app->ui_stack_depth - 1U].screen;
    az_ui_screen_for(old_screen).onPopped(app);
    if(reset_target) app->screen_selection[screen] = 0U;
    AzUiStackEntry* entry = &app->ui_stack[app->ui_stack_depth - 1U];
    entry->screen = screen;
    entry->selection = app->screen_selection[screen];
    entry->detail_scroll = 0U;
    az_ui_stack_activate_top(app);
}

/**
 * @brief Remove the current screen. Removing the final entry terminates the app loop.
 * @return True when an entry was removed.
 */
bool az_ui_pop(AmiiboZeroApp* app) {
    if(!app || app->ui_stack_depth == 0U) return false;

    az_ui_stack_sync_top(app);
    const AzScreen popped = app->ui_stack[app->ui_stack_depth - 1U].screen;
    az_ui_screen_for(popped).onPopped(app);
    app->ui_stack_depth--;
    if(app->ui_stack_depth == 0U) {
        view_dispatcher_stop(app->dispatcher);
        return true;
    }

    az_ui_stack_activate_top(app);
    return true;
}

/**
 * @brief Store only the basename of a persistent NFC path in app state.
 */
static void az_set_saved_filename_from_path(AmiiboZeroApp* app, const char* path) {
    const char* slash = strrchr(path, '/');
    az_str_copy(app->current_saved_filename, sizeof(app->current_saved_filename), slash ? slash + 1 : path);
}

/**
 * @brief Allocate the lock-on catalog on demand and rescan user payload files.
 * @param app Application state.
 * @return True when the catalog buffer exists and the scan completed.
 */
bool az_ui_refresh_lockon_catalog(AmiiboZeroApp* app) {
    if(!app) return false;
    if(!app->lockon_entries) {
        app->lockon_entries = static_cast<AzLockOnEntry*>(calloc(AZ_MAX_LOCKONS, sizeof(AzLockOnEntry)));
        if(!app->lockon_entries) {
            app->lockon_count = 0;
            return false;
        }
    }
    memset(app->lockon_entries, 0, AZ_MAX_LOCKONS * sizeof(AzLockOnEntry));
    app->lockon_count = az_lockon_scan(app->storage, app->lockon_entries, AZ_MAX_LOCKONS);
    return true;
}

/**
 * @brief Release the lock-on catalog after selection/cancellation.
 * @param app Application state.
 */
void az_ui_clear_lockon_catalog(AmiiboZeroApp* app) {
    if(!app) return;
    free(app->lockon_entries);
    app->lockon_entries = NULL;
    app->lockon_count = 0;
}

/**
 * @brief Open the type-3 lock-on picker for a deferred generation/update action.
 */
bool az_ui_open_lockon_selector(AmiiboZeroApp* app, AzLockOnAction action) {
    if(!app || action == AzLockOnActionNone) return false;
    /* Saved and lock-on catalogs do not need to coexist; this keeps the picker cheap on RAM. */
    az_ui_clear_saved_catalog(app);
    if(!az_ui_refresh_lockon_catalog(app)) {
        az_ui_toast(app, "Lock-on catalog unavailable");
        return false;
    }
    app->lockon_action = action;
    if(app->screen < AzScreenCount) app->screen_selection[app->screen] = app->selection;
    if(app->screen_selection[AzScreenLockOn] >= app->lockon_count && app->lockon_count) {
        app->screen_selection[AzScreenLockOn] = (uint16_t)(app->lockon_count - 1U);
    }
    az_ui_navigate(app, AzScreenLockOn, false);
    return true;
}

/**
 * @brief Prepare saved or fresh NFC data, start the appropriate listener, and enter emulation UI.
 */
static bool az_begin_emulation(AmiiboZeroApp* app, bool persistent, bool fresh) {
    if(!app->keys.valid && fresh) {
        az_ui_toast(app, "Add key_retail.bin first");
        return false;
    }
    if(fresh && az_figure_is_v3(app->current_figure.id) && !app->current_lockon_valid) {
        az_ui_toast(app, "Select a lock-on first");
        return false;
    }

    nfc_device_clear(app->nfc_device);
    char path[AZ_PATH_MAX] = {0};
    bool ok = false;
    if(!fresh && app->current_is_saved) {
        snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, app->current_saved_filename);
        ok = az_nfc_load_device(app->nfc_device, path);
        if(ok && az_nfc_device_is_v3(app->nfc_device)) {
            app->current_lockon_valid =
                az_saved_lockon_load(app->storage, app->current_saved_filename, app->current_lockon_sram);
            app->current_lockon_filename[0] = '\0';
            ok = app->current_lockon_valid;
        }
    } else {
        ok = az_nfc_generate_device(app->nfc_device, &app->current_figure, &app->keys);
        if(ok && persistent) {
            az_make_unique_save_path(
                app->storage,
                &app->current_figure,
                az_ui_current_figure_name(app),
                path,
                sizeof(path));
            ok = path[0] && az_nfc_save_device(app->nfc_device, path);
            if(ok) {
                az_set_saved_filename_from_path(app, path);
                if(az_figure_is_v3(app->current_figure.id)) {
                    ok = app->current_lockon_valid &&
                         az_saved_lockon_save(
                             app->storage,
                             app->current_saved_filename,
                             app->current_lockon_sram);
                    if(!ok) az_saved_delete(app->storage, app->current_saved_filename);
                }
                if(ok) app->current_is_saved = true;
            }
        }
    }
    if(!ok) {
        az_ui_toast(app, "Could not prepare figure");
        return false;
    }

    if(!az_nfc_listener_start(app)) {
        az_ui_toast(app, "NFC listener unavailable");
        return false;
    }
    app->emulation_persistent = persistent || app->current_is_saved;
    if(app->emulation_persistent) {
        if(path[0]) az_str_copy(app->emulation_path, sizeof(app->emulation_path), path);
        else snprintf(
            app->emulation_path,
            sizeof(app->emulation_path),
            "%s/%s",
            AZ_FIGURES_DIR,
            app->current_saved_filename);
    } else {
        app->emulation_path[0] = '\0';
    }
    az_ui_navigate(app, AzScreenEmulate, true);
    return true;
}

/**
 * @brief Start immediately for standard/saved data or defer fresh type-3 generation to lock-on selection.
 */
bool az_ui_request_emulation(AmiiboZeroApp* app, bool persistent, bool fresh) {
    if(!app) return false;
    if(fresh && !app->keys.valid) {
        az_ui_toast(app, "Add key_retail.bin first");
        return false;
    }
    if(fresh && az_figure_is_v3(app->current_figure.id)) {
        app->current_lockon_valid = false;
        app->current_lockon_filename[0] = '\0';
        return az_ui_open_lockon_selector(
            app,
            persistent ? AzLockOnActionGeneratePersistent : AzLockOnActionGenerateTemporary);
    }
    if(!fresh && app->current_is_saved && az_figure_is_v3(app->current_figure.id)) {
        app->current_lockon_valid =
            az_saved_lockon_load(app->storage, app->current_saved_filename, app->current_lockon_sram);
        app->current_lockon_filename[0] = '\0';
        if(!app->current_lockon_valid) {
            return az_ui_open_lockon_selector(app, AzLockOnActionEmulateSaved);
        }
    }
    return az_begin_emulation(app, persistent, fresh);
}

/**
 * @brief Apply the selected lock-on payload and resume the deferred generation/update action.
 */
void az_ui_apply_selected_lockon(AmiiboZeroApp* app) {
    if(!app || !app->lockon_entries || app->selection >= app->lockon_count) return;
    AzLockOnEntry selected = app->lockon_entries[app->selection];
    uint8_t sram[AZ_LOCKON_SRAM_SIZE];
    if(!az_lockon_load(app->storage, selected.filename, sram)) {
        az_ui_toast(app, "Could not read lock-on");
        return;
    }

    memcpy(app->current_lockon_sram, sram, sizeof(sram));
    app->current_lockon_valid = true;
    az_str_copy(
        app->current_lockon_filename,
        sizeof(app->current_lockon_filename),
        selected.filename);
    app->screen_selection[AzScreenLockOn] = app->selection;
    AzLockOnAction action = app->lockon_action;
    app->lockon_action = AzLockOnActionNone;
    az_ui_clear_lockon_catalog(app);
    if(app->screen == AzScreenLockOn) az_ui_pop(app);

    if(action == AzLockOnActionGenerateTemporary) {
        az_begin_emulation(app, false, true);
        return;
    }
    if(action == AzLockOnActionGeneratePersistent) {
        az_begin_emulation(app, true, true);
        return;
    }
    if(action == AzLockOnActionEmulateSaved && app->current_is_saved) {
        bool ok = az_saved_lockon_save(
            app->storage, app->current_saved_filename, app->current_lockon_sram);
        if(ok) {
            az_begin_emulation(app, true, false);
        } else {
            app->selection = app->screen_selection[AzScreenFigure];
            az_ui_toast(app, "Could not attach lock-on");
        }
        return;
    }
    if(action == AzLockOnActionReplaceSaved && app->current_is_saved) {
        bool ok = az_saved_lockon_save(
            app->storage, app->current_saved_filename, app->current_lockon_sram);
        app->selection = app->screen_selection[AzScreenFigure];
        az_ui_toast(app, ok ? "Lock-on changed" : "Lock-on change failed");
        return;
    }
}

/**
 * @brief Stop NFC emulation, synchronize reader writes, and persist when requested.
 */
void az_emulation_stop(AmiiboZeroApp* app) {
    if(!app || !app->emulating) return;
    bool synced = az_nfc_listener_pause_and_sync(app);
    if(app->emulation_persistent && synced) {
        if(!az_nfc_save_device(app->nfc_device, app->emulation_path)) {
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
 * @brief Randomize the active Amiibo UID with RF fully stopped during decrypt/re-key/encrypt.
 * @param app Application state.
 */
void az_ui_emulation_randomize_uid(AmiiboZeroApp* app) {
    if(!app || !app->emulating) return;
    if(az_nfc_device_is_v3(app->nfc_device)) {
        az_ui_toast(app, "V3 UID is fixed");
        return;
    }
    if(!app->keys.valid) {
        az_ui_toast(app, "key_retail.bin required");
        return;
    }
    if(!az_nfc_listener_pause_and_sync(app)) {
        az_ui_toast(app, "Could not pause NFC");
        return;
    }

    bool randomized = az_nfc_randomize_uid(app->nfc_device, &app->keys);
    bool saved = true;
    if(randomized && app->emulation_persistent) {
        saved = az_nfc_save_device(app->nfc_device, app->emulation_path);
    }
    bool restarted = az_nfc_listener_start(app);
    if(!restarted) {
        app->emulation_persistent = false;
        az_ui_toast(app, "NFC restart failed");
        az_ui_pop(app);
        return;
    }
    if(!randomized) az_ui_toast(app, "UID randomize failed");
    else if(!saved) az_ui_toast(app, "UID changed; save failed");
    else az_ui_toast(app, "UID randomized");
}

/**
 * @brief Configure and display the shared TextInput module for library search.
 */
void az_ui_open_search(AmiiboZeroApp* app) {
    az_str_copy(app->text_buffer, sizeof(app->text_buffer), app->query);
    app->text_input_mode = AzTextInputSearch;
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
 * @brief Handle completion of library search TextInput.
 */
static void az_search_done(void* context) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    az_str_copy(app->query, sizeof(app->query), app->text_buffer);
    app->screen_selection[AzScreenSearchResults] = 0;
    if(app->screen == AzScreenSearchResults) {
        if(app->query[0]) {
            az_ui_replace(app, AzScreenSearchResults, true);
        } else {
            az_ui_pop(app);
        }
    } else if(app->query[0]) {
        az_ui_navigate(app, AzScreenSearchResults, true);
    } else {
        az_ui_refresh(app);
        view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
    }
}

/**
 * @brief Configure TextInput for renaming the current saved NFC file.
 * @param app Application state.
 */
void az_ui_open_rename(AmiiboZeroApp* app) {
    az_str_copy(app->text_buffer, sizeof(app->text_buffer), app->current_saved_filename);
    char* dot = strrchr(app->text_buffer, '.');
    if(dot) *dot = '\0';
    app->text_input_mode = AzTextInputRename;
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Rename saved Amiibo");
    text_input_set_minimum_length(app->text_input, 1);
    text_input_set_result_callback(
        app->text_input,
        az_rename_done,
        app,
        app->text_buffer,
        sizeof(app->text_buffer),
        false);
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_SEARCH);
}

/**
 * @brief Complete a saved-file rename and refresh the saved catalog.
 */
static void az_rename_done(void* context) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    char renamed[96];
    if(az_saved_rename(
           app->storage,
           app->current_saved_filename,
           app->text_buffer,
           renamed,
           sizeof(renamed))) {
        az_str_copy(app->current_saved_filename, sizeof(app->current_saved_filename), renamed);
        if(!az_ui_refresh_saved_catalog(app)) {
            view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
            az_ui_toast(app, "Renamed; catalog refresh failed");
            return;
        }
        app->screen_selection[AzScreenSaved] =
            az_ui_saved_selection_for_filename(app, renamed, app->screen_selection[AzScreenSaved]);
        view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
        az_ui_toast(app, "Renamed");
    } else {
        view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
        az_ui_toast(app, "Rename failed");
    }
}

/**
 * @brief Open the eight-byte hex-only manual figure-ID editor.
 * @param app Application state.
 */
void az_ui_open_manual_id(AmiiboZeroApp* app) {
    memset(app->manual_id, 0, sizeof(app->manual_id));
    byte_input_set_header_text(app->byte_input, "Manual figure ID (8 bytes)");
    byte_input_set_result_callback(
        app->byte_input,
        az_manual_id_done,
        NULL,
        app,
        app->manual_id,
        sizeof(app->manual_id));
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MANUAL_ID);
}

/**
 * @brief Build figure state from the exactly eight bytes entered in ByteInput.
 */
static void az_manual_id_done(void* context) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    memset(&app->current_figure, 0, sizeof(app->current_figure));
    if(az_db_find_by_id(app->storage, app->manual_id, &app->current_figure)) {
        furi_string_set_str(
            app->current_figure_name,
            app->current_figure.name[0] ? app->current_figure.name : "Amiibo");
    } else {
        memcpy(app->current_figure.id, app->manual_id, sizeof(app->manual_id));
        app->current_figure.category = app->manual_id[6];
        app->current_figure.type = app->manual_id[3];
        char id_hex[17];
        az_ui_format_figure_id(&app->current_figure, id_hex);
        az_str_copy(app->current_figure.id_hex, sizeof(app->current_figure.id_hex), id_hex);
        snprintf(
            app->current_figure.name,
            sizeof(app->current_figure.name),
            "Manual %s",
            id_hex);
        furi_string_set_str(app->current_figure_name, app->current_figure.name);
    }
    app->current_is_saved = false;
    app->current_saved_filename[0] = '\0';
    app->current_lockon_valid = false;
    app->current_lockon_filename[0] = '\0';
    app->screen_selection[AzScreenFigure] = 0;
    az_ui_navigate(app, AzScreenFigure, true);
}

/**
 * @brief Handle dispatcher-level Back navigation requests from module views.
 */
static bool az_navigation_callback(void* context) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    view_dispatcher_switch_to_view(app->dispatcher, AZ_VIEW_MAIN);
    return true;
}

/**
 * @brief Allocate the saved catalog on demand and rescan native NFC files.
 * @param app Application state.
 * @return True when the catalog buffer exists and the scan completed.
 */
bool az_ui_refresh_saved_catalog(AmiiboZeroApp* app) {
    if(!app) return false;
    if(!app->saved_entries) {
        app->saved_entries = static_cast<AzSavedEntry*>(calloc(AZ_MAX_SAVED, sizeof(AzSavedEntry)));
        if(!app->saved_entries) {
            app->saved_count = 0;
            return false;
        }
    }
    memset(app->saved_entries, 0, AZ_MAX_SAVED * sizeof(AzSavedEntry));
    app->saved_count = az_saved_scan(app->storage, app->saved_entries, AZ_MAX_SAVED);
    return true;
}

/**
 * @brief Release the saved catalog when the user leaves saved-file workflows.
 * @param app Application state.
 */
void az_ui_clear_saved_catalog(AmiiboZeroApp* app) {
    if(!app) return;
    free(app->saved_entries);
    app->saved_entries = NULL;
    app->saved_count = 0;
}

/**
 * @brief Find one saved basename in the current alphabetized saved catalog.
 * @param app Application state containing the current saved catalog.
 * @param filename Basename to locate.
 * @param fallback Fallback ordinal when the basename is no longer present.
 * @return Matching or safely clamped saved-menu ordinal.
 */
uint16_t az_ui_saved_selection_for_filename(
    const AmiiboZeroApp* app,
    const char* filename,
    uint16_t fallback) {
    if(!app || !app->saved_entries || app->saved_count == 0) return 0;
    if(filename && filename[0]) {
        for(uint16_t i = 0; i < app->saved_count; i++) {
            if(strcmp(app->saved_entries[i].filename, filename) == 0) return i;
        }
    }
    return fallback < app->saved_count ? fallback : (uint16_t)(app->saved_count - 1);
}

/**
 * @brief Enter the shared physical-tag screen after starting one operation.
 */
static void az_show_tag_operation(AmiiboZeroApp* app) {
    az_ui_navigate(app, AzScreenTagOperation, true);
}

/**
 * @brief Build/load the currently selected v2 encrypted dump and begin blank-tag programming.
 */
void az_ui_open_tag_write(AmiiboZeroApp* app) {
    if(!app || az_figure_is_v3(app->current_figure.id)) return;
    if(!app->keys.valid) {
        az_ui_toast(app, "Valid key_retail.bin required");
        return;
    }

    uint8_t dump[AZ_DUMP_SIZE];
    bool ok = false;
    if(app->current_is_saved) {
        ok = az_nfc_export_v2_dump(app->nfc_device, dump);
        if(!ok) {
            char path[AZ_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, app->current_saved_filename);
            nfc_device_clear(app->nfc_device);
            ok = az_nfc_load_device(app->nfc_device, path) &&
                 az_nfc_export_v2_dump(app->nfc_device, dump);
        }
    } else {
        uint8_t temporary_uid[9];
        ok = az_generate_dump(app->current_figure.id, &app->keys, dump, temporary_uid);
    }
    if(!ok) {
        az_ui_toast(app, "Could not prepare v2 dump");
        return;
    }

    app->screen_selection[AzScreenFigure] = app->selection;
    if(!az_tag_write_begin(app, dump)) {
        az_ui_toast(app, "Could not start tag writer");
        return;
    }
    az_show_tag_operation(app);
}

/** Begin Advanced > Read & save Amiibo. */
void az_ui_open_tag_read_save(AmiiboZeroApp* app) {
    if(!app->keys.valid) {
        az_ui_toast(app, "Valid key_retail.bin required");
        return;
    }
    app->screen_selection[AzScreenAdvanced] = app->selection;
    if(!az_tag_read_save_begin(app)) {
        az_ui_toast(app, "Could not start tag reader");
        return;
    }
    az_show_tag_operation(app);
}

/** Begin Advanced > Clear tag user data. */
void az_ui_open_tag_clear(AmiiboZeroApp* app) {
    if(!app->keys.valid) {
        az_ui_toast(app, "Valid key_retail.bin required");
        return;
    }
    app->screen_selection[AzScreenAdvanced] = app->selection;
    if(!az_tag_clear_begin(app)) {
        az_ui_toast(app, "Could not start tag reset");
        return;
    }
    az_show_tag_operation(app);
}

/**
 * @brief Load the selected saved entry, figure metadata, and authenticated dump details.
 */
void az_ui_open_current_saved(AmiiboZeroApp* app) {
    if(!app->saved_entries || app->saved_count == 0 || app->selection >= app->saved_count) return;
    app->screen_selection[AzScreenSaved] = app->selection;
    const AzSavedEntry* entry = &app->saved_entries[app->selection];
    memset(&app->current_figure, 0, sizeof(app->current_figure));
    if(az_db_find_by_id(app->storage, entry->id, &app->current_figure)) {
        furi_string_set_str(
            app->current_figure_name,
            app->current_figure.name[0] ? app->current_figure.name : entry->display_name);
    } else {
        memcpy(app->current_figure.id, entry->id, sizeof(app->current_figure.id));
        app->current_figure.category = entry->id[6];
        app->current_figure.type = entry->id[3];
        az_str_copy(
            app->current_figure.name, sizeof(app->current_figure.name), entry->display_name);
        furi_string_set_str(app->current_figure_name, entry->display_name);
    }
    app->current_is_saved = true;
    az_str_copy(app->current_saved_filename, sizeof(app->current_saved_filename), entry->filename);
    memset(&app->current_details, 0, sizeof(app->current_details));
    char path[AZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, app->current_saved_filename);
    nfc_device_clear(app->nfc_device);
    if(az_nfc_load_device(app->nfc_device, path)) {
        az_nfc_read_details(app->nfc_device, &app->keys, &app->current_details);
        app->current_lockon_valid = az_nfc_device_is_v3(app->nfc_device) &&
                                    az_saved_lockon_load(
                                        app->storage,
                                        app->current_saved_filename,
                                        app->current_lockon_sram);
        app->current_lockon_filename[0] = '\0';
    } else {
        app->current_lockon_valid = false;
        app->current_lockon_filename[0] = '\0';
    }
    app->screen_selection[AzScreenFigure] = 0;
    az_ui_navigate(app, AzScreenFigure, true);
}

/**
 * @brief Release compatibility rows allocated only while the Games screen is active.
 * @param app Application state.
 */
void az_ui_clear_games(AmiiboZeroApp* app) {
    if(!app) return;
    free(app->game_entries);
    app->game_entries = NULL;
    app->game_count = 0;
}

/**
 * @brief Load the indexed compatibility object for the current figure and open the games screen.
 */
void az_ui_open_games(AmiiboZeroApp* app) {
    app->screen_selection[AzScreenFigure] = app->selection;
    az_ui_clear_games(app);
    app->game_entries = static_cast<AzGame*>(calloc(AZ_MAX_GAMES, sizeof(AzGame)));
    if(!app->game_entries) {
        az_ui_toast(app, "Not enough memory for games");
        return;
    }
    if(!az_db_load_games(
           app->storage,
           app->current_figure.id,
           app->game_entries,
           AZ_MAX_GAMES,
           &app->game_count)) {
        az_ui_toast(app, "Could not read games DB");
    }
    app->screen_selection[AzScreenGames] = 0;
    az_ui_navigate(app, AzScreenGames, true);
}

/**
 * @brief Publish worker progress into lock-free scalar fields sampled by the GUI tick.
 * @param context AmiiboZeroApp pointer.
 * @param stage Current database preparation stage.
 * @param percent Overall completion percentage.
 */
static void az_database_progress(void* context, AzDbProgressStage stage, uint8_t percent) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    if(!app) return;
    app->db_progress_stage = stage;
    app->db_progress = percent > 100U ? 100U : percent;
}

/**
 * @brief Worker entry point that validates/rebuilds the unified index off the GUI thread.
 * @param context AmiiboZeroApp pointer.
 * @return Zero after publishing the worker result.
 */
static int32_t az_database_worker(void* context) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    uint32_t count = 0;
    bool result = az_db_ensure_index(
        app->storage,
        app->db_thread_force,
        &count,
        az_database_progress,
        app);
    app->db_thread_count = count;
    app->db_thread_result = result;
    app->db_thread_done = true;
    return 0;
}

/**
 * @brief Start background database preparation and enter the animated working screen.
 */
bool az_ui_start_database_prepare(AmiiboZeroApp* app, bool force) {
    if(!app || app->db_thread) return false;
    if(app->screen < AzScreenCount) app->screen_selection[app->screen] = app->selection;
    app->db_thread_force = force;
    app->db_thread_done = false;
    app->db_thread_result = false;
    app->db_thread_count = 0;
    app->db_progress = 0;
    app->db_progress_stage = AzDbProgressChecking;
    app->db_thread = furi_thread_alloc_ex("AmiiboIndex", 6144, az_database_worker, app);
    if(!app->db_thread) return false;
    az_ui_navigate(app, AzScreenWorking, true);
    furi_thread_start(app->db_thread);
    return true;
}

/**
 * @brief Collapse navigation back to the launch root without resuming covered screens.
 *
 * A pushed screen stays alive until it is popped, so a deep navigation history can keep lifecycle
 * state around that did not exist during the initial database build. Forced database preparation is
 * deliberately a low-memory operation: unwind every covered entry, run each pop cleanup exactly
 * once, then reactivate Home as the same root used at launch. The caller may push its return screen
 * again before entering Working.
 */
static void az_ui_reset_stack_to_launch_root(AmiiboZeroApp* app) {
    if(!app || app->ui_stack_depth == 0U) return;

    az_ui_stack_sync_top(app);
    while(app->ui_stack_depth > 1U) {
        const AzScreen popped = app->ui_stack[app->ui_stack_depth - 1U].screen;
        az_ui_screen_for(popped).onPopped(app);
        app->ui_stack_depth--;
    }

    AzUiStackEntry* root = &app->ui_stack[0];
    root->screen = AzScreenHome;
    root->selection = app->screen_selection[AzScreenHome];
    root->detail_scroll = 0U;
    az_ui_stack_activate_top(app);
}

/**
 * @brief Release on-demand runtime records before a memory-intensive forced index rebuild.
 *
 * The application launches with these catalogs/results absent and with an empty NFC device. Returning
 * to that baseline before allocating index-build buffers prevents stale Saved/Games/lock-on records
 * from competing with the sorter for heap space.
 */
static void az_release_runtime_records_for_index_refresh(AmiiboZeroApp* app) {
    if(!app) return;
    az_emulation_stop(app);
    az_ui_clear_games(app);
    az_ui_clear_saved_catalog(app);
    az_ui_clear_lockon_catalog(app);
    if(app->nfc_device) nfc_device_clear(app->nfc_device);

    memset(&app->current_category, 0, sizeof(app->current_category));
    memset(&app->current_figure, 0, sizeof(app->current_figure));
    if(app->current_figure_name) furi_string_reset(app->current_figure_name);
    memset(&app->current_details, 0, sizeof(app->current_details));
    memset(app->current_lockon_sram, 0, sizeof(app->current_lockon_sram));
    app->current_is_saved = false;
    app->current_saved_filename[0] = '\0';
    app->current_lockon_valid = false;
    app->current_lockon_filename[0] = '\0';
    app->lockon_action = AzLockOnActionNone;
}

/**
 * @brief Reload keys, release on-demand records, and start a forced background index refresh.
 */
void az_ui_status_refresh(AmiiboZeroApp* app) {
    if(!app) return;

    /* Reproduce the launch-time memory shape before the sorter starts. The stack framework keeps
     * covered screens alive by design, so explicitly retire them before releasing shared catalogs. */
    az_ui_reset_stack_to_launch_root(app);
    az_release_runtime_records_for_index_refresh(app);

    app->keys.valid = az_keys_load(app->storage, &app->keys);
    az_ui_navigate(app, AzScreenStatus, false);
    if(!az_ui_start_database_prepare(app, true)) {
        az_ui_toast(app, "Refresh already running");
    }
}

/**
 * @brief Move a bounded selection by one row while resetting marquee animation.
 */
void az_ui_move_selection(AmiiboZeroApp* app, int direction, uint16_t count) {
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
 * @brief Forward input to the active C++ screen object.
 */
static bool az_main_input(InputEvent* event, void* context) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    if(!app || !event) return false;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        const AzUiScreen& screen = az_ui_screen_for(app->screen);
        if(screen.backRequested(app, event)) {
            az_ui_pop(app);
        } else {
            az_ui_refresh(app);
        }
        return true;
    }

    const bool consumed = az_ui_screen_for(app->screen).input(app, event);
    if(consumed) az_ui_refresh(app);
    return consumed;
}

/**
 * @brief Advance toast and marquee animation on the dispatcher tick.
 */
static void az_tick_callback(void* context) {
    AmiiboZeroApp* app = static_cast<AmiiboZeroApp*>(context);
    az_tag_operation_tick(app);
    if(app->toast_ticks) app->toast_ticks--;
    app->animation++;
    if(app->db_thread && app->db_thread_done) {
        furi_thread_join(app->db_thread);
        furi_thread_free(app->db_thread);
        app->db_thread = NULL;
        app->index_ready = app->db_thread_result;
        app->index_count = app->index_ready ? app->db_thread_count : 0U;
        if(app->screen == AzScreenWorking) az_ui_pop(app);
        az_ui_toast(app, app->index_ready ? "Database ready" : "Database prepare failed");
        return;
    }
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
 * @brief Rebuild the lock-protected view snapshot for the current screen.
 */
void az_ui_refresh(AmiiboZeroApp* app) {
    if(!app || !app->main_view) return;
    with_view_model_cpp(
        app->main_view,
        AzViewModel*,
        model,
        {
            memset(model, 0, sizeof(*model));
            model->app = app;
            model->screen = app->screen;
            model->selection = app->selection;
            model->category = app->current_category;
            model->figure = app->current_figure;
            model->figure_name = app->current_figure_name;
            model->figure_saved = app->current_is_saved;
            az_str_copy(model->saved_filename, sizeof(model->saved_filename), app->current_saved_filename);
            az_str_copy(model->query, sizeof(model->query), app->query);
            model->animation = app->animation;
            model->db_progress = app->db_progress;
            model->db_progress_stage = app->db_progress_stage;

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
                for(uint8_t i = 0U; i < model->row_count; i++)
                    model->figure_row_names[i] = model->figure_rows[i].name;
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
                for(uint8_t i = 0U; i < model->row_count; i++)
                    model->figure_row_names[i] = model->figure_rows[i].name;
                app->list_count = total;
                model->count = total;
            } else if(app->screen == AzScreenSaved) {
                model->count = app->saved_count;
            } else if(app->screen == AzScreenLockOn) {
                model->count = app->lockon_count;
            } else if(app->screen == AzScreenGames) {
                model->games_total = app->game_count;
                if(app->game_entries && app->game_count && app->selection < app->game_count) {
                    model->games[0] = app->game_entries[app->selection];
                }
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
    app->current_figure_name = furi_string_alloc();
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

    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(app->dispatcher, AZ_VIEW_MANUAL_ID, byte_input_get_view(app->byte_input));

    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->screen = AzScreenHome;
    app->selection = 0U;
    app->detail_scroll = 0U;
    app->ui_stack_depth = 1U;
    app->ui_stack[0].screen = AzScreenHome;
    app->ui_stack[0].selection = 0U;
    app->ui_stack[0].detail_scroll = 0U;
    az_ui_stack_activate_top(app);
}

/**
 * @brief Unregister and free all UI objects in reverse ownership order.
 */
void az_ui_deinit(AmiiboZeroApp* app) {
    if(!app || !app->dispatcher) return;
    az_tag_operation_cancel(app);
    az_ui_clear_games(app);
    az_ui_clear_saved_catalog(app);
    az_ui_clear_lockon_catalog(app);
    if(app->db_thread) {
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
    if(app->current_figure_name) furi_string_free(app->current_figure_name);
    app->current_figure_name = NULL;
    furi_string_free(app->ui_scratch);
    app->ui_scratch = NULL;
}
