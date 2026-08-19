/** @file screen.h @brief Base interface and lifecycle contract for AmiiboZero C++ screens. */
#pragma once

#include "../amiibo_zero.h"
#include <gui/canvas.h>
#include <input/input.h>

class AzUiScreen {
public:
    /** Render this screen from the current immutable view-model snapshot. */
    virtual void draw(Canvas* canvas, const AzViewModel* model) const = 0;

    /** Handle non-navigation input. Back is handled centrally by the UI stack. */
    virtual bool input(AmiiboZeroApp* app, const InputEvent* event) const = 0;

    /**
     * Inspect a Back request before the framework pops this screen.
     * @return True to accept the default pop, false to keep this screen on top.
     */
    virtual bool backRequested(AmiiboZeroApp* app, const InputEvent* event) const {
        (void)app;
        (void)event;
        return true;
    }

    /** Called whenever this stack entry becomes the visible screen. */
    virtual void onResume(AmiiboZeroApp* app) const {
        (void)app;
    }

    /** Called exactly when this stack entry is removed or replaced. */
    virtual void onPopped(AmiiboZeroApp* app) const {
        (void)app;
    }

protected:
    /* Screens are static framework objects and are never deleted through the base type. */
    ~AzUiScreen() = default;
};
