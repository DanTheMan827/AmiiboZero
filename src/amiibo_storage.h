/**
 * @file amiibo_storage.h
 * @brief Application directory, saved-figure, and Lock-On persistence
 * operations.
 */

#pragma once

#include "amiibo_zero.h"

/**
 * @brief Create the application data directories required for persistent
 * storage.
 * @param storage Storage service used for file operations.
 */
void az_storage_init(Storage *storage);

/**
 * @brief Enumerate saved Amiibo NFC files into a sorted catalog.
 * @param storage Storage service used for file operations.
 * @param out Destination for the computed result.
 * @param max_entries Maximum supported entries.
 * @return The resulting count.
 */
uint16_t az_saved_scan(Storage *storage, AzSavedEntry *out,
                       uint16_t max_entries);

/**
 * @brief Enumerate valid Lock-On payload files into a sorted catalog.
 * @param storage Storage service used for file operations.
 * @param out Destination for the computed result.
 * @param max_entries Maximum supported entries.
 * @return The resulting count.
 */
uint16_t az_lockon_scan(Storage *storage, AzLockOnEntry *out,
                        uint16_t max_entries);

/**
 * @brief Load and validate a Lock-On SRAM payload by filename.
 * @param storage Storage service used for file operations.
 * @param filename Filename relative to the relevant application data directory.
 * @param out_sram Destination Lock-On SRAM buffer.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_lockon_load(Storage *storage, const char *filename,
                    uint8_t out_sram[AZ_LOCKON_SRAM_SIZE]);

/**
 * @brief Load the Lock-On companion payload associated with a saved figure.
 * @param storage Storage service used for file operations.
 * @param saved_filename Saved figure filename.
 * @param out_sram Destination Lock-On SRAM buffer.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_lockon_load(Storage *storage, const char *saved_filename,
                          uint8_t out_sram[AZ_LOCKON_SRAM_SIZE]);

/**
 * @brief Persist a Lock-On companion payload for a saved figure.
 * @param storage Storage service used for file operations.
 * @param saved_filename Saved figure filename.
 * @param sram Lock-On SRAM payload.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_lockon_save(Storage *storage, const char *saved_filename,
                          const uint8_t sram[AZ_LOCKON_SRAM_SIZE]);

/**
 * @brief Build an unused save path derived from figure metadata.
 * @param storage Storage service used for file operations.
 * @param figure Figure metadata.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 */
void az_make_unique_save_path(Storage *storage, const AzFigure *figure,
                              char *out, size_t out_size);

/**
 * @brief Rename a saved figure and its companion Lock-On data.
 * @param storage Storage service used for file operations.
 * @param old_filename Current saved figure filename.
 * @param requested_name User-requested replacement display name.
 * @param out_filename Destination for the final saved filename.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_rename(Storage *storage, const char *old_filename,
                     const char *requested_name, char *out_filename,
                     size_t out_size);

/**
 * @brief Delete a saved figure and any companion Lock-On data.
 * @param storage Storage service used for file operations.
 * @param filename Filename relative to the relevant application data directory.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_delete(Storage *storage, const char *filename);
