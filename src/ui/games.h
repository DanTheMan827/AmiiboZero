/** @file games.h @brief AzGamesScreen declaration. */
#pragma once

#include "screen.h"

class AzGamesScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
    void onPopped(AmiiboZeroApp* app) const override;
};

extern const AzGamesScreen az_ui_games_screen;
