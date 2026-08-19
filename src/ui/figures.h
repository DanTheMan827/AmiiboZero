/** @file figures.h @brief AzFiguresScreen declaration. */
#pragma once

#include "screen.h"

class AzFiguresScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzFiguresScreen az_ui_figures_screen;
