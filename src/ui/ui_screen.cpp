/**
 * @file ui_screen.cpp
 * @brief Default behavior shared by all stack-managed UI screens.
 */

#include "ui_screen.h"
#include "ui_manager.h"

Screen::Screen(UiManager& ui) : ui_(ui), selection_(0), scroll_(0) {
}

bool Screen::handleInput(const InputEvent&) {
    return false;
}

BackAction Screen::onBack() {
    return BackAction::Pop;
}

bool Screen::onPushed() {
    return true;
}

void Screen::onRevealed() {
}

void Screen::onPopped() {
}

void Screen::onTick() {
}

void Screen::moveSelection(int direction, uint16_t count) {
    if(count == 0) {
        selection_ = 0;
        return;
    }

    const uint16_t old = selection_;
    if(direction < 0 && selection_ > 0) {
        --selection_;
    } else if(direction > 0 && selection_ + 1U < count) {
        ++selection_;
    }

    if(old != selection_) ui_.resetAnimation();
}

bool Screen::isShortOk(const InputEvent& event) {
    return event.type == InputTypeShort && event.key == InputKeyOk;
}
