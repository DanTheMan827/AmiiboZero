/**
 * @file ui_bridge.h
 * @brief C linkage boundary between the application runtime and the C++ UI stack.
 */

#pragma once

#include "../amiibo_zero.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create, run, and destroy one UI-manager session.
 * @param app Shared application runtime state.
 * @param force_database_rebuild Whether database preparation should force a rebuild.
 * @param restart_for_database Receives whether the session requested a fresh rebuild session.
 * @return true if the UI manager was created and initialized; false on fatal setup failure.
 */
bool az_ui_run_session(
    AmiiboZeroApp* app,
    bool force_database_rebuild,
    bool* restart_for_database);

#ifdef __cplusplus
}
#endif
