/**
 * @file amiibo_zero.cpp
 * @brief Application entry point and lifetime management.
 */

#include "amiibo_zero.h"
#include "amiibo_crypto.h"
#include "amiibo_storage.h"
#include "ui/ui_manager.h"

#include <new>
#include <string.h>

namespace {
/** @brief Release NFC-only resources so a database rebuild starts from a clean heap. */
void az_release_nfc_runtime(AmiiboZeroApp* app) {
    if(!app) return;

    if(app->nfc_device) {
        nfc_device_free(app->nfc_device);
        app->nfc_device = nullptr;
    }
    if(app->nfc) {
        nfc_free(app->nfc);
        app->nfc = nullptr;
    }

    app->listener = nullptr;
    app->v3_tx_buffer = nullptr;
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
void az_reset_database_runtime(AmiiboZeroApp* app) {
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
    app->db_thread = nullptr;
    app->db_thread_done = false;
    app->db_thread_result = false;
    app->db_thread_count = 0U;
    app->db_thread_force = false;
    app->db_progress = 0U;
    app->db_progress_stage = AzDbProgressChecking;
}
} // namespace

extern "C" int32_t amiibo_zero_app(void* p) {
    UNUSED(p);

    auto* app = new(std::nothrow) AmiiboZeroApp{};
    if(!app) return -1;

    app->storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    app->gui = static_cast<Gui*>(furi_record_open(RECORD_GUI));
    az_storage_init(app->storage);
    app->keys.valid = az_keys_load(app->storage, &app->keys);

    bool force_database_rebuild = false;
    bool fatal_error = false;

    while(true) {
        /*
         * Each pass owns a completely fresh UI session. Setup/Status refresh exits the current
         * dispatcher and returns here so every screen, view, modal control, NFC object, and their
         * heap fragments are gone before the forced database worker is created.
         */
        app->ui = new(std::nothrow) UiManager(*app);
        if(!app->ui || !app->ui->init()) {
            delete app->ui;
            app->ui = nullptr;
            fatal_error = true;
            break;
        }

        if(!app->ui->startDatabasePrepare(force_database_rebuild)) {
            app->ui->toast(force_database_rebuild ? "Could not start refresh" :
                                                    "Could not start DB check");
        }
        force_database_rebuild = false;

        app->ui->run();

        const bool restart_for_database = app->ui->databaseRefreshRequested();
        app->ui->stopEmulation();
        delete app->ui;
        app->ui = nullptr;
        az_release_nfc_runtime(app);

        if(!restart_for_database) break;

        az_reset_database_runtime(app);
        app->keys.valid = az_keys_load(app->storage, &app->keys);
        force_database_rebuild = true;
    }

    az_release_nfc_runtime(app);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    delete app;
    return fatal_error ? -1 : 0;
}
