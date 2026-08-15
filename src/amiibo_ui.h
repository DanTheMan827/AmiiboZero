/**
 * @file amiibo_ui.h
 * @brief Application UI lifecycle, navigation, status, and emulation-session
 * operations.
 */

#pragma once

#include "amiibo_zero.h"

/**
 * @brief Allocate and configure UI views, callbacks, and dispatcher state.
 * @param app Application state.
 */
void az_ui_init(AmiiboZeroApp *app);

/**
 * @brief Release UI resources owned by the application.
 * @param app Application state.
 */
void az_ui_deinit(AmiiboZeroApp *app);

/**
 * @brief Refresh the active view model from application state.
 * @param app Application state.
 */
void az_ui_refresh(AmiiboZeroApp *app);

/**
 * @brief Navigate to a screen and request a redraw.
 * @param app Application state.
 * @param screen Screen to activate.
 */
void az_ui_show(AmiiboZeroApp *app, AzScreen screen);

/**
 * @brief Start asynchronous database validation or rebuilding.
 * @param app Application state.
 * @param force Whether to rebuild even when the existing index is current.
 * @param return_screen Screen to restore when background preparation completes.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_ui_start_database_prepare(AmiiboZeroApp *app, bool force,
                                  AzScreen return_screen);

/**
 * @brief Display a temporary status message in the UI.
 * @param app Application state.
 * @param text Text to process or display.
 */
void az_ui_toast(AmiiboZeroApp *app, const char *text);

/**
 * @brief Stop NFC emulation and persist mutable state when required.
 * @param app Application state.
 */
void az_emulation_stop(AmiiboZeroApp *app);
