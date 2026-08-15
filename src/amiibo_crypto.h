/**
 * @file amiibo_crypto.h
 * @brief Amiibo key handling, dump generation, encryption, decryption, and UID
 * helpers.
 */

#pragma once

#include "amiibo_zero.h"

/**
 * @brief Load retail Amiibo key material from persistent storage.
 * @param storage Storage service used for file operations.
 * @param keys Loaded Amiibo key material.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_keys_load(Storage *storage, AzKeys *keys);

/**
 * @brief Generate an encrypted standard Amiibo dump for a figure identifier.
 * @param figure_id Eight-byte Amiibo figure identifier.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_raw_uid Destination for the generated raw UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_generate_dump(const uint8_t figure_id[8], const AzKeys *keys,
                      uint8_t out_dump[AZ_DUMP_SIZE], uint8_t out_raw_uid[9]);

/**
 * @brief Generate an encrypted version-3 Amiibo dump and seven-byte UID.
 * @param figure_id Eight-byte Amiibo figure identifier.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_uid7 Destination for the generated seven-byte UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_generate_v3_dump(const uint8_t figure_id[8], const AzKeys *keys,
                         uint8_t out_dump[AZ_DUMP_SIZE], uint8_t out_uid7[7]);

/**
 * @brief Decrypt an Amiibo dump into its plaintext layout.
 * @param encrypted_dump Encrypted Amiibo dump bytes.
 * @param keys Loaded Amiibo key material.
 * @param out_plain Destination for decrypted Amiibo bytes.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_decrypt_dump(const uint8_t encrypted_dump[AZ_DUMP_SIZE],
                     const AzKeys *keys, uint8_t out_plain[AZ_DUMP_SIZE]);

/**
 * @brief Assign a fresh standard UID to an encrypted dump and re-encrypt it.
 * @param encrypted_dump Encrypted Amiibo dump bytes.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_raw_uid Destination for the generated raw UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_rekey_dump_uid(const uint8_t encrypted_dump[AZ_DUMP_SIZE],
                       const AzKeys *keys, uint8_t out_dump[AZ_DUMP_SIZE],
                       uint8_t out_raw_uid[9]);

/**
 * @brief Assign a fresh version-3 UID to an encrypted dump and re-encrypt it.
 * @param encrypted_dump Encrypted Amiibo dump bytes.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_uid7 Destination for the generated seven-byte UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_rekey_v3_dump_uid(const uint8_t encrypted_dump[AZ_DUMP_SIZE],
                          const AzKeys *keys, uint8_t out_dump[AZ_DUMP_SIZE],
                          uint8_t out_uid7[7]);

/**
 * @brief Convert the nine-byte Amiibo UID representation into the seven-byte
 * NFC UID.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 * @param uid7 Seven-byte NFC UID buffer.
 */
void az_raw_uid_to_nfc_uid(const uint8_t raw_uid[9], uint8_t uid7[7]);

/**
 * @brief Derive the NTAG password associated with a raw Amiibo UID.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 * @param password Four-byte password output buffer.
 */
void az_tag_password(const uint8_t raw_uid[9], uint8_t password[4]);
