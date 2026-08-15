/**
 * @file amiibo_zero.c
 * @brief Application entry point and lifetime management.
 */

#include "amiibo_zero.h"
#include "amiibo_crypto.h"
#include "amiibo_db.h"
#include "amiibo_storage.h"
#include "ui/ui_bridge.h"

#include <stdlib.h>
#include <string.h>

/** @brief Release NFC-only resources so a database rebuild starts from a clean heap. */
static void az_release_nfc_runtime(AmiiboZeroApp* app) {
    if(!app) return;

    if(app->nfc_device) {
        nfc_device_free(app->nfc_device);
        app->nfc_device = NULL;
    }
    if(app->nfc) {
        nfc_free(app->nfc);
        app->nfc = NULL;
    }

    app->listener = NULL;
    app->v3_tx_buffer = NULL;
    app->v3_i2c_listener = false;
    app->v3_sector_select_pending = false;
    app->v3_authenticated = false;
    app->v3_sram_ready = false;
    app->v3_sector = 0U;
    app->emulating = false;
    app->emulation_persistent = false;
    app->emulation_path[0] = '\0';
}

/** @brief Clear state derived from an index before recreating the UI for a forced rebuild. */
static void az_reset_database_runtime(AmiiboZeroApp* app) {
    if(!app) return;

    memset(&app->current_category, 0, sizeof(app->current_category));
    memset(&app->current_figure, 0, sizeof(app->current_figure));
    memset(&app->current_details, 0, sizeof(app->current_details));
    memset(app->current_lockon_sram, 0, sizeof(app->current_lockon_sram));
    app->current_is_saved = false;
    app->current_saved_filename[0] = '\0';
    app->current_lockon_valid = false;
    app->current_lockon_filename[0] = '\0';

    app->index_ready = false;
    app->index_count = 0U;
    app->db_thread = NULL;
    app->db_thread_done = false;
    app->db_thread_result = false;
    app->db_thread_count = 0U;
    app->db_thread_force = false;
    app->db_progress = 0U;
    app->db_progress_stage = AzDbProgressChecking;
}

int32_t amiibo_zero_app(void* p) {
    UNUSED(p);

    AmiiboZeroApp* app = calloc(1, sizeof(AmiiboZeroApp));
    if(!app) return -1;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->gui = furi_record_open(RECORD_GUI);
    az_storage_init(app->storage);
    az_storage_check_data_files(app->storage, &app->data_files);
    app->keys.valid = app->data_files.key_retail && az_keys_load(app->storage, &app->keys);

    bool force_database_rebuild = false;
    bool fatal_error = false;

    while(true) {
        bool restart_for_database = false;
        if(!az_ui_run_session(app, force_database_rebuild, &restart_for_database)) {
            fatal_error = true;
            break;
        }

        az_release_nfc_runtime(app);
        if(!restart_for_database) break;

        /* Manual refresh starts from no index state and no stale build artifacts. */
        az_db_remove_index_files(app->storage);
        az_reset_database_runtime(app);
        az_storage_check_data_files(app->storage, &app->data_files);
        memset(&app->keys, 0, sizeof(app->keys));
        app->keys.valid = app->data_files.key_retail && az_keys_load(app->storage, &app->keys);
        force_database_rebuild = true;
    }

    az_release_nfc_runtime(app);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    free(app);
    return fatal_error ? -1 : 0;
}
