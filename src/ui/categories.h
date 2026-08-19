/** @file categories.h @brief AzCategoriesScreen declaration. */
#pragma once

#include "screen.h"

class AzCategoriesScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzCategoriesScreen az_ui_categories_screen;
