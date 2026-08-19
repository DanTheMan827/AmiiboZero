/** @file tag_operation.h @brief AzTagOperationScreen declaration. */
#pragma once

#include "screen.h"

class AzTagOperationScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
    void onPopped(AmiiboZeroApp* app) const override;
};

extern const AzTagOperationScreen az_ui_tag_operation_screen;
