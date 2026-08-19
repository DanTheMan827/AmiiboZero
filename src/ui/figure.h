/** @file figure.h @brief AzFigureScreen declaration. */
#pragma once

#include "screen.h"

class AzFigureScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzFigureScreen az_ui_figure_screen;
