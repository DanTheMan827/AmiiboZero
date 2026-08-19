/** @file advanced.h @brief AzAdvancedScreen declaration. */
#pragma once

#include "screen.h"

class AzAdvancedScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzAdvancedScreen az_ui_advanced_screen;
