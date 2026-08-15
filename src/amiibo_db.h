/**
 * @file amiibo_db.h
 * @brief Indexed Amiibo metadata and game-compatibility database operations.
 */

#pragma once

#include "amiibo_zero.h"

/**
 * @brief Ensure the binary metadata index exists and matches the current JSON sources.
 * @param storage Storage service used for file operations.
 * @param force Whether to rebuild even when the existing index is current.
 * @param out_count Destination for a resulting record count.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_ensure_index(
    Storage* storage,
    bool force,
    uint32_t* out_count,
    AzDbProgressCallback progress_callback,
    void* progress_context);

/**
 * @brief Remove the active index and every index-build temporary/backup file.
 * @param storage Storage service used for file operations.
 */
void az_db_remove_index_files(Storage* storage);

/**
 * @brief Read the visible category window surrounding a selection.
 * @param storage Storage service used for file operations.
 * @param selection Selected result ordinal.
 * @param out_rows Destination array for visible rows.
 * @param out_row_count Destination for the number of populated output rows.
 * @param out_window_start Destination for the absolute ordinal of the first returned row.
 * @param out_total Destination for the total number of matching records.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_category_window(
    Storage* storage,
    uint16_t selection,
    AzCategory out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total);

/**
 * @brief Read one category by index ordinal.
 * @param storage Storage service used for file operations.
 * @param category_index Category ordinal in the index.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_category(Storage* storage, uint16_t category_index, AzCategory* out);

/**
 * @brief Read the visible figure window for a category and selection.
 * @param storage Storage service used for file operations.
 * @param category Category identifier.
 * @param selection Selected result ordinal.
 * @param out_rows Destination array for visible rows.
 * @param out_row_count Destination for the number of populated output rows.
 * @param out_window_start Destination for the absolute ordinal of the first returned row.
 * @param out_total Destination for the total number of matching records.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_figure_window(
    Storage* storage,
    uint8_t category,
    uint16_t selection,
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total);

/**
 * @brief Read one figure by category-relative ordinal.
 * @param storage Storage service used for file operations.
 * @param category Category identifier.
 * @param figure_index Figure ordinal within the category.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_figure(Storage* storage, uint8_t category, uint16_t figure_index, AzFigure* out);

/**
 * @brief Read the visible window of figures matching a search query.
 * @param storage Storage service used for file operations.
 * @param query Case-insensitive search query.
 * @param selection Selected result ordinal.
 * @param out_rows Destination array for visible rows.
 * @param out_row_count Destination for the number of populated output rows.
 * @param out_window_start Destination for the absolute ordinal of the first returned row.
 * @param out_total Destination for the total number of matching records.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_search_window(
    Storage* storage,
    const char* query,
    uint16_t selection,
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total);

/**
 * @brief Read one search result by match ordinal.
 * @param storage Storage service used for file operations.
 * @param query Case-insensitive search query.
 * @param match_index Matching-result ordinal.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_search_get(Storage* storage, const char* query, uint16_t match_index, AzFigure* out);

/**
 * @brief Find a figure whose identifier exactly matches the requested ID.
 * @param storage Storage service used for file operations.
 * @param id Eight-byte Amiibo identifier.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_find_by_id(Storage* storage, const uint8_t id[8], AzFigure* out);

/**
 * @brief Load game compatibility records matching an Amiibo identifier.
 * @param storage Storage service used for file operations.
 * @param id Eight-byte Amiibo identifier.
 * @param out_games Destination array for game compatibility records.
 * @param max_games Capacity of the output game array.
 * @param out_count Destination for a resulting record count.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_load_games(
    Storage* storage,
    const uint8_t id[8],
    AzGame* out_games,
    uint16_t max_games,
    uint16_t* out_count);
