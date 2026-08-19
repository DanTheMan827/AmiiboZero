/** @file controller.h @brief Shared UI actions used by per-screen C++ classes. */
#pragma once

#include "../amiibo_zero.h"

void az_ui_navigate(AmiiboZeroApp* app, AzScreen screen, bool reset_target);
bool az_ui_refresh_saved_catalog(AmiiboZeroApp* app);
void az_ui_clear_saved_catalog(AmiiboZeroApp* app);
bool az_ui_refresh_lockon_catalog(AmiiboZeroApp* app);
void az_ui_clear_lockon_catalog(AmiiboZeroApp* app);
bool az_ui_open_lockon_selector(AmiiboZeroApp* app, AzLockOnAction action);
bool az_ui_request_emulation(AmiiboZeroApp* app, bool persistent, bool fresh);
void az_ui_apply_selected_lockon(AmiiboZeroApp* app);
void az_ui_emulation_randomize_uid(AmiiboZeroApp* app);
void az_ui_open_search(AmiiboZeroApp* app);
void az_ui_open_rename(AmiiboZeroApp* app);
void az_ui_open_manual_id(AmiiboZeroApp* app);
uint16_t az_ui_saved_selection_for_filename(
    const AmiiboZeroApp* app,
    const char* filename,
    uint16_t fallback);
void az_ui_open_tag_write(AmiiboZeroApp* app);
void az_ui_open_tag_read_save(AmiiboZeroApp* app);
void az_ui_open_tag_clear(AmiiboZeroApp* app);
void az_ui_open_current_saved(AmiiboZeroApp* app);
void az_ui_clear_games(AmiiboZeroApp* app);
void az_ui_open_games(AmiiboZeroApp* app);
void az_ui_status_refresh(AmiiboZeroApp* app);
void az_ui_move_selection(AmiiboZeroApp* app, int direction, uint16_t count);
