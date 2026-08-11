/**
 * @file amiibo_zero.c
 * @brief Amiibo Zero application entry point and top-level resource lifetime management.
 */

#include "./amiibo_zero.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Allocate application state, initialize services/modules, run the UI loop, and clean up.
 * @param p Unused Flipper application argument.
 * @return Zero on normal exit or -1 when the application state cannot be allocated.
 */
int32_t amiibo_zero_app(void* p) {
    UNUSED(p);

    AmiiboZeroApp* app = calloc(1, sizeof(AmiiboZeroApp));
    if(!app) return -1;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->gui = furi_record_open(RECORD_GUI);
    az_storage_init(app->storage);

    app->keys.valid = az_keys_load(app->storage, &app->keys);
    app->index_ready = az_db_ensure_index(app->storage, false, &app->index_count);
    app->nfc = nfc_alloc();
    app->nfc_device = nfc_device_alloc();

    az_ui_init(app);
    if(!app->index_ready) {
        az_ui_toast(app, "Add amiibo.json in app data");
    }
    view_dispatcher_run(app->dispatcher);

    az_emulation_stop(app);
    az_ui_deinit(app);
    nfc_device_free(app->nfc_device);
    nfc_free(app->nfc);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    free(app);
    return 0;
}
