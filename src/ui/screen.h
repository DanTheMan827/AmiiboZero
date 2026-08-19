/** @file screen.h @brief Base interface for AmiiboZero C++ screens. */
#pragma once

#include "../amiibo_zero.h"
#include <gui/canvas.h>
#include <input/input.h>

class AzUiScreen {
public:
    virtual void draw(Canvas* canvas, const AzViewModel* model) const = 0;
    virtual bool input(AmiiboZeroApp* app, const InputEvent* event) const = 0;

protected:
    ~AzUiScreen() = default;
};
