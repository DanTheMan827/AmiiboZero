/** @file about.h @brief AzAboutScreen declaration. */
#pragma once

#include "screen.h"

class AzAboutScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzAboutScreen az_ui_about_screen;
