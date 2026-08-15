/**
 * @file ui_screens.cpp
 * @brief Concrete draw and input behavior for every stack-managed application screen.
 */

#include "ui_screens.h"

#include "amiibo_db.h"
#include "amiibo_nfc.h"
#include "amiibo_storage.h"
#include "ui_controls.h"
#include "ui_manager.h"

#include <gui/elements.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {
/** @brief Return the number of actions available for a saved/library figure variant. */
uint8_t figureActionCount(bool saved, bool v3) {
    return saved ? (v3 ? 7U : 6U) : 3U;
}

/** @brief Return the display label for one figure action index. */
const char* figureAction(bool saved, bool v3, uint8_t index) {
    if(saved) {
        static const char* standard_actions[] = {
            "Emulate + autosave",
            "Dump details",
            "Compatibility",
            "Rename",
            "Fresh copy",
            "Delete",
        };
        static const char* v3_actions[] = {
            "Emulate + autosave",
            "Dump details",
            "Compatibility",
            "Change lock-on",
            "Rename",
            "Fresh copy",
            "Delete",
        };
        if(v3) return index < COUNT_OF(v3_actions) ? v3_actions[index] : "";
        return index < COUNT_OF(standard_actions) ? standard_actions[index] : "";
    }
    static const char* actions[] = {"Emulate once", "Save + emulate", "Compatibility"};
    return index < COUNT_OF(actions) ? actions[index] : "";
}

/** @brief Return the user-facing label for a database worker stage. */
const char* databaseProgressLabel(AzDbProgressStage stage) {
    switch(stage) {
    case AzDbProgressChecking: return "Checking source files";
    case AzDbProgressAmiibo: return "Reading amiibo.json";
    case AzDbProgressSorting: return "Sorting figures";
    case AzDbProgressGames: return "Reading games_info.json";
    case AzDbProgressFinalizing: return "Finalizing index";
    case AzDbProgressDone: return "Database ready";
    default: return "Preparing database";
    }
}

/** @brief Apply Up/Down input to a screen-local wrapped-text scroll offset. */
void handleScrollInput(uint16_t& scroll, const InputEvent& event) {
    if(event.key == InputKeyUp) {
        if(scroll > 0) --scroll;
    } else if(event.key == InputKeyDown) {
        if(scroll < UINT16_MAX) ++scroll;
    }
}
} // namespace

ScreenId HomeScreen::id() const {
    return ScreenId::Home;
}

void HomeScreen::draw(Canvas* canvas) {
    static const char* labels[] = {
        "Browse library", "Saved figures", "Advanced", "Setup & status", "About"};
    UiControls::header(canvas, ui_, AZ_APP_NAME);
    uint8_t start = selection_ >= 2 ? static_cast<uint8_t>(selection_ - 2) : 0;
    if(static_cast<size_t>(start) + AZ_LIST_ROWS > COUNT_OF(labels)) {
        start = static_cast<uint8_t>(COUNT_OF(labels) - AZ_LIST_ROWS);
    }
    for(uint8_t row = 0; row < AZ_LIST_ROWS; ++row) {
        const uint8_t index = static_cast<uint8_t>(start + row);
        UiControls::listRow(canvas, ui_, row, labels[index], selection_ == index);
    }
    UiControls::listScrollbar(canvas, selection_, COUNT_OF(labels));
    UiControls::footer(canvas, "", "OK", "");
}

bool HomeScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp) {
        moveSelection(-1, 5);
    } else if(event.key == InputKeyDown) {
        moveSelection(1, 5);
    } else if(isShortOk(event)) {
        Screen* next = nullptr;
        switch(selection_) {
        case 0: next = new(std::nothrow) CategoriesScreen(ui_); break;
        case 1: next = new(std::nothrow) SavedScreen(ui_); break;
        case 2: next = new(std::nothrow) AdvancedScreen(ui_); break;
        case 3: next = new(std::nothrow) StatusScreen(ui_); break;
        case 4: next = new(std::nothrow) AboutScreen(ui_); break;
        default: break;
        }
        if(!next || !ui_.push(next)) ui_.toast("Could not open screen");
        return true;
    }
    return true;
}

BackAction HomeScreen::onBack() {
    return BackAction::Exit;
}

ScreenId CategoriesScreen::id() const {
    return ScreenId::Categories;
}

void CategoriesScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, "Library categories");
    AzCategory rows[AZ_LIST_ROWS] = {};
    uint8_t row_count = 0;
    uint16_t window_start = 0;
    uint16_t total = 0;
    az_db_get_category_window(
        ui_.app().storage, selection_, rows, &row_count, &window_start, &total);
    count_ = total;
    if(count_ && selection_ >= count_) selection_ = static_cast<uint16_t>(count_ - 1U);

    for(uint8_t i = 0; i < row_count; ++i) {
        char label[AZ_NAME_MAX + 16];
        snprintf(label, sizeof(label), "%s  (%u)", rows[i].name, rows[i].count);
        const uint16_t absolute = static_cast<uint16_t>(window_start + i);
        UiControls::listRow(canvas, ui_, i, label, absolute == selection_);
    }
    if(count_ == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "No categories");
    }
    UiControls::listScrollbar(canvas, selection_, count_);
    UiControls::footer(canvas, "< Search", "OK", "");
}

bool CategoriesScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp) {
        moveSelection(-1, count_);
    } else if(event.key == InputKeyDown) {
        moveSelection(1, count_);
    } else if(event.type == InputTypeShort && event.key == InputKeyLeft) {
        ui_.openSearchInput();
        return true;
    } else if(isShortOk(event) && count_) {
        AzCategory category = {};
        if(az_db_get_category(ui_.app().storage, selection_, &category)) {
            ui_.app().current_category = category;
            Screen* next = new(std::nothrow) FiguresScreen(ui_);
            if(!next || !ui_.push(next)) ui_.toast("Could not open category");
        }
        return true;
    }
    return true;
}

ScreenId FiguresScreen::id() const {
    return ScreenId::Figures;
}

void FiguresScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, ui_.app().current_category.name);
    AzFigure rows[AZ_LIST_ROWS] = {};
    uint8_t row_count = 0;
    uint16_t window_start = 0;
    uint16_t total = 0;
    az_db_get_figure_window(
        ui_.app().storage,
        ui_.app().current_category.id,
        selection_,
        rows,
        &row_count,
        &window_start,
        &total);
    count_ = total;
    if(count_ && selection_ >= count_) selection_ = static_cast<uint16_t>(count_ - 1U);

    for(uint8_t i = 0; i < row_count; ++i) {
        const uint16_t absolute = static_cast<uint16_t>(window_start + i);
        UiControls::listRow(canvas, ui_, i, rows[i].name, absolute == selection_);
    }
    if(count_ == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "No entries");
    }
    UiControls::listScrollbar(canvas, selection_, count_);
    UiControls::footer(canvas, "< Search", "OK", "");
}

bool FiguresScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp) {
        moveSelection(-1, count_);
    } else if(event.key == InputKeyDown) {
        moveSelection(1, count_);
    } else if(event.type == InputTypeShort && event.key == InputKeyLeft) {
        ui_.openSearchInput();
        return true;
    } else if(isShortOk(event) && count_) {
        AzFigure figure = {};
        if(az_db_get_figure(
               ui_.app().storage, ui_.app().current_category.id, selection_, &figure)) {
            ui_.app().current_figure = figure;
            ui_.app().current_is_saved = false;
            ui_.app().current_saved_filename[0] = '\0';
            ui_.app().current_lockon_valid = false;
            ui_.app().current_lockon_filename[0] = '\0';
            Screen* next = new(std::nothrow) FigureScreen(ui_);
            if(!next || !ui_.push(next)) ui_.toast("Could not open figure");
        }
        return true;
    }
    return true;
}

SearchResultsScreen::SearchResultsScreen(UiManager& ui, const char* query) :
    Screen(ui), count_(0) {
    az_str_copy(query_, sizeof(query_), query);
}

ScreenId SearchResultsScreen::id() const {
    return ScreenId::SearchResults;
}

const char* SearchResultsScreen::query() const {
    return query_;
}

void SearchResultsScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, "Search results");
    AzFigure rows[AZ_LIST_ROWS] = {};
    uint8_t row_count = 0;
    uint16_t window_start = 0;
    uint16_t total = 0;
    az_db_search_window(
        ui_.app().storage,
        query_,
        selection_,
        rows,
        &row_count,
        &window_start,
        &total);
    count_ = total;
    if(count_ && selection_ >= count_) selection_ = static_cast<uint16_t>(count_ - 1U);

    for(uint8_t i = 0; i < row_count; ++i) {
        const uint16_t absolute = static_cast<uint16_t>(window_start + i);
        UiControls::listRow(canvas, ui_, i, rows[i].name, absolute == selection_);
    }
    if(count_ == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, "No entries");
    }
    UiControls::listScrollbar(canvas, selection_, count_);
    UiControls::footer(canvas, "< Search", "OK", "");
}

bool SearchResultsScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp) {
        moveSelection(-1, count_);
    } else if(event.key == InputKeyDown) {
        moveSelection(1, count_);
    } else if(event.type == InputTypeShort && event.key == InputKeyLeft) {
        ui_.openSearchInput();
        return true;
    } else if(isShortOk(event) && count_) {
        AzFigure figure = {};
        if(az_db_search_get(ui_.app().storage, query_, selection_, &figure)) {
            ui_.app().current_figure = figure;
            ui_.app().current_is_saved = false;
            ui_.app().current_saved_filename[0] = '\0';
            ui_.app().current_lockon_valid = false;
            ui_.app().current_lockon_filename[0] = '\0';
            Screen* next = new(std::nothrow) FigureScreen(ui_);
            if(!next || !ui_.push(next)) ui_.toast("Could not open figure");
        }
        return true;
    }
    return true;
}

SavedScreen::SavedScreen(UiManager& ui, const char* preferred_filename) :
    Screen(ui), entries_(nullptr), count_(0) {
    az_str_copy(preferred_filename_, sizeof(preferred_filename_), preferred_filename);
}

SavedScreen::~SavedScreen() {
    delete[] entries_;
}

ScreenId SavedScreen::id() const {
    return ScreenId::Saved;
}

bool SavedScreen::onPushed() {
    if(!refresh(preferred_filename_)) ui_.toast("Saved catalog unavailable");
    preferred_filename_[0] = '\0';
    return true;
}

void SavedScreen::onRevealed() {
    const char* preferred = ui_.app().current_is_saved ? ui_.app().current_saved_filename : nullptr;
    if(!refresh(preferred)) ui_.toast("Saved catalog unavailable");
}

bool SavedScreen::refresh(const char* preferred_filename) {
    if(!entries_) {
        entries_ = new(std::nothrow) AzSavedEntry[AZ_MAX_SAVED]{};
        if(!entries_) {
            count_ = 0;
            return false;
        }
    }
    memset(entries_, 0, sizeof(AzSavedEntry) * AZ_MAX_SAVED);
    const uint16_t fallback = selection_;
    count_ = az_saved_scan(ui_.app().storage, entries_, AZ_MAX_SAVED);
    selection_ = selectionForFilename(preferred_filename, fallback);
    return true;
}

uint16_t SavedScreen::selectionForFilename(const char* filename, uint16_t fallback) const {
    if(!entries_ || count_ == 0) return 0;
    if(filename && filename[0]) {
        for(uint16_t i = 0; i < count_; ++i) {
            if(strcmp(entries_[i].filename, filename) == 0) return i;
        }
    }
    return fallback < count_ ? fallback : static_cast<uint16_t>(count_ - 1U);
}

uint16_t SavedScreen::count() const {
    return count_;
}

uint16_t SavedScreen::selection() const {
    return selection_;
}

void SavedScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, "Saved figures");
    uint16_t start = selection_ >= 2 ? static_cast<uint16_t>(selection_ - 2) : 0;
    if(count_ > AZ_LIST_ROWS && start + AZ_LIST_ROWS > count_) {
        start = static_cast<uint16_t>(count_ - AZ_LIST_ROWS);
    }
    for(uint8_t row = 0; row < AZ_LIST_ROWS && start + row < count_; ++row) {
        if(!entries_) break;
        const AzSavedEntry& entry = entries_[start + row];
        UiControls::listRow(canvas, ui_, row, entry.display_name, start + row == selection_);
    }
    if(count_ == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, "No saved figures yet");
        canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignCenter, "Browse to create one");
    }
    UiControls::listScrollbar(canvas, selection_, count_);
    UiControls::footer(canvas, "", "OK", "");
}

bool SavedScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp) {
        moveSelection(-1, count_);
    } else if(event.key == InputKeyDown) {
        moveSelection(1, count_);
    } else if(isShortOk(event)) {
        if(count_ == 0) {
            ui_.schedulePop();
        } else {
            openSelected();
        }
        return true;
    }
    return true;
}

void SavedScreen::openSelected() {
    if(!entries_ || count_ == 0 || selection_ >= count_) return;
    AmiiboZeroApp& app = ui_.app();
    const AzSavedEntry& entry = entries_[selection_];
    memset(&app.current_figure, 0, sizeof(app.current_figure));
    memcpy(app.current_figure.id, entry.id, 8);
    app.current_figure.category = entry.id[6];
    app.current_figure.type = entry.id[3];
    if(!az_db_find_by_id(app.storage, entry.id, &app.current_figure)) {
        az_str_copy(app.current_figure.name, sizeof(app.current_figure.name), entry.display_name);
        static const char hex[] = "0123456789abcdef";
        for(size_t i = 0; i < 8; ++i) {
            app.current_figure.id_hex[i * 2] = hex[entry.id[i] >> 4];
            app.current_figure.id_hex[i * 2 + 1] = hex[entry.id[i] & 0x0F];
        }
        app.current_figure.id_hex[16] = '\0';
    }
    app.current_is_saved = true;
    az_str_copy(app.current_saved_filename, sizeof(app.current_saved_filename), entry.filename);
    memset(&app.current_details, 0, sizeof(app.current_details));

    char path[AZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, app.current_saved_filename);
    nfc_device_clear(app.nfc_device);
    if(az_nfc_load_device(app.nfc_device, path)) {
        az_nfc_read_details(app.nfc_device, &app.keys, &app.current_details);
        app.current_lockon_valid = az_nfc_device_is_v3(app.nfc_device) &&
                                   az_saved_lockon_load(
                                       app.storage,
                                       app.current_saved_filename,
                                       app.current_lockon_sram);
        app.current_lockon_filename[0] = '\0';
    } else {
        app.current_lockon_valid = false;
        app.current_lockon_filename[0] = '\0';
    }

    Screen* next = new(std::nothrow) FigureScreen(ui_);
    if(!next || !ui_.push(next)) ui_.toast("Could not open figure");
}

ScreenId AdvancedScreen::id() const {
    return ScreenId::Advanced;
}

void AdvancedScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, "Advanced");
    UiControls::listRow(canvas, ui_, 0, "Manual figure ID", true);
    UiControls::footer(canvas, "Back", "OK", "");
}

bool AdvancedScreen::handleInput(const InputEvent& event) {
    if(isShortOk(event)) {
        ui_.openManualIdInput();
        return true;
    }
    return true;
}

LockOnScreen::LockOnScreen(UiManager& ui, LockOnAction action) :
    Screen(ui), action_(action), entries_(nullptr), count_(0) {
    if(!loadCatalog()) ui_.toast("Lock-on catalog unavailable");
}

LockOnScreen::~LockOnScreen() {
    delete[] entries_;
}

bool LockOnScreen::loadCatalog() {
    entries_ = new(std::nothrow) AzLockOnEntry[AZ_MAX_LOCKONS]{};
    if(!entries_) return false;
    count_ = az_lockon_scan(ui_.app().storage, entries_, AZ_MAX_LOCKONS);
    return true;
}

ScreenId LockOnScreen::id() const {
    return ScreenId::LockOn;
}

void LockOnScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, "Select lock-on");
    uint16_t start = selection_ >= 2 ? static_cast<uint16_t>(selection_ - 2) : 0;
    if(count_ > AZ_LIST_ROWS && start + AZ_LIST_ROWS > count_) {
        start = static_cast<uint16_t>(count_ - AZ_LIST_ROWS);
    }
    for(uint8_t row = 0; row < AZ_LIST_ROWS && start + row < count_; ++row) {
        UiControls::listRow(
            canvas, ui_, row, entries_[start + row].display_name, start + row == selection_);
    }
    if(count_ == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 27, AlignCenter, AlignCenter, "No lock-on files");
        canvas_draw_str_aligned(canvas, 64, 39, AlignCenter, AlignCenter, "Put files in /lock_on/");
    }
    UiControls::listScrollbar(canvas, selection_, count_);
    UiControls::footer(canvas, "Back", "OK", "");
}

bool LockOnScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp) {
        moveSelection(-1, count_);
    } else if(event.key == InputKeyDown) {
        moveSelection(1, count_);
    } else if(isShortOk(event) && count_) {
        applySelected();
        return true;
    }
    return true;
}

void LockOnScreen::applySelected() {
    if(!entries_ || selection_ >= count_) return;
    AmiiboZeroApp& app = ui_.app();
    const AzLockOnEntry& selected = entries_[selection_];
    uint8_t sram[AZ_LOCKON_SRAM_SIZE];
    if(!az_lockon_load(app.storage, selected.filename, sram)) {
        ui_.toast("Could not read lock-on");
        return;
    }

    memcpy(app.current_lockon_sram, sram, sizeof(sram));
    app.current_lockon_valid = true;
    az_str_copy(
        app.current_lockon_filename, sizeof(app.current_lockon_filename), selected.filename);

    switch(action_) {
    case LockOnAction::GenerateTemporary:
        if(!ui_.beginEmulation(false, true, true)) ui_.schedulePop();
        break;
    case LockOnAction::GeneratePersistent:
        if(!ui_.beginEmulation(true, true, true)) ui_.schedulePop();
        break;
    case LockOnAction::EmulateSaved:
        if(az_saved_lockon_save(app.storage, app.current_saved_filename, app.current_lockon_sram)) {
            if(!ui_.beginEmulation(true, false, true)) ui_.schedulePop();
        } else {
            ui_.toast("Could not attach lock-on");
            ui_.schedulePop();
        }
        break;
    case LockOnAction::ReplaceSaved: {
        const bool ok = az_saved_lockon_save(
            app.storage, app.current_saved_filename, app.current_lockon_sram);
        ui_.toast(ok ? "Lock-on changed" : "Lock-on change failed");
        ui_.schedulePop();
        break;
    }
    }
}

ScreenId FigureScreen::id() const {
    return ScreenId::Figure;
}

void FigureScreen::draw(Canvas* canvas) {
    const AmiiboZeroApp& app = ui_.app();
    UiControls::header(
        canvas, ui_, app.current_figure.name[0] ? app.current_figure.name : "Amiibo");
    canvas_set_font(canvas, FontSecondary);

    char type_line[48];
    snprintf(
        type_line,
        sizeof(type_line),
        "Type: %s%s",
        az_figure_type_name(app.current_figure.type),
        az_figure_is_v3(app.current_figure.id) ? " / Lock-on" : "");
    UiControls::fitted(canvas, ui_, 2, 20, type_line, 124);

    char id_line[32];
    snprintf(id_line, sizeof(id_line), "ID %s", app.current_figure.id_hex);
    UiControls::fitted(canvas, ui_, 2, 29, id_line, 124);

    const bool v3 = az_figure_is_v3(app.current_figure.id);
    const uint8_t count = figureActionCount(app.current_is_saved, v3);
    uint8_t start_row = selection_ >= 2 ? static_cast<uint8_t>(selection_ - 2) : 0;
    if(count > 3 && start_row + 3 > count) start_row = static_cast<uint8_t>(count - 3);
    for(uint8_t row = 0; row < 3 && start_row + row < count; ++row) {
        const uint8_t index = static_cast<uint8_t>(start_row + row);
        const int top = 31 + row * 8;
        const int baseline = top + 7;
        const bool selected = index == selection_;
        if(selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rbox(canvas, 1, top, 124, 8, 2);
            canvas_set_color(canvas, ColorWhite);
            UiControls::marquee(
                canvas, ui_, 4, baseline, figureAction(app.current_is_saved, v3, index), 118);
            canvas_set_color(canvas, ColorBlack);
        } else {
            UiControls::fitted(
                canvas, ui_, 4, baseline, figureAction(app.current_is_saved, v3, index), 118);
        }
    }
    if(count > 3) elements_scrollbar_pos(canvas, 125, 31, 24, selection_, count);
}

bool FigureScreen::handleInput(const InputEvent& event) {
    AmiiboZeroApp& app = ui_.app();
    const bool v3 = az_figure_is_v3(app.current_figure.id);
    const uint8_t action_count = figureActionCount(app.current_is_saved, v3);
    if(event.key == InputKeyUp) {
        moveSelection(-1, action_count);
    } else if(event.key == InputKeyDown) {
        moveSelection(1, action_count);
    } else if(isShortOk(event)) {
        if(app.current_is_saved) {
            if(selection_ == 0) {
                ui_.requestEmulation(true, false);
            } else if(selection_ == 1) {
                Screen* next = new(std::nothrow) DumpInfoScreen(ui_);
                if(!next || !ui_.push(next)) ui_.toast("Could not open details");
            } else if(selection_ == 2) {
                Screen* next = new(std::nothrow) GamesScreen(ui_);
                if(!next || !ui_.push(next)) ui_.toast("Could not open compatibility");
            } else if(v3 && selection_ == 3) {
                Screen* next = new(std::nothrow) LockOnScreen(ui_, LockOnAction::ReplaceSaved);
                if(!next || !ui_.push(next)) ui_.toast("Could not open lock-on list");
            } else if((v3 && selection_ == 4) || (!v3 && selection_ == 3)) {
                ui_.openRenameInput();
            } else if((v3 && selection_ == 5) || (!v3 && selection_ == 4)) {
                ui_.requestEmulation(true, true);
            } else if((v3 && selection_ == 6) || (!v3 && selection_ == 5)) {
                Screen* next = new(std::nothrow) ConfirmDeleteScreen(ui_);
                if(!next || !ui_.push(next)) ui_.toast("Could not open confirmation");
            }
        } else {
            if(selection_ == 0) {
                ui_.requestEmulation(false, true);
            } else if(selection_ == 1) {
                ui_.requestEmulation(true, true);
            } else if(selection_ == 2) {
                Screen* next = new(std::nothrow) GamesScreen(ui_);
                if(!next || !ui_.push(next)) ui_.toast("Could not open compatibility");
            }
        }
        return true;
    }
    return true;
}

ScreenId DumpInfoScreen::id() const {
    return ScreenId::DumpInfo;
}

void DumpInfoScreen::draw(Canvas* canvas) {
    const AmiiboZeroApp& app = ui_.app();
    UiControls::header(
        canvas, ui_, app.current_figure.name[0] ? app.current_figure.name : "Dump details");
    char text[640];
    const AzAmiiboDetails& details = app.current_details;
    const bool v3 = az_figure_is_v3(app.current_figure.id);
    const char* lockon_line =
        v3 ? (app.current_lockon_valid ? "\nLock-on: attached" : "\nLock-on: missing") : "";
    if(!details.available) {
        snprintf(
            text,
            sizeof(text),
            "Type: %s%s\nID: %s\nFile: %s%s\nEncrypted details unavailable. A valid key_retail.bin is required and the dump must authenticate.",
            az_figure_type_name(app.current_figure.type),
            v3 ? " / Lock-on" : "",
            app.current_figure.id_hex,
            app.current_saved_filename,
            lockon_line);
    } else {
        const unsigned long app_hi = static_cast<unsigned long>(details.application_id >> 32);
        const unsigned long app_lo =
            static_cast<unsigned long>(details.application_id & 0xFFFFFFFFULL);
        snprintf(
            text,
            sizeof(text),
            "Type: %s%s\nID: %s\nFile: %s%s\nNickname: %s\nOwner Mii: %s\nInitialized: %s\nApp data: %s\nRegistered: %s\nLast write: %s\nWrite count: %u\nApplication: %08lX%08lX\nArea ID: %08lX\nApp writes: %u",
            az_figure_type_name(app.current_figure.type),
            v3 ? " / Lock-on" : "",
            app.current_figure.id_hex,
            app.current_saved_filename,
            lockon_line,
            details.nickname[0] ? details.nickname : "(none)",
            details.owner_mii[0] ? details.owner_mii : "(none)",
            details.initialized ? "yes" : "no",
            details.app_data_initialized ? "yes" : "no",
            details.init_date[0] ? details.init_date : "-",
            details.write_date[0] ? details.write_date : "-",
            details.write_counter,
            app_hi,
            app_lo,
            static_cast<unsigned long>(details.application_area_id),
            details.application_write_counter);
    }
    UiControls::wrappedText(
        canvas, text, 20, UiControls::DetailLines, 120, scroll_);
    UiControls::footer(canvas, "Back", "Up/Dn", "");
}

bool DumpInfoScreen::handleInput(const InputEvent& event) {
    handleScrollInput(scroll_, event);
    return true;
}

GamesScreen::GamesScreen(UiManager& ui) : Screen(ui), entries_(nullptr), count_(0) {
    entries_ = new(std::nothrow) AzGame[AZ_MAX_GAMES]{};
    if(!entries_) {
        ui_.toast("Not enough memory for games");
        return;
    }
    if(!az_db_load_games(
           ui_.app().storage,
           ui_.app().current_figure.id,
           entries_,
           AZ_MAX_GAMES,
           &count_)) {
        ui_.toast("Could not read games DB");
    }
}

GamesScreen::~GamesScreen() {
    delete[] entries_;
}

ScreenId GamesScreen::id() const {
    return ScreenId::Games;
}

void GamesScreen::draw(Canvas* canvas) {
    char title[48];
    if(count_) {
        snprintf(title, sizeof(title), "Compatibility %u/%u", selection_ + 1, count_);
    } else {
        az_str_copy(title, sizeof(title), "Compatibility");
    }
    UiControls::header(canvas, ui_, title);

    if(count_ == 0) {
        UiControls::wrappedText(
            canvas,
            "No compatibility records were found for this Amiibo in games_info.json.",
            20,
            UiControls::DetailLines,
            120,
            scroll_);
    } else {
        const AzGame& game = entries_[selection_];
        char detail[AZ_NAME_MAX + AZ_USAGE_MAX + 64];
        snprintf(
            detail,
            sizeof(detail),
            "%s\n%s - %s\n%s",
            game.name,
            game.platform,
            game.writes ? "writes tag data" : "read only",
            game.usage);
        UiControls::wrappedText(
            canvas, detail, 20, UiControls::DetailLines, 120, scroll_);
    }
    UiControls::footer(canvas, "< Prev", "Up/Dn", "Next >");
}

bool GamesScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp || event.key == InputKeyDown) {
        handleScrollInput(scroll_, event);
    } else if(event.key == InputKeyLeft && count_) {
        if(selection_ > 0) --selection_;
        scroll_ = 0;
        ui_.resetAnimation();
    } else if(event.key == InputKeyRight && count_) {
        if(selection_ + 1U < count_) ++selection_;
        scroll_ = 0;
        ui_.resetAnimation();
    }
    return true;
}

EmulationScreen::EmulationScreen(UiManager& ui) : Screen(ui), active_(true) {
}

EmulationScreen::~EmulationScreen() {
    if(active_) ui_.stopEmulation();
}

void EmulationScreen::onPopped() {
    if(!active_) return;
    ui_.stopEmulation();
    active_ = false;
}

ScreenId EmulationScreen::id() const {
    return ScreenId::Emulation;
}

void EmulationScreen::draw(Canvas* canvas) {
    const AmiiboZeroApp& app = ui_.app();
    UiControls::header(canvas, ui_, "Emulating");
    canvas_set_font(canvas, FontPrimary);
    UiControls::marquee(canvas, ui_, 2, 21, app.current_figure.name, 124, true);
    const int radius = 4 + ((ui_.animation() / 2) % 2);
    canvas_draw_circle(canvas, 15, 43, radius);
    canvas_draw_circle(canvas, 15, 43, radius + 4);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(
        canvas, 29, 42, app.current_is_saved ? "Writes autosave" : "Temporary session");
    canvas_draw_str(canvas, 29, 51, "OK: randomize UID");
    UiControls::footer(canvas, "Back", "OK UID", "");
}

bool EmulationScreen::handleInput(const InputEvent& event) {
    if(isShortOk(event)) {
        ui_.randomizeEmulationUid();
        return true;
    }
    return true;
}

ScreenId StatusScreen::id() const {
    return ScreenId::Status;
}

void StatusScreen::draw(Canvas* canvas) {
    const AmiiboZeroApp& app = ui_.app();
    UiControls::header(canvas, ui_, "Setup & status");
    char text[320];
    snprintf(
        text,
        sizeof(text),
        "%s\n%s\n%s\n%s\nIndex: %s (%lu figures)\nOK reloads keys and forces a background index rebuild.",
        app.keys.valid ? "Keys: ready" : "Keys: missing/invalid",
        storage_file_exists(app.storage, AZ_AMIIBO_JSON) ? "Amiibo JSON: found" :
                                                          "Amiibo JSON: missing",
        storage_file_exists(app.storage, AZ_GAMES_JSON) ? "Games JSON: found" :
                                                         "Games JSON: missing",
        "Index identity: size + samples",
        app.index_ready ? "ready" : "unavailable",
        static_cast<unsigned long>(app.index_count));
    UiControls::wrappedText(
        canvas, text, 20, UiControls::DetailLines, 120, scroll_);
    UiControls::footer(canvas, "Back", "OK", "");
}

bool StatusScreen::handleInput(const InputEvent& event) {
    if(event.key == InputKeyUp || event.key == InputKeyDown) {
        handleScrollInput(scroll_, event);
    } else if(isShortOk(event)) {
        ui_.refreshStatus();
        return true;
    }
    return true;
}

ScreenId AboutScreen::id() const {
    return ScreenId::About;
}

void AboutScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, "About");
    char text[192];
    snprintf(
        text,
        sizeof(text),
        "Amiibo Zero %s\nBrowse, generate, emulate, and persist Amiibo.\nJSON: AmiiboAPI + lwJSON. Crypto: mbedTLS.\nKeys/dumps are never bundled.",
        AZ_APP_VERSION);
    UiControls::wrappedText(
        canvas, text, 20, UiControls::DetailLines, 120, scroll_);
    UiControls::footer(canvas, "Back", "Up/Dn", "");
}

bool AboutScreen::handleInput(const InputEvent& event) {
    handleScrollInput(scroll_, event);
    return true;
}

ScreenId ConfirmDeleteScreen::id() const {
    return ScreenId::ConfirmDelete;
}

void ConfirmDeleteScreen::draw(Canvas* canvas) {
    UiControls::header(canvas, ui_, "Delete saved figure?");
    canvas_set_font(canvas, FontPrimary);
    UiControls::marquee(canvas, ui_, 2, 24, ui_.app().current_figure.name, 124, true);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "This cannot be undone.");
    UiControls::footer(canvas, "Back", "OK Del", "");
}

bool ConfirmDeleteScreen::handleInput(const InputEvent& event) {
    if(!isShortOk(event)) return true;

    AmiiboZeroApp& app = ui_.app();
    if(!az_saved_delete(app.storage, app.current_saved_filename)) {
        ui_.toast("Delete failed");
        return true;
    }

    Screen* saved_base = ui_.find(ScreenId::Saved);
    if(saved_base) {
        auto* saved = static_cast<SavedScreen*>(saved_base);
        if(!saved->refresh()) ui_.toast("Deleted; catalog refresh failed");
    }
    app.current_is_saved = false;
    app.current_saved_filename[0] = '\0';
    ui_.toast("Figure deleted");
    ui_.schedulePop(2);
    return true;
}

ScreenId WorkingScreen::id() const {
    return ScreenId::Working;
}

void WorkingScreen::draw(Canvas* canvas) {
    const AmiiboZeroApp& app = ui_.app();
    UiControls::header(canvas, ui_, "Preparing database");
    const uint8_t phase = (ui_.animation() / 2U) & 1U;

    canvas_draw_frame(canvas, 7, 17, 14, 16);
    canvas_draw_line(canvas, 9, 19, 19, 19);
    canvas_draw_line(canvas, 9, 31, 19, 31);
    if(phase == 0U) {
        canvas_draw_line(canvas, 10, 21, 18, 21);
        canvas_draw_line(canvas, 12, 23, 16, 23);
    } else {
        canvas_draw_line(canvas, 12, 27, 16, 27);
        canvas_draw_line(canvas, 10, 29, 18, 29);
    }

    canvas_set_font(canvas, FontSecondary);
    UiControls::fitted(
        canvas, ui_, 26, 28, databaseProgressLabel(app.db_progress_stage), 98);

    canvas_draw_frame(canvas, 13, 38, 102, 8);
    const uint8_t progress = app.db_progress > 100U ? 100U : app.db_progress;
    if(progress) canvas_draw_box(canvas, 14, 39, progress, 6);

    char percent[8];
    snprintf(percent, sizeof(percent), "%u%%", progress);
    canvas_draw_str_aligned(canvas, 64, 53, AlignCenter, AlignBottom, percent);
}

bool WorkingScreen::handleInput(const InputEvent&) {
    return true;
}

BackAction WorkingScreen::onBack() {
    return BackAction::Cancel;
}
