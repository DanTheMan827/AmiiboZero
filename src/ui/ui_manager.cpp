/**
 * @file ui_manager.cpp
 * @brief Screen-stack navigation, Flipper view integration, modal inputs, and UI-level actions.
 */

#include "ui_manager.h"

#include "../amiibo_crypto.h"
#include "../amiibo_db.h"
#include "../amiibo_nfc.h"
#include "../amiibo_storage.h"
#include "ui_screens.h"

#include <new>
#include <stdio.h>
#include <string.h>

namespace {
/** @brief Minimal locking view model pointing back to the owning C++ UI manager. */
struct UiViewModel {
    UiManager* manager; /**< Manager that renders the current stack top. */
};
} // namespace

UiManager::UiManager(AmiiboZeroApp& app) :
    app_(app),
    dispatcher_(nullptr),
    main_view_(nullptr),
    text_input_(nullptr),
    byte_input_(nullptr),
    scratch_(nullptr),
    stack_{},
    depth_(0),
    toast_{},
    toast_ticks_(0),
    animation_(0),
    text_mode_(TextMode::None),
    text_buffer_{},
    manual_id_{},
    pending_pop_count_(0),
    pending_push_(nullptr),
    pending_status_refresh_(false),
    database_refresh_requested_(false) {
}

UiManager::~UiManager() {
    deinit();
}

bool UiManager::init() {
    if(dispatcher_) return true;

    scratch_ = furi_string_alloc();
    dispatcher_ = view_dispatcher_alloc();
    main_view_ = view_alloc();
    text_input_ = text_input_alloc();
    byte_input_ = byte_input_alloc();
    if(!scratch_ || !dispatcher_ || !main_view_ || !text_input_ || !byte_input_) {
        deinit();
        return false;
    }

    view_dispatcher_set_event_callback_context(dispatcher_, this);
    view_dispatcher_set_navigation_event_callback(dispatcher_, navigationCallback);
    view_dispatcher_set_tick_event_callback(dispatcher_, tickCallback, 250);

    view_allocate_model(main_view_, ViewModelTypeLocking, sizeof(UiViewModel));
    view_set_context(main_view_, this);
    view_set_draw_callback(main_view_, drawCallback);
    view_set_input_callback(main_view_, inputCallback);
    auto* model = static_cast<UiViewModel*>(view_get_model(main_view_));
    model->manager = this;
    view_commit_model(main_view_, true);
    view_dispatcher_add_view(dispatcher_, ViewMain, main_view_);
    view_dispatcher_add_view(dispatcher_, ViewTextInput, text_input_get_view(text_input_));
    view_dispatcher_add_view(dispatcher_, ViewByteInput, byte_input_get_view(byte_input_));
    view_dispatcher_attach_to_gui(dispatcher_, app_.gui, ViewDispatcherTypeFullscreen);

    Screen* home = new(std::nothrow) HomeScreen(*this);
    if(!home) {
        deinit();
        return false;
    }
    if(!push(home)) {
        deinit();
        return false;
    }
    switchToMainView();
    return true;
}

void UiManager::deinit() {
    clearPendingNavigation();

    if(app_.db_thread) {
        furi_thread_join(app_.db_thread);
        furi_thread_free(app_.db_thread);
        app_.db_thread = nullptr;
    }

    while(depth_) {
        Screen* screen = stack_[--depth_];
        stack_[depth_] = nullptr;
        destroyScreen(screen);
    }

    if(dispatcher_ && byte_input_) view_dispatcher_remove_view(dispatcher_, ViewByteInput);
    if(byte_input_) {
        byte_input_free(byte_input_);
        byte_input_ = nullptr;
    }

    if(dispatcher_ && text_input_) view_dispatcher_remove_view(dispatcher_, ViewTextInput);
    if(text_input_) {
        text_input_free(text_input_);
        text_input_ = nullptr;
    }

    if(dispatcher_ && main_view_) view_dispatcher_remove_view(dispatcher_, ViewMain);
    if(main_view_) {
        view_free(main_view_);
        main_view_ = nullptr;
    }

    if(dispatcher_) {
        view_dispatcher_free(dispatcher_);
        dispatcher_ = nullptr;
    }
    if(scratch_) {
        furi_string_free(scratch_);
        scratch_ = nullptr;
    }
}

void UiManager::run() {
    if(dispatcher_) view_dispatcher_run(dispatcher_);
}

void UiManager::stop() {
    if(dispatcher_) view_dispatcher_stop(dispatcher_);
}

bool UiManager::databaseRefreshRequested() const {
    return database_refresh_requested_;
}

bool UiManager::push(Screen* screen) {
    if(!screen) return false;
    if(depth_ >= MaxScreens) {
        delete screen;
        return false;
    }

    stack_[depth_++] = screen;
    if(!screen->onPushed()) {
        stack_[--depth_] = nullptr;
        destroyScreen(screen);
        if(top()) top()->onRevealed();
        resetAnimation();
        refresh();
        return false;
    }

    resetAnimation();
    refresh();
    switchToMainView();
    return true;
}

bool UiManager::pop() {
    if(depth_ <= 1) {
        stop();
        return false;
    }

    Screen* screen = stack_[depth_ - 1];
    stack_[depth_ - 1] = nullptr;
    --depth_;
    destroyScreen(screen);
    if(top()) top()->onRevealed();
    resetAnimation();
    refresh();
    return true;
}

bool UiManager::replace(Screen* screen) {
    if(!screen) return false;
    if(depth_ == 0) return push(screen);

    Screen* old = stack_[depth_ - 1];
    stack_[depth_ - 1] = screen;
    if(!screen->onPushed()) {
        stack_[depth_ - 1] = old;
        destroyScreen(screen);
        refresh();
        return false;
    }

    destroyScreen(old);
    resetAnimation();
    refresh();
    switchToMainView();
    return true;
}

void UiManager::schedulePop(uint8_t count) {
    if(count > pending_pop_count_) pending_pop_count_ = count;
}

void UiManager::schedulePopAndPush(uint8_t pop_count, Screen* screen) {
    if(pending_push_) delete pending_push_;
    pending_push_ = screen;
    pending_pop_count_ = pop_count;
}

bool UiManager::rebaseScreenOnParent(ScreenId child, Screen* parent) {
    if(!parent || depth_ < 2) {
        delete parent;
        return false;
    }

    size_t child_index = depth_;
    for(size_t i = 1; i < depth_; ++i) {
        if(stack_[i]->id() == child) {
            child_index = i;
            break;
        }
    }
    if(child_index == depth_) {
        delete parent;
        return false;
    }

    const size_t tail_count = depth_ - child_index;
    if(2U + tail_count > MaxScreens) {
        delete parent;
        return false;
    }

    Screen* tail[MaxScreens] = {};
    for(size_t i = 0; i < tail_count; ++i) tail[i] = stack_[child_index + i];

    for(size_t i = 1; i < child_index; ++i) {
        destroyScreen(stack_[i]);
        stack_[i] = nullptr;
    }

    stack_[1] = parent;
    if(!parent->onPushed()) {
        stack_[1] = nullptr;
        destroyScreen(parent);
        return false;
    }
    for(size_t i = 0; i < tail_count; ++i) stack_[2 + i] = tail[i];
    for(size_t i = 2 + tail_count; i < depth_; ++i) stack_[i] = nullptr;
    depth_ = 2 + tail_count;
    refresh();
    return true;
}

Screen* UiManager::top() const {
    return depth_ ? stack_[depth_ - 1] : nullptr;
}

Screen* UiManager::find(ScreenId id) const {
    for(size_t i = depth_; i > 0; --i) {
        if(stack_[i - 1]->id() == id) return stack_[i - 1];
    }
    return nullptr;
}

bool UiManager::contains(ScreenId id) const {
    return find(id) != nullptr;
}

size_t UiManager::depth() const {
    return depth_;
}

AmiiboZeroApp& UiManager::app() {
    return app_;
}

const AmiiboZeroApp& UiManager::app() const {
    return app_;
}

void UiManager::refresh() {
    if(!main_view_) return;
    auto* model = static_cast<UiViewModel*>(view_get_model(main_view_));
    model->manager = this;
    view_commit_model(main_view_, true);
}

void UiManager::toast(const char* text) {
    az_str_copy(toast_, sizeof(toast_), text ? text : "");
    toast_ticks_ = 8;
    resetAnimation();
    refresh();
}

bool UiManager::toastActive() const {
    return toast_ticks_ != 0 && toast_[0] != '\0';
}

const char* UiManager::toastText() const {
    return toast_;
}

uint8_t UiManager::animation() const {
    return animation_;
}

void UiManager::resetAnimation() {
    animation_ = 0;
}

FuriString* UiManager::scratch() const {
    return scratch_;
}

const char* UiManager::scratchText() const {
    return scratch_ ? furi_string_get_cstr(scratch_) : "";
}

void UiManager::setScratch(const char* text) {
    if(scratch_) furi_string_set_str(scratch_, text ? text : "");
}

void UiManager::openSearchInput() {
    const char* initial = "";
    if(top() && top()->id() == ScreenId::SearchResults) {
        initial = static_cast<SearchResultsScreen*>(top())->query();
    }
    az_str_copy(text_buffer_, sizeof(text_buffer_), initial);
    text_mode_ = TextMode::Search;
    text_input_reset(text_input_);
    text_input_set_header_text(text_input_, "Search name or Amiibo ID");
    text_input_set_minimum_length(text_input_, 0);
    text_input_set_result_callback(
        text_input_, textInputDone, this, text_buffer_, sizeof(text_buffer_), false);
    view_dispatcher_switch_to_view(dispatcher_, ViewTextInput);
}

void UiManager::openRenameInput() {
    az_str_copy(text_buffer_, sizeof(text_buffer_), app_.current_saved_filename);
    char* dot = strrchr(text_buffer_, '.');
    if(dot) *dot = '\0';
    text_mode_ = TextMode::Rename;
    text_input_reset(text_input_);
    text_input_set_header_text(text_input_, "Rename saved Amiibo");
    text_input_set_minimum_length(text_input_, 1);
    text_input_set_result_callback(
        text_input_, textInputDone, this, text_buffer_, sizeof(text_buffer_), false);
    view_dispatcher_switch_to_view(dispatcher_, ViewTextInput);
}

void UiManager::openManualIdInput() {
    memset(manual_id_, 0, sizeof(manual_id_));
    byte_input_set_header_text(byte_input_, "Manual figure ID (8 bytes)");
    byte_input_set_result_callback(
        byte_input_, manualIdDone, nullptr, this, manual_id_, sizeof(manual_id_));
    view_dispatcher_switch_to_view(dispatcher_, ViewByteInput);
}

void UiManager::completeSearch() {
    switchToMainView();
    Screen* current = top();
    if(!current) return;

    if(current->id() == ScreenId::SearchResults) {
        if(text_buffer_[0]) {
            Screen* replacement = new(std::nothrow) SearchResultsScreen(*this, text_buffer_);
            if(!replacement || !replace(replacement)) toast("Could not show search results");
        } else {
            pop();
        }
        return;
    }

    if(current->id() == ScreenId::Figures) {
        pop();
        current = top();
    }
    if(current && current->id() == ScreenId::Categories && text_buffer_[0]) {
        Screen* results = new(std::nothrow) SearchResultsScreen(*this, text_buffer_);
        if(!results || !push(results)) toast("Could not show search results");
    }
}

void UiManager::completeRename() {
    switchToMainView();
    char renamed[96] = {};
    if(az_saved_rename(
           app_.storage,
           app_.current_saved_filename,
           text_buffer_,
           renamed,
           sizeof(renamed))) {
        az_str_copy(app_.current_saved_filename, sizeof(app_.current_saved_filename), renamed);
        Screen* saved_base = find(ScreenId::Saved);
        if(saved_base && !static_cast<SavedScreen*>(saved_base)->refresh(renamed)) {
            toast("Renamed; catalog refresh failed");
            return;
        }
        toast("Renamed");
    } else {
        toast("Rename failed");
    }
}

void UiManager::completeManualId() {
    switchToMainView();
    memset(&app_.current_figure, 0, sizeof(app_.current_figure));
    if(!az_db_find_by_id(app_.storage, manual_id_, &app_.current_figure)) {
        static const char hex[] = "0123456789abcdef";
        memcpy(app_.current_figure.id, manual_id_, 8);
        for(size_t i = 0; i < 8; ++i) {
            app_.current_figure.id_hex[i * 2] = hex[manual_id_[i] >> 4];
            app_.current_figure.id_hex[i * 2 + 1] = hex[manual_id_[i] & 0x0F];
        }
        app_.current_figure.id_hex[16] = '\0';
        app_.current_figure.category = manual_id_[6];
        app_.current_figure.type = manual_id_[3];
        snprintf(
            app_.current_figure.name,
            sizeof(app_.current_figure.name),
            "Manual %s",
            app_.current_figure.id_hex);
    }
    app_.current_is_saved = false;
    app_.current_saved_filename[0] = '\0';
    app_.current_lockon_valid = false;
    app_.current_lockon_filename[0] = '\0';
    Screen* figure = new(std::nothrow) FigureScreen(*this);
    if(!figure || !push(figure)) toast("Could not open figure");
}

bool UiManager::requestEmulation(bool persistent, bool fresh) {
    if(fresh && !app_.keys.valid) {
        toast("Add key_retail.bin first");
        return false;
    }

    if(fresh && az_figure_is_v3(app_.current_figure.id)) {
        app_.current_lockon_valid = false;
        app_.current_lockon_filename[0] = '\0';
        Screen* lockon = new(std::nothrow) LockOnScreen(
            *this,
            persistent ? LockOnAction::GeneratePersistent : LockOnAction::GenerateTemporary);
        if(!lockon || !push(lockon)) {
            toast("Could not open lock-on list");
            return false;
        }
        return true;
    }

    if(!fresh && app_.current_is_saved && az_figure_is_v3(app_.current_figure.id)) {
        app_.current_lockon_valid = az_saved_lockon_load(
            app_.storage, app_.current_saved_filename, app_.current_lockon_sram);
        app_.current_lockon_filename[0] = '\0';
        if(!app_.current_lockon_valid) {
            Screen* lockon =
                new(std::nothrow) LockOnScreen(*this, LockOnAction::EmulateSaved);
            if(!lockon || !push(lockon)) {
                toast("Could not open lock-on list");
                return false;
            }
            return true;
        }
    }
    return beginEmulation(persistent, fresh, false);
}

void UiManager::setSavedFilenameFromPath(const char* path) {
    const char* slash = strrchr(path, '/');
    az_str_copy(
        app_.current_saved_filename,
        sizeof(app_.current_saved_filename),
        slash ? slash + 1 : path);
}

bool UiManager::beginEmulation(bool persistent, bool fresh, bool replace_top) {
    /*
     * Start NFC before the screen-stack mutation. This preserves the known-good v3 listener
     * ordering from the C implementation while the EmulationScreen still owns teardown.
     */
    if(!startEmulationSession(persistent, fresh)) return false;

    Screen* emulation = new(std::nothrow) EmulationScreen(*this);
    if(!emulation) {
        stopEmulation();
        toast("Could not open emulation screen");
        return false;
    }

    if(replace_top) {
        schedulePopAndPush(1, emulation);
        return true;
    }
    if(!push(emulation)) {
        /* push() deletes the screen on failure, and its destructor stops the adopted session. */
        toast("Could not open emulation screen");
        return false;
    }
    return true;
}

bool UiManager::startEmulationSession(bool persistent, bool fresh) {
    if(!ensureNfcResources()) {
        toast("NFC unavailable");
        return false;
    }
    if(!app_.keys.valid && fresh) {
        toast("Add key_retail.bin first");
        return false;
    }
    if(fresh && az_figure_is_v3(app_.current_figure.id) && !app_.current_lockon_valid) {
        toast("Select a lock-on first");
        return false;
    }

    nfc_device_clear(app_.nfc_device);
    char path[AZ_PATH_MAX] = {};
    bool ok = false;
    if(!fresh && app_.current_is_saved) {
        snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, app_.current_saved_filename);
        ok = az_nfc_load_device(app_.nfc_device, path);
        if(ok && az_nfc_device_is_v3(app_.nfc_device)) {
            app_.current_lockon_valid = az_saved_lockon_load(
                app_.storage, app_.current_saved_filename, app_.current_lockon_sram);
            app_.current_lockon_filename[0] = '\0';
            ok = app_.current_lockon_valid;
        }
    } else {
        ok = az_nfc_generate_device(app_.nfc_device, &app_.current_figure, &app_.keys);
        if(ok && persistent) {
            az_make_unique_save_path(app_.storage, &app_.current_figure, path, sizeof(path));
            ok = path[0] && az_nfc_save_device(app_.nfc_device, path);
            if(ok) {
                setSavedFilenameFromPath(path);
                if(az_figure_is_v3(app_.current_figure.id)) {
                    ok = app_.current_lockon_valid &&
                         az_saved_lockon_save(
                             app_.storage,
                             app_.current_saved_filename,
                             app_.current_lockon_sram);
                    if(!ok) az_saved_delete(app_.storage, app_.current_saved_filename);
                }
                if(ok) app_.current_is_saved = true;
            }
        }
    }

    if(!ok) {
        toast("Could not prepare figure");
        return false;
    }

    /*
     * Preserve the working pre-screen-lifecycle order: update any persistent navigation before
     * the RF listener is started, so screen/catalog allocation cannot disturb a live v3 session.
     */
    if(persistent && app_.current_is_saved && !contains(ScreenId::Saved)) {
        Screen* saved = new(std::nothrow) SavedScreen(*this, app_.current_saved_filename);
        if(!saved || !rebaseScreenOnParent(ScreenId::Figure, saved)) {
            toast("Saved, but could not update navigation");
        }
    }

    if(!az_nfc_listener_start(&app_)) {
        toast("NFC listener unavailable");
        return false;
    }

    app_.emulation_persistent = persistent || app_.current_is_saved;
    if(app_.emulation_persistent) {
        if(path[0]) {
            az_str_copy(app_.emulation_path, sizeof(app_.emulation_path), path);
        } else {
            snprintf(
                app_.emulation_path,
                sizeof(app_.emulation_path),
                "%s/%s",
                AZ_FIGURES_DIR,
                app_.current_saved_filename);
        }
    } else {
        app_.emulation_path[0] = '\0';
    }

    return true;
}

void UiManager::stopEmulation() {
    if(!app_.emulating) return;
    const bool synced = az_nfc_listener_pause_and_sync(&app_);
    if(app_.emulation_persistent && synced) {
        toast(
            az_nfc_save_device(app_.nfc_device, app_.emulation_path) ? "Saved changes" :
                                                                         "Autosave failed");
    }
    app_.emulation_persistent = false;
}

void UiManager::randomizeEmulationUid() {
    if(!app_.emulating) return;
    if(!app_.keys.valid) {
        toast("key_retail.bin required");
        return;
    }
    if(!az_nfc_listener_pause_and_sync(&app_)) {
        toast("Could not pause NFC");
        return;
    }

    const bool randomized = az_nfc_randomize_uid(app_.nfc_device, &app_.keys);
    bool saved = true;
    if(randomized && app_.emulation_persistent) {
        saved = az_nfc_save_device(app_.nfc_device, app_.emulation_path);
    }
    const bool restarted = az_nfc_listener_start(&app_);
    if(!restarted) {
        app_.emulation_persistent = false;
        toast("NFC restart failed");
        schedulePop();
        return;
    }
    if(!randomized) toast("UID randomize failed");
    else if(!saved) toast("UID changed; save failed");
    else toast("UID randomized");
}

void UiManager::databaseProgress(void* context, AzDbProgressStage stage, uint8_t percent) {
    auto* app = static_cast<AmiiboZeroApp*>(context);
    if(!app) return;
    app->db_progress_stage = stage;
    app->db_progress = percent > 100U ? 100U : percent;
}

int32_t UiManager::databaseWorker(void* context) {
    auto* app = static_cast<AmiiboZeroApp*>(context);
    uint32_t count = 0;
    const bool result = az_db_ensure_index(
        app->storage, app->db_thread_force, &count, databaseProgress, app);
    app->db_thread_count = count;
    app->db_thread_result = result;
    app->db_thread_done = true;
    return 0;
}

bool UiManager::startDatabasePrepare(bool force) {
    if(app_.db_thread) return false;

    /* Index generation never uses NFC. Free it before allocating the worker stack and sort heap. */
    releaseNfcResourcesForDatabase();

    app_.db_thread_force = force;
    app_.db_thread_done = false;
    app_.db_thread_result = false;
    app_.db_thread_count = 0;
    app_.db_progress = 0;
    app_.db_progress_stage = AzDbProgressChecking;
    app_.db_thread = furi_thread_alloc_ex("AmiiboIndex", 6144, databaseWorker, &app_);
    if(!app_.db_thread) {
        ensureNfcResources();
        return false;
    }

    Screen* working = new(std::nothrow) WorkingScreen(*this);
    if(!working) {
        furi_thread_free(app_.db_thread);
        app_.db_thread = nullptr;
        ensureNfcResources();
        return false;
    }
    if(!push(working)) {
        furi_thread_free(app_.db_thread);
        app_.db_thread = nullptr;
        ensureNfcResources();
        return false;
    }
    furi_thread_start(app_.db_thread);
    return true;
}

bool UiManager::ensureNfcResources() {
    if(!app_.nfc) app_.nfc = nfc_alloc();
    if(!app_.nfc) return false;

    if(!app_.nfc_device) app_.nfc_device = nfc_device_alloc();
    if(!app_.nfc_device) {
        nfc_free(app_.nfc);
        app_.nfc = nullptr;
        return false;
    }

    return true;
}

void UiManager::releaseNfcResourcesForDatabase() {
    stopEmulation();
    if(app_.nfc_device) {
        nfc_device_free(app_.nfc_device);
        app_.nfc_device = nullptr;
    }
    if(app_.nfc) {
        nfc_free(app_.nfc);
        app_.nfc = nullptr;
    }
}

void UiManager::refreshStatus() {
    if(app_.db_thread) {
        toast("Refresh already running");
        return;
    }
    pending_status_refresh_ = true;
}

void UiManager::performStatusRefresh() {
    /*
     * A rebuild needs the same clean allocator state as startup. Do not rebuild inside the
     * current UI session: stop the dispatcher and let the outer app loop destroy this manager,
     * all screens/views, and NFC resources before creating a fresh manager for the forced build.
     */
    database_refresh_requested_ = true;
    stop();
}

void UiManager::destroyScreen(Screen* screen) {
    if(!screen) return;
    screen->onPopped();
    delete screen;
}

void UiManager::draw(Canvas* canvas) {
    canvas_clear(canvas);
    Screen* screen = top();
    if(screen) screen->draw(canvas);
}

bool UiManager::handleInput(const InputEvent& event) {
    if(event.type != InputTypeShort && event.type != InputTypeRepeat) return false;
    Screen* screen = top();
    if(!screen) return false;

    if(event.type == InputTypeShort && event.key == InputKeyBack) {
        handleBack();
        refresh();
        return true;
    }

    const bool handled = screen->handleInput(event);
    applyPendingNavigation();
    refresh();
    return handled;
}

void UiManager::handleBack() {
    Screen* screen = top();
    if(!screen) return;

    const BackAction action = screen->onBack();
    switch(action) {
    case BackAction::Pop:
        // Only pop the screen that received Back. A hook that wants different navigation should
        // request it through the deferred-navigation helpers rather than synchronously pushing.
        if(top() == screen) pop();
        break;
    case BackAction::Cancel:
        break;
    case BackAction::Exit:
        stop();
        clearPendingNavigation();
        return;
    }

    // Back hooks may cancel the default pop and schedule an alternate stack mutation.
    applyPendingNavigation();
}

void UiManager::tick() {
    if(toast_ticks_) --toast_ticks_;
    ++animation_;
    if(top()) top()->onTick();

    if(app_.db_thread && app_.db_thread_done) {
        furi_thread_join(app_.db_thread);
        furi_thread_free(app_.db_thread);
        app_.db_thread = nullptr;

        /* Recreate NFC only after the database worker stack and sort allocations are gone. */
        const bool nfc_ready = ensureNfcResources();
        app_.index_ready = app_.db_thread_result;
        app_.index_count = app_.db_thread_count;
        if(top() && top()->id() == ScreenId::Working) pop();
        if(!nfc_ready) toast("NFC unavailable");
        else toast(app_.index_ready ? "Database ready" : "Database prepare failed");
        return;
    }
    refresh();
}

void UiManager::applyPendingNavigation() {
    uint8_t pop_count = pending_pop_count_;
    Screen* push_screen = pending_push_;
    const bool status_refresh = pending_status_refresh_;
    pending_pop_count_ = 0;
    pending_push_ = nullptr;
    pending_status_refresh_ = false;

    if(status_refresh) {
        if(push_screen) delete push_screen;
        performStatusRefresh();
        return;
    }

    while(pop_count-- && depth_ > 1) pop();
    if(push_screen) {
        if(!push(push_screen)) toast("Could not open screen");
    }
}

void UiManager::clearPendingNavigation() {
    pending_pop_count_ = 0;
    pending_status_refresh_ = false;
    if(pending_push_) {
        delete pending_push_;
        pending_push_ = nullptr;
    }
}

void UiManager::switchToMainView() {
    if(dispatcher_) view_dispatcher_switch_to_view(dispatcher_, ViewMain);
}

void UiManager::drawCallback(Canvas* canvas, void* model) {
    auto* view_model = static_cast<UiViewModel*>(model);
    if(view_model && view_model->manager) view_model->manager->draw(canvas);
}

bool UiManager::inputCallback(InputEvent* event, void* context) {
    auto* manager = static_cast<UiManager*>(context);
    return manager && event ? manager->handleInput(*event) : false;
}

bool UiManager::navigationCallback(void* context) {
    auto* manager = static_cast<UiManager*>(context);
    if(!manager) return false;
    manager->text_mode_ = TextMode::None;
    manager->switchToMainView();
    manager->refresh();
    return true;
}

void UiManager::tickCallback(void* context) {
    auto* manager = static_cast<UiManager*>(context);
    if(manager) manager->tick();
}

void UiManager::textInputDone(void* context) {
    auto* manager = static_cast<UiManager*>(context);
    if(!manager) return;
    const TextMode mode = manager->text_mode_;
    manager->text_mode_ = TextMode::None;
    if(mode == TextMode::Search) manager->completeSearch();
    else if(mode == TextMode::Rename) manager->completeRename();
}

void UiManager::manualIdDone(void* context) {
    auto* manager = static_cast<UiManager*>(context);
    if(manager) manager->completeManualId();
}
