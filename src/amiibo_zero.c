/**
 * @file amiibo_zero.c
 * @brief Application entry point and lifetime management.
 * @details Allocates shared services, starts database preparation and the UI,
 * then releases resources on exit.
 */

#include "amiibo_zero.h"
#include "amiibo_crypto.h"
#include "amiibo_storage.h"
#include "amiibo_ui.h"

#include <stdlib.h>

/**
 * @brief Run the Amiibo Zero application lifecycle.
 * @param p Unused application launch parameter.
 * @return 0 after normal shutdown, or -1 if application state allocation fails.
 */
int32_t amiibo_zero_app(void *p) {
  UNUSED(p);

  AmiiboZeroApp *app = calloc(1, sizeof(AmiiboZeroApp));
  if (!app)
    return -1;

  app->storage = furi_record_open(RECORD_STORAGE);
  app->gui = furi_record_open(RECORD_GUI);
  az_storage_init(app->storage);
  app->keys.valid = az_keys_load(app->storage, &app->keys);
  app->nfc = nfc_alloc();
  app->nfc_device = nfc_device_alloc();

  az_ui_init(app);
  if (!az_ui_start_database_prepare(app, false, AzScreenHome)) {
    az_ui_toast(app, "Could not start DB check");
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
