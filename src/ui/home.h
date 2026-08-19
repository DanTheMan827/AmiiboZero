/** @file home.h @brief AzHomeScreen declaration. */
#pragma once

#include "screen.h"

class AzHomeScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzHomeScreen az_ui_home_screen;
