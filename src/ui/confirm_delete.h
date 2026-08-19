/** @file confirm_delete.h @brief AzConfirmDeleteScreen declaration. */
#pragma once

#include "screen.h"

class AzConfirmDeleteScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzConfirmDeleteScreen az_ui_confirm_delete_screen;
