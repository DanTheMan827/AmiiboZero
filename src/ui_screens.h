/**
 * @file ui_screens.h
 * @brief Concrete stack-managed screen classes for Amiibo Zero.
 */

#pragma once

#include "ui_screen.h"

/** @brief Root menu screen. Back exits the application instead of popping the root. */
class HomeScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
    /** @copydoc Screen::onBack */
    BackAction onBack() override;
};

/** @brief Browses indexed Amiibo categories and opens a FiguresScreen for the selected category. */
class CategoriesScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;

private:
    uint16_t count_ = 0; /**< Total category count reported by the current database query. */
};

/** @brief Browses figures belonging to the currently selected category. */
class FiguresScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;

private:
    uint16_t count_ = 0; /**< Total figure count for the current category. */
};

/** @brief Browses database figures matching one immutable search query. */
class SearchResultsScreen final : public Screen {
public:
    /**
     * @brief Construct a result screen for one query.
     * @param ui Owning UI manager.
     * @param query Search text copied into screen-local storage.
     */
    SearchResultsScreen(UiManager& ui, const char* query);

    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;

    /** @brief Return the screen-local query used by the native search editor. */
    const char* query() const;

private:
    char query_[AZ_QUERY_MAX]; /**< Search query retained for this stack entry. */
    uint16_t count_; /**< Total number of matching figures. */
};

/** @brief Owns and displays the catalog of saved `.nfc` figure files. */
class SavedScreen final : public Screen {
public:
    /**
     * @brief Construct the saved-figure browser.
     * @param ui Owning UI manager.
     * @param preferred_filename Optional filename to select after the next catalog scan.
     */
    SavedScreen(UiManager& ui, const char* preferred_filename = nullptr);

    /** @brief Release the screen-owned saved-file catalog. */
    ~SavedScreen() override;

    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
    /** @copydoc Screen::onPushed */
    bool onPushed() override;
    /** @copydoc Screen::onRevealed */
    void onRevealed() override;

    /**
     * @brief Rescan saved figures and preserve/select an appropriate row.
     * @param preferred_filename Optional exact filename to prefer after the rescan.
     * @return true when the catalog buffer exists and the scan completes.
     */
    bool refresh(const char* preferred_filename = nullptr);

    /** @brief Return the number of cataloged saved figures. */
    uint16_t count() const;

    /** @brief Return the current saved-figure selection. */
    uint16_t selection() const;

private:
    AzSavedEntry* entries_; /**< Screen-owned bounded saved-file catalog. */
    uint16_t count_; /**< Number of populated catalog entries. */
    char preferred_filename_[96]; /**< Filename requested for selection after first scan. */

    /** @brief Load the selected saved figure into shared runtime state and push FigureScreen. */
    void openSelected();

    /**
     * @brief Find a preferred filename in the current catalog.
     * @param filename Filename to locate.
     * @param fallback Selection to use when no match exists.
     * @return Matching or clamped fallback selection.
     */
    uint16_t selectionForFilename(const char* filename, uint16_t fallback) const;
};

/** @brief Menu for advanced/manual lookup tools. */
class AdvancedScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
};

/** @brief Owns and displays Lock-On payload choices for one deferred version-3 action. */
class LockOnScreen final : public Screen {
public:
    /**
     * @brief Construct a Lock-On picker for a deferred operation.
     * @param ui Owning UI manager.
     * @param action Operation to execute after a payload is selected.
     */
    LockOnScreen(UiManager& ui, LockOnAction action);

    /** @brief Release the screen-owned Lock-On catalog. */
    ~LockOnScreen() override;

    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;

private:
    LockOnAction action_; /**< Deferred operation associated with this picker. */
    AzLockOnEntry* entries_; /**< Screen-owned Lock-On catalog. */
    uint16_t count_; /**< Number of cataloged Lock-On files. */

    /** @brief Allocate and populate the Lock-On catalog. */
    bool loadCatalog();

    /** @brief Load the selected payload and execute/schedule action_. */
    void applySelected();
};

/** @brief Displays actions for the current library or saved figure. */
class FigureScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
};

/** @brief Displays authenticated/decrypted details for the current saved figure. */
class DumpInfoScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
};

/** @brief Owns and displays game-compatibility records for the current figure. */
class GamesScreen final : public Screen {
public:
    /**
     * @brief Load compatibility entries for the current figure.
     * @param ui Owning UI manager.
     */
    explicit GamesScreen(UiManager& ui);

    /** @brief Release the screen-owned compatibility array. */
    ~GamesScreen() override;

    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;

private:
    AzGame* entries_; /**< Screen-owned compatibility entries. */
    uint16_t count_; /**< Number of loaded compatibility entries. */
};

/** @brief Active NFC emulation screen with UID-randomization control. */
class EmulationScreen final : public Screen {
public:
    /**
     * @brief Construct a screen that adopts the NFC session already started by UiManager.
     * @param ui Owning UI manager.
     */
    explicit EmulationScreen(UiManager& ui);

    /** @brief Stop any still-active session before releasing the screen. */
    ~EmulationScreen() override;

    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
    /** @brief Stop/synchronize NFC when this screen leaves the stack. */
    void onPopped() override;

private:
    bool active_; /**< Whether this screen currently owns an active NFC session. */
};

/** @brief Displays key/database/index state and triggers a forced refresh. */
class StatusScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
};

/** @brief Displays application version and implementation information. */
class AboutScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
};

/** @brief Requires an explicit OK press before deleting the current saved figure. */
class ConfirmDeleteScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
};

/** @brief Modal database-preparation screen that vetoes Back until the worker completes. */
class WorkingScreen final : public Screen {
public:
    using Screen::Screen;
    /** @copydoc Screen::id */
    ScreenId id() const override;
    /** @copydoc Screen::draw */
    void draw(Canvas* canvas) override;
    /** @copydoc Screen::handleInput */
    bool handleInput(const InputEvent& event) override;
    /** @brief Veto Back while database preparation is active. */
    BackAction onBack() override;
};
