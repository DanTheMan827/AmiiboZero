/** @file emulate.h @brief AzEmulateScreen declaration. */
#pragma once

#include "screen.h"

class AzEmulateScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzEmulateScreen az_ui_emulate_screen;
