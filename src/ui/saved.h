/** @file saved.h @brief AzSavedScreen declaration. */
#pragma once

#include "screen.h"

class AzSavedScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzSavedScreen az_ui_saved_screen;
