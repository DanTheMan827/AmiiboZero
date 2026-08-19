/** @file dump_info.h @brief AzDumpInfoScreen declaration. */
#pragma once

#include "screen.h"

class AzDumpInfoScreen final : public AzUiScreen {
public:
    void draw(Canvas* canvas, const AzViewModel* model) const override;
    bool input(AmiiboZeroApp* app, const InputEvent* event) const override;
};

extern const AzDumpInfoScreen az_ui_dump_info_screen;
