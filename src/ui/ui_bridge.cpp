/**
 * @file ui_bridge.cpp
 * @brief C linkage boundary for constructing and running the C++ UI stack.
 */

#include "ui_bridge.h"
#include "ui_manager.h"
#include "../amiibo_storage.h"

#include <new>

extern "C" bool az_ui_run_session(
    AmiiboZeroApp* app,
    bool force_database_rebuild,
    bool* restart_for_database) {
    if(restart_for_database) *restart_for_database = false;
    if(!app) return false;

    app->ui = new(std::nothrow) UiManager(*app);
    if(!app->ui || !app->ui->init()) {
        delete app->ui;
        app->ui = nullptr;
        return false;
    }

    if(az_storage_required_data_present(&app->data_files)) {
        if(!app->ui->startDatabasePrepare(force_database_rebuild)) {
            app->ui->toast(force_database_rebuild ? "Could not start refresh" :
                                                    "Could not start DB check");
        }
    }

    app->ui->run();

    if(restart_for_database) {
        *restart_for_database = app->ui->databaseRefreshRequested();
    }
    app->ui->stopEmulation();
    delete app->ui;
    app->ui = nullptr;
    return true;
}
