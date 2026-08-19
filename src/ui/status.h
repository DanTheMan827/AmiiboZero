/** @file status.h @brief AzStatusScreen declaration. */
#pragma once

#include "screen.h"

class AzStatusScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzStatusScreen az_ui_status_screen;
