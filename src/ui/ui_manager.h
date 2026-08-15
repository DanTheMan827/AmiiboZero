/**
 * @file ui_manager.h
 * @brief UI resource owner, screen-stack navigator, and UI-level application coordinator.
 */

#pragma once

#include "../amiibo_zero.h"
#include "ui_screen.h"

#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Owns Flipper UI resources and an owning stack of Screen instances.
 * @details Only the top screen is drawn and receives custom-view input. Screens push child screens
 * through this manager. Back behavior is delegated to the top screen before the manager mutates the
 * stack, allowing a screen to cancel Back or perform cleanup safely.
 */
class UiManager {
public:
    /** @brief Maximum number of simultaneously retained custom screens. */
    static constexpr size_t MaxScreens = 12;

    /**
     * @brief Construct a manager for one application instance.
     * @param app Non-UI application state whose lifetime exceeds this manager.
     */
    explicit UiManager(AmiiboZeroApp& app);

    /** @brief Release owned screens and Flipper UI resources. */
    ~UiManager();

    UiManager(const UiManager&) = delete;
    UiManager& operator=(const UiManager&) = delete;

    /**
     * @brief Allocate and register Flipper views and push the appropriate initial screen.
     * @return true when initialization succeeds.
     */
    bool init();

    /** @brief Release every screen, modal control, custom view, and dispatcher resource. */
    void deinit();

    /** @brief Run the view dispatcher until the application UI requests exit. */
    void run();

    /** @brief Stop the view dispatcher. */
    void stop();

    /** @brief Return whether Setup/Status requested a full UI-session restart for a forced DB rebuild. */
    bool databaseRefreshRequested() const;

    /**
     * @brief Push a newly allocated screen.
     * @param screen Screen to add. UiManager takes ownership immediately and deletes it if the push
     * cannot be completed.
     * @return true when the screen becomes the new top item.
     */
    bool push(Screen* screen);

    /**
     * @brief Pop and destroy the top screen.
     * @details When the final screen is removed, the dispatcher is stopped and the app exits.
     * @return true when a screen was removed.
     */
    bool pop();

    /**
     * @brief Replace the current top screen.
     * @param screen Replacement screen whose ownership transfers to the manager.
     * @return true when replacement succeeds.
     */
    bool replace(Screen* screen);

    /**
     * @brief Request one or more pops after the active input callback returns.
     * @param count Number of screens to remove. Removing the final screen exits the app.
     */
    void schedulePop(uint8_t count = 1);

    /**
     * @brief Request pops followed by a push after the active input callback returns.
     * @param pop_count Number of screens to remove before pushing the replacement.
     * @param screen Screen whose ownership transfers immediately to the manager's deferred action.
     */
    void schedulePopAndPush(uint8_t pop_count, Screen* screen);

    /**
     * @brief Rebuild the ancestry of an existing stack item without deleting that item or its children.
     * @details Used when a newly generated persistent figure changes its logical parent from a library
     * screen to Saved Figures while an input handler is still executing.
     * @param child Identity of the retained screen whose ancestry should be replaced.
     * @param parent New direct child of the root screen; ownership transfers immediately.
     * @return true when the stack was successfully rebased.
     */
    bool rebaseScreenOnParent(ScreenId child, Screen* parent);

    /** @brief Return the current top screen, or nullptr for an empty stack. */
    Screen* top() const;

    /** @brief Find the nearest screen with the requested identity, searching from top to root. */
    Screen* find(ScreenId id) const;

    /** @brief Return whether the stack currently contains the requested screen identity. */
    bool contains(ScreenId id) const;

    /** @brief Return the current number of custom screens in the stack. */
    size_t depth() const;

    /** @brief Return mutable non-UI application state. */
    AmiiboZeroApp& app();

    /** @brief Return const non-UI application state. */
    const AmiiboZeroApp& app() const;

    /** @brief Invalidate the custom view so its top screen is redrawn. */
    void refresh();

    /** @brief Display a short-lived header message. */
    void toast(const char* text);

    /** @brief Return whether a toast is currently active. */
    bool toastActive() const;

    /** @brief Return the current toast text. */
    const char* toastText() const;

    /** @brief Return the monotonically advancing UI animation tick. */
    uint8_t animation() const;

    /** @brief Reset the animation tick, normally after a selection change. */
    void resetAnimation();

    /** @brief Return the reusable FuriString used by common drawing controls. */
    FuriString* scratch() const;

    /** @brief Return the current C-string contents of the reusable scratch string. */
    const char* scratchText() const;

    /** @brief Replace the reusable scratch string contents. */
    void setScratch(const char* text);

    /** @brief Open the native text-input control for library search. */
    void openSearchInput();

    /** @brief Open the native text-input control for renaming the current saved figure. */
    void openRenameInput();

    /** @brief Open the native byte-input control for manual eight-byte figure-ID entry. */
    void openManualIdInput();

    /**
     * @brief Start emulation immediately or push LockOnScreen when a payload must be selected first.
     * @param persistent Whether generated/changed state should be persisted.
     * @param fresh Whether a fresh Amiibo instance should be generated instead of loading a saved one.
     * @return true when the request is accepted.
     */
    bool requestEmulation(bool persistent, bool fresh);

    /**
     * @brief Start NFC emulation and push an EmulationScreen that owns the active session.
     * @param persistent Whether mutable emulation state should be saved.
     * @param fresh Whether to generate a fresh image rather than load the saved image.
     * @param replace_top Whether the current Lock-On screen should be replaced after this handler returns.
     * @return true when the listener starts and the screen is pushed or queued for replacement.
     */
    bool beginEmulation(bool persistent, bool fresh, bool replace_top = false);

    /**
     * @brief Prepare NFC data and start a session before its EmulationScreen is pushed.
     * @param persistent Whether mutable state should be persisted.
     * @param fresh Whether to generate a fresh image rather than load a saved image.
     * @return true when the NFC listener starts successfully.
     */
    bool startEmulationSession(bool persistent, bool fresh);

    /** @brief Stop active emulation and persist synchronized data when required. */
    void stopEmulation();

    /** @brief Pause emulation, re-key the current tag with a new UID, save it if needed, and restart. */
    void randomizeEmulationUid();

    /**
     * @brief Start asynchronous database validation/rebuild and push a modal WorkingScreen.
     * @param force Whether to rebuild even when the existing index appears current.
     * @return true when a worker thread was started.
     */
    bool startDatabasePrepare(bool force);

    /**
     * @brief Defer a launch-like key reload and forced database rebuild until the input callback returns.
     * @details The refresh destroys every non-root screen before allocating the database worker.
     */
    void refreshStatus();

private:
    /** @brief Purpose of the shared native TextInput view. */
    enum class TextMode : uint8_t {
        None, /**< No text operation is pending. */
        Search, /**< The next text result updates library search. */
        Rename, /**< The next text result renames the current saved figure. */
    };

    /** @brief Dispatcher ID for the custom stack-rendering view. */
    static constexpr uint32_t ViewMain = 0;

    /** @brief Dispatcher ID for the native TextInput view. */
    static constexpr uint32_t ViewTextInput = 1;

    /** @brief Dispatcher ID for the native ByteInput view. */
    static constexpr uint32_t ViewByteInput = 2;

    AmiiboZeroApp& app_; /**< Shared non-UI application state. */
    ViewDispatcher* dispatcher_; /**< Flipper view dispatcher owned by this manager. */
    View* main_view_; /**< Custom view that renders the top Screen. */
    TextInput* text_input_; /**< Reusable native text-input module. */
    ByteInput* byte_input_; /**< Reusable native byte-input module. */
    FuriString* scratch_; /**< Reusable string for fit/marquee drawing helpers. */

    Screen* stack_[MaxScreens]; /**< Owning fixed-capacity custom screen stack. */
    size_t depth_; /**< Number of valid entries in stack_. */

    char toast_[64]; /**< Current short-lived header message. */
    uint8_t toast_ticks_; /**< Remaining UI ticks for the current toast. */
    uint8_t animation_; /**< Animation/marquee tick advanced by the dispatcher timer. */

    TextMode text_mode_; /**< Operation currently using the shared TextInput view. */
    char text_buffer_[AZ_NAME_MAX]; /**< Shared native text-input result buffer. */
    uint8_t manual_id_[8]; /**< Shared native byte-input result buffer. */

    uint8_t pending_pop_count_; /**< Number of deferred pops requested by the current callback. */
    Screen* pending_push_; /**< Screen to push after deferred pops, or nullptr. */
    bool pending_status_refresh_; /**< Whether a forced database refresh is deferred until the callback returns. */
    bool database_refresh_requested_; /**< Whether the dispatcher stopped to rebuild from a fresh UI session. */

    /** @brief Draw the top screen into the custom view canvas. */
    void draw(Canvas* canvas);

    /** @brief Dispatch an input event to Back handling or the top screen. */
    bool handleInput(const InputEvent& event);

    /** @brief Execute the top screen's virtual Back policy. */
    void handleBack();

    /** @brief Advance UI timers, screen ticks, and database-worker completion handling. */
    void tick();

    /** @brief Apply navigation or refresh work deferred by the completed input callback. */
    void applyPendingNavigation();

    /** @brief Destroy a removed screen after its pop hook releases external resources. */
    void destroyScreen(Screen* screen);

    /** @brief Request a full UI-session restart after the initiating callback has returned. */
    void performStatusRefresh();

    /** @brief Delete and clear any outstanding deferred navigation request. */
    void clearPendingNavigation();

    /** @brief Return the dispatcher to the custom stack-rendering view. */
    void switchToMainView();

    /** @brief Apply the shared TextInput result as a library search operation. */
    void completeSearch();

    /** @brief Apply the shared TextInput result as a saved-figure rename operation. */
    void completeRename();

    /** @brief Apply the shared ByteInput result as a manual figure-ID lookup. */
    void completeManualId();

    /** @brief Extract and store the saved filename portion of a generated path. */
    void setSavedFilenameFromPath(const char* path);

    /** @brief Ensure the NFC controller and reusable NFC device are allocated. */
    bool ensureNfcResources();

    /** @brief Release NFC-only heap while the database worker is rebuilding the index. */
    void releaseNfcResourcesForDatabase();

    /** @brief Flipper custom-view draw callback. */
    static void drawCallback(Canvas* canvas, void* model);

    /** @brief Flipper custom-view input callback. */
    static bool inputCallback(InputEvent* event, void* context);

    /** @brief Flipper dispatcher Back callback used by native modal controls. */
    static bool navigationCallback(void* context);

    /** @brief Flipper dispatcher periodic-tick callback. */
    static void tickCallback(void* context);

    /** @brief Completion callback for the shared TextInput module. */
    static void textInputDone(void* context);

    /** @brief Completion callback for the shared ByteInput module. */
    static void manualIdDone(void* context);

    /** @brief Database worker progress callback that updates lock-free scalar state. */
    static void databaseProgress(void* context, AzDbProgressStage stage, uint8_t percent);

    /** @brief Background thread entry point for database validation/rebuild. */
    static int32_t databaseWorker(void* context);
};
