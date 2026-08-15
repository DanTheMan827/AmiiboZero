/**
 * @file ui_screen.h
 * @brief Base class and navigation contracts for stack-managed UI screens.
 */

#pragma once

#include "../amiibo_zero.h"

#include <gui/canvas.h>
#include <input/input.h>
#include <stdint.h>

class UiManager;

/** @brief Stable identities for the custom screens managed by UiManager. */
enum class ScreenId : uint8_t {
    Home, /**< Root application menu. */
    Categories, /**< Amiibo-series/category browser. */
    Figures, /**< Figures within the selected category. */
    SearchResults, /**< Figure search results. */
    Saved, /**< Saved NFC figure catalog. */
    Advanced, /**< Advanced-tool menu. */
    LockOn, /**< Version-3 Lock-On payload picker. */
    Figure, /**< Actions for the current figure. */
    DumpInfo, /**< Decoded saved-dump details. */
    Games, /**< Game compatibility details. */
    Emulation, /**< Active NFC emulation status. */
    Status, /**< Setup and database/key status. */
    About, /**< Application information. */
    ConfirmDelete, /**< Saved-figure deletion confirmation. */
    Working, /**< Modal background-database-work status. */
};

/**
 * @brief Result of a screen's Back hook.
 * @details UiManager applies the result only after the virtual callback returns, so a screen is
 * never destroyed while one of its member functions is still executing.
 */
enum class BackAction : uint8_t {
    Pop, /**< Remove the current screen and reveal the previous stack item. */
    Cancel, /**< Keep the current screen on top of the stack. */
    Exit, /**< Stop the UI dispatcher and exit the application. */
};

/** @brief Deferred operation performed after a Lock-On payload is selected. */
enum class LockOnAction : uint8_t {
    GenerateTemporary, /**< Generate a temporary version-3 figure and emulate it. */
    GeneratePersistent, /**< Generate, save, and emulate a version-3 figure. */
    EmulateSaved, /**< Attach a payload to the current saved figure for emulation. */
    ReplaceSaved, /**< Replace the saved figure's Lock-On sidecar payload. */
};

/**
 * @brief Polymorphic base for every custom application screen.
 * @details Derived classes own their local selection/scroll/resource state and override draw(),
 * handleInput(), and whichever lifecycle hooks they require. The default Back policy is to pop the
 * screen. Returning BackAction::Cancel from onBack() vetoes that navigation request.
 */
class Screen {
public:
    /**
     * @brief Construct a screen bound to a UI manager.
     * @param ui Manager that owns navigation and shared UI services.
     */
    explicit Screen(UiManager& ui);

    /** @brief Virtual destructor allowing UiManager to destroy derived screens through Screen*. */
    virtual ~Screen() = default;

    /** @brief Screens are uniquely owned by UiManager and cannot be copied. */
    Screen(const Screen&) = delete;

    /** @brief Screens are uniquely owned by UiManager and cannot be assigned. */
    Screen& operator=(const Screen&) = delete;

    /** @brief Return the concrete screen identity. */
    virtual ScreenId id() const = 0;

    /**
     * @brief Draw the complete screen.
     * @param canvas Destination Flipper canvas.
     */
    virtual void draw(Canvas* canvas) = 0;

    /**
     * @brief Handle a non-Back input event delivered while this screen is on top.
     * @param event Input event to process.
     * @return true when the event is consumed.
     */
    virtual bool handleInput(const InputEvent& event);

    /**
     * @brief Decide what should happen when Back is pressed.
     * @return BackAction::Pop by default. Derived screens may return Cancel or Exit and may perform
     * cleanup before returning. Hooks that want alternate navigation should schedule it through
     * UiManager instead of synchronously removing themselves.
     */
    virtual BackAction onBack();

    /**
     * @brief Called after the screen is inserted as the top stack item.
     * @return true to keep the screen; false to reject the push and destroy the instance.
     */
    virtual bool onPushed();

    /** @brief Called when a child screen is popped and this screen becomes visible again. */
    virtual void onRevealed();

    /**
     * @brief Called immediately before UiManager destroys this removed screen.
     * @details Release non-object resources here. Normal C++ members and owned allocations should be
     * released by the destructor.
     */
    virtual void onPopped();

    /** @brief Called from the UI timer while this screen is on top. */
    virtual void onTick();

protected:
    UiManager& ui_; /**< Owning navigation manager and shared UI service provider. */
    uint16_t selection_; /**< Screen-local selected item index. */
    uint16_t scroll_; /**< Screen-local wrapped-text/list scroll position. */

    /**
     * @brief Move a bounded selection and reset marquee animation when it changes.
     * @param direction Negative to move up, positive to move down.
     * @param count Number of selectable items.
     */
    void moveSelection(int direction, uint16_t count);

    /**
     * @brief Return whether an input event is a short OK press.
     * @param event Event to test.
     * @return true for InputTypeShort/InputKeyOk.
     */
    static bool isShortOk(const InputEvent& event);
};
