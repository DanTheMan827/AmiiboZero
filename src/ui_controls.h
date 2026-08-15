/**
 * @file ui_controls.h
 * @brief Reusable drawing controls shared by application screens.
 */

#pragma once

#include "amiibo_zero.h"

#include <gui/canvas.h>
#include <stddef.h>
#include <stdint.h>

class UiManager;

/**
 * @brief Stateless drawing primitives shared by concrete Screen implementations.
 * @details Controls render directly to the supplied Flipper Canvas. They do not own navigation or
 * screen state; selection, scrolling, and animation state remain owned by the calling screen or
 * UiManager.
 */
class UiControls {
public:
    /** @brief First vertical pixel available to list/detail content below the header. */
    static constexpr int ContentTop = 12;

    /** @brief First vertical pixel reserved for footer hints. */
    static constexpr int FooterTop = 54;

    /** @brief Maximum width used by standard list text. */
    static constexpr size_t ListWidth = 116;

    /** @brief Number of wrapped detail lines visible above the footer. */
    static constexpr uint8_t DetailLines = 4;

    /** @brief Vertical distance in pixels between wrapped detail lines. */
    static constexpr int DetailLineHeight = 9;

    /**
     * @brief Draw the standard inverted application header.
     * @param canvas Destination canvas.
     * @param ui UI manager providing toast, scratch-string, and animation state.
     * @param title Screen title shown when no toast is active.
     */
    static void header(Canvas* canvas, UiManager& ui, const char* title);

    /**
     * @brief Draw left, center, and right footer hints.
     * @param canvas Destination canvas.
     * @param left Left-aligned hint, or an empty string.
     * @param center Centered hint, or an empty string.
     * @param right Right-aligned hint, or an empty string.
     */
    static void footer(Canvas* canvas, const char* left, const char* center, const char* right);

    /**
     * @brief Draw text shortened to fit a fixed width.
     * @param canvas Destination canvas.
     * @param ui UI manager providing the reusable scratch string.
     * @param x Left coordinate.
     * @param y Text baseline.
     * @param text Source text.
     * @param width Maximum width in pixels.
     */
    static void fitted(Canvas* canvas, UiManager& ui, int x, int y, const char* text, size_t width);

    /**
     * @brief Draw a horizontally scrolling text line.
     * @param canvas Destination canvas.
     * @param ui UI manager providing scratch and animation state.
     * @param x Left coordinate.
     * @param y Text baseline.
     * @param text Source text.
     * @param width Available width in pixels.
     * @param ellipsis Whether the Flipper helper should render an ellipsis when appropriate.
     */
    static void marquee(
        Canvas* canvas,
        UiManager& ui,
        int x,
        int y,
        const char* text,
        size_t width,
        bool ellipsis = false);

    /**
     * @brief Draw one standard list row.
     * @param canvas Destination canvas.
     * @param ui UI manager providing text-animation state.
     * @param row Zero-based visible row index.
     * @param text Row label.
     * @param selected Whether the row is currently selected.
     */
    static void listRow(
        Canvas* canvas,
        UiManager& ui,
        uint8_t row,
        const char* text,
        bool selected);

    /**
     * @brief Draw the list scrollbar when the collection exceeds the visible row count.
     * @param canvas Destination canvas.
     * @param selection Zero-based selected item index.
     * @param total Total number of list items.
     */
    static void listScrollbar(Canvas* canvas, uint16_t selection, uint16_t total);

    /**
     * @brief Wrap and draw a scrollable block of detail text.
     * @param canvas Destination canvas.
     * @param text Source text, including optional newline separators.
     * @param top Baseline of the first visible line.
     * @param visible_lines Maximum lines to draw.
     * @param width Maximum text width in pixels.
     * @param scroll In/out first wrapped line index; clamped to the available range.
     */
    static void wrappedText(
        Canvas* canvas,
        const char* text,
        int top,
        uint8_t visible_lines,
        size_t width,
        uint16_t& scroll);
};
