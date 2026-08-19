/** @file search_results.h @brief AzSearchResultsScreen declaration. */
#pragma once

#include "screen.h"

class AzSearchResultsScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzSearchResultsScreen az_ui_search_results_screen;
