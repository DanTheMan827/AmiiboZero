/**
 * @file amiibo_nfc.h
 * @brief Amiibo NFC image construction, inspection, persistence, and emulation operations.
 */

#pragma once

#include "amiibo_zero.h"

/**
 * @brief Return a display label for an Amiibo type code.
 * @param type Type or event code to interpret.
 * @return Pointer to the selected text, or NULL when no text is available.
 */
const char* az_figure_type_name(uint8_t type);

/**
 * @brief Determine whether an Amiibo identifier uses the version-3 layout.
 * @param id Eight-byte Amiibo identifier.
 * @return true when the tested condition is satisfied; false otherwise.
 */
bool az_figure_is_v3(const uint8_t id[8]);

/**
 * @brief Populate an NFC device with a newly generated Amiibo tag image.
 * @param device NFC device to inspect or modify.
 * @param figure Figure metadata.
 * @param keys Loaded Amiibo key material.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_generate_device(NfcDevice* device, const AzFigure* figure, const AzKeys* keys);

/**
 * @brief Determine whether an NFC device contains the version-3 tag layout.
 * @param device NFC device to inspect or modify.
 * @return true when the tested condition is satisfied; false otherwise.
 */
bool az_nfc_device_is_v3(const NfcDevice* device);

/**
 * @brief Start NFC emulation for the application's current device.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_listener_start(AmiiboZeroApp* app);

/**
 * @brief Stop active emulation and synchronize mutable tag state back to the device.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_listener_pause_and_sync(AmiiboZeroApp* app);

/**
 * @brief Replace the UID of the current Amiibo image while preserving encrypted content.
 * @param device NFC device to inspect or modify.
 * @param keys Loaded Amiibo key material.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_randomize_uid(NfcDevice* device, const AzKeys* keys);

/**
 * @brief Decode user-facing Amiibo metadata from an NFC device.
 * @param device NFC device to inspect or modify.
 * @param keys Loaded Amiibo key material.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_read_details(const NfcDevice* device, const AzKeys* keys, AzAmiiboDetails* out);

/**
 * @brief Extract the Amiibo figure identifier from an NFC device.
 * @param device NFC device to inspect or modify.
 * @param out_id Destination for the resulting id.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_extract_figure_id(const NfcDevice* device, uint8_t out_id[8]);

/**
 * @brief Check whether an NFC device has a supported Amiibo memory layout.
 * @param device NFC device to inspect or modify.
 * @return true when the tested condition is satisfied; false otherwise.
 */
bool az_nfc_validate_amiibo(const NfcDevice* device);

/**
 * @brief Persist an NFC device image to a file path.
 * @param device NFC device to inspect or modify.
 * @param path Filesystem path.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_save_device(NfcDevice* device, const char* path);

/**
 * @brief Load an NFC device image from a file path.
 * @param device NFC device to inspect or modify.
 * @param path Filesystem path.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_load_device(NfcDevice* device, const char* path);
