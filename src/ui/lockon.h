/** @file lockon.h @brief AzLockOnScreen declaration. */
#pragma once

#include "screen.h"

class AzLockOnScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzLockOnScreen az_ui_lockon_screen;
