/** @file working.h @brief AzWorkingScreen declaration. */
#pragma once

#include "screen.h"

class AzWorkingScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
    bool backRequested(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzWorkingScreen az_ui_working_screen;
