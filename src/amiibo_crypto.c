/**
 * @file amiibo_crypto.c
 * @brief Amiibo dump cryptography and UID generation.
 * @details Loads retail key material, derives per-tag keys, encrypts or decrypts dumps, and generates compatible identities.
 */

#include "amiibo_crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <furi_hal_random.h>
#include <string.h>

/** @brief Per-tag AES and HMAC material derived from a retail key. */
typedef struct {
    uint8_t aes_key[16]; /**< AES-128 key used for dump encryption or decryption. */
    uint8_t aes_iv[16]; /**< Initial counter block used by AES-CTR. */
    uint8_t hmac_key[16]; /**< HMAC key used to authenticate tag data. */
} AzDerivedKey;

/**
 * @brief Compute an HMAC-SHA-256 digest.
 * @param key Cryptographic key bytes.
 * @param key_len Length of the key in bytes.
 * @param data Data buffer or tag state used by the operation.
 * @param data_len Length of the data buffer in bytes.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[32]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(!info) return false;
    return mbedtls_md_hmac(info, key, key_len, data, data_len, out) == 0;
}

/**
 * @brief Validate the variable-length metadata encoded in an 80-byte retail key.
 * @param key Cryptographic key bytes.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_tag_key_valid(const uint8_t key[80]) {
    return key && key[31] <= 16;
}

/**
 * @brief Derive per-tag AES, IV, and HMAC material from a retail key and tag state.
 * @param key Cryptographic key bytes.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 * @param write_counter Two-byte Amiibo write counter.
 * @param salt Per-tag salt bytes.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_derive_key(
    const uint8_t key[80],
    const uint8_t raw_uid[9],
    const uint8_t write_counter[2],
    const uint8_t salt[32],
    AzDerivedKey* out) {
    if(!az_tag_key_valid(key) || !raw_uid || !write_counter || !salt || !out) return false;

    uint8_t seed[80];
    size_t seed_len = 0;
    memcpy(seed + seed_len, key + 16, 14);
    seed_len += 14;

    const uint8_t magic_size = key[31];
    if(magic_size < 16) {
        memcpy(seed + seed_len, write_counter, 2);
        seed_len += 2;
    }

    memcpy(seed + seed_len, key + 32, magic_size);
    seed_len += magic_size;
    memcpy(seed + seed_len, raw_uid, 8);
    seed_len += 8;
    memcpy(seed + seed_len, raw_uid, 8);
    seed_len += 8;
    for(size_t i = 0; i < 32; i++) seed[seed_len++] = salt[i] ^ key[48 + i];

    uint8_t input[82];
    uint8_t h0[32];
    uint8_t h1[32];
    input[0] = 0;
    input[1] = 0;
    memcpy(input + 2, seed, seed_len);
    if(!az_hmac_sha256(key, 16, input, seed_len + 2, h0)) return false;
    input[1] = 1;
    if(!az_hmac_sha256(key, 16, input, seed_len + 2, h1)) return false;

    memcpy(out->aes_key, h0, 16);
    memcpy(out->aes_iv, h0 + 16, 16);
    memcpy(out->hmac_key, h1, 16);
    return true;
}

/**
 * @brief Transform a buffer with AES-128 in CTR mode.
 * @param key Cryptographic key bytes.
 * @param iv AES counter initialization value.
 * @param input Input buffer.
 * @param output Output buffer.
 * @param length Number of bytes to process.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_aes_ctr(
    const uint8_t key[16],
    const uint8_t iv[16],
    const uint8_t* input,
    uint8_t* output,
    size_t length) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if(mbedtls_aes_setkey_enc(&aes, key, 128) != 0) {
        mbedtls_aes_free(&aes);
        return false;
    }

    size_t nc_off = 0;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16] = {0};
    memcpy(nonce_counter, iv, sizeof(nonce_counter));
    int rc = mbedtls_aes_crypt_ctr(
        &aes,
        length,
        &nc_off,
        nonce_counter,
        stream_block,
        input,
        output);
    mbedtls_aes_free(&aes);
    return rc == 0;
}

/**
 * @brief Generate a standard raw Amiibo UID with required manufacturer and checksum bytes.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 */
static void az_random_raw_uid(uint8_t raw_uid[9]) {
    furi_hal_random_fill_buf(raw_uid, 9);
    raw_uid[0] = 0x04;
    raw_uid[3] = raw_uid[0] ^ raw_uid[1] ^ raw_uid[2] ^ 0x88;
    raw_uid[8] = raw_uid[4] ^ raw_uid[5] ^ raw_uid[6] ^ raw_uid[7];
}

/**
 * @brief Generate a version-3 seven-byte UID with the required manufacturer prefix.
 * @param uid7 Seven-byte NFC UID buffer.
 */
static void az_random_v3_uid(uint8_t uid7[7]) {
    furi_hal_random_fill_buf(uid7, 7);
    uid7[0] = 0x04;
}

/**
 * @brief Calculate the tag HMAC over the plaintext Amiibo fields covered by the tag key.
 * @param plain Plaintext Amiibo dump bytes.
 * @param tag_key Derived key used for tag authentication.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_calculate_tag_hmac(
    const uint8_t plain[AZ_DUMP_SIZE],
    const AzDerivedKey* tag_key,
    uint8_t out[32]) {
    uint8_t tag_input[52];
    memcpy(tag_input, plain, 8);
    memcpy(tag_input + 8, plain + 84, 44);
    return az_hmac_sha256(tag_key->hmac_key, 16, tag_input, sizeof(tag_input), out);
}

/**
 * @brief Calculate the data HMAC over plaintext Amiibo data and the tag HMAC.
 * @param plain Plaintext Amiibo dump bytes.
 * @param data_key Derived key used for encrypted Amiibo data.
 * @param tag_hmac Previously calculated tag HMAC.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_calculate_data_hmac(
    const uint8_t plain[AZ_DUMP_SIZE],
    const AzDerivedKey* data_key,
    const uint8_t tag_hmac[32],
    uint8_t out[32]) {
    uint8_t input[35 + 360 + 32 + 8 + 44];
    size_t p = 0;
    memcpy(input + p, plain + 17, 35);
    p += 35;
    memcpy(input + p, plain + 160, 360);
    p += 360;
    memcpy(input + p, tag_hmac, 32);
    p += 32;
    memcpy(input + p, plain, 8);
    p += 8;
    memcpy(input + p, plain + 84, 44);
    p += 44;
    return az_hmac_sha256(data_key->hmac_key, 16, input, p, out);
}

/**
 * @brief Encrypt a plaintext Amiibo dump and populate its authentication fields.
 * @param plain Plaintext Amiibo dump bytes.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_encrypt_plain_dump(
    const uint8_t plain[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE]) {
    if(!plain || !keys || !keys->valid || !out_dump) return false;

    AzDerivedKey tag_key;
    AzDerivedKey data_key;
    if(!az_derive_key(keys->static_key, plain, plain + 17, plain + 96, &tag_key)) return false;
    if(!az_derive_key(keys->data_key, plain, plain + 17, plain + 96, &data_key)) return false;

    memcpy(out_dump, plain, AZ_DUMP_SIZE);
    uint8_t tag_hmac[32];
    uint8_t data_hmac[32];
    if(!az_calculate_tag_hmac(plain, &tag_key, tag_hmac)) return false;
    if(!az_calculate_data_hmac(plain, &data_key, tag_hmac, data_hmac)) return false;
    memcpy(out_dump + 52, tag_hmac, sizeof(tag_hmac));
    memcpy(out_dump + 128, data_hmac, sizeof(data_hmac));

    uint8_t crypt_input[392];
    uint8_t crypt_output[392];
    memcpy(crypt_input, plain + 20, 32);
    memcpy(crypt_input + 32, plain + 160, 360);
    if(!az_aes_ctr(data_key.aes_key, data_key.aes_iv, crypt_input, crypt_output, sizeof(crypt_input))) {
        return false;
    }
    memcpy(out_dump + 20, crypt_output, 32);
    memcpy(out_dump + 160, crypt_output + 32, 360);
    return true;
}

/**
 * @brief Load retail Amiibo key material from persistent storage.
 * @param storage Storage service used for file operations.
 * @param keys Loaded Amiibo key material.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_keys_load(Storage* storage, AzKeys* keys) {
    if(!storage || !keys) return false;
    memset(keys, 0, sizeof(*keys));

    File* file = storage_file_alloc(storage);
    if(!file) return false;
    bool ok = storage_file_open(file, AZ_KEYS_FILE, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok && storage_file_size(file) == 160) {
        uint8_t all[160];
        ok = storage_file_read(file, all, sizeof(all)) == sizeof(all);
        if(ok) {
            memcpy(keys->data_key, all, 80);
            memcpy(keys->static_key, all + 80, 80);
            keys->valid = az_tag_key_valid(keys->data_key) && az_tag_key_valid(keys->static_key);
            ok = keys->valid;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Convert the nine-byte Amiibo UID representation into the seven-byte NFC UID.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 * @param uid7 Seven-byte NFC UID buffer.
 */
void az_raw_uid_to_nfc_uid(const uint8_t raw_uid[9], uint8_t uid7[7]) {
    uid7[0] = raw_uid[0];
    uid7[1] = raw_uid[1];
    uid7[2] = raw_uid[2];
    uid7[3] = raw_uid[4];
    uid7[4] = raw_uid[5];
    uid7[5] = raw_uid[6];
    uid7[6] = raw_uid[7];
}

/**
 * @brief Derive the NTAG password associated with a raw Amiibo UID.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 * @param password Four-byte password output buffer.
 */
void az_tag_password(const uint8_t raw_uid[9], uint8_t password[4]) {
    password[0] = 0xAA ^ (raw_uid[1] ^ raw_uid[4]);
    password[1] = 0x55 ^ (raw_uid[2] ^ raw_uid[5]);
    password[2] = 0xAA ^ (raw_uid[4] ^ raw_uid[6]);
    password[3] = 0x55 ^ (raw_uid[5] ^ raw_uid[7]);
}

/**
 * @brief Decrypt an Amiibo dump into its plaintext layout.
 * @param encrypted_dump Encrypted Amiibo dump bytes.
 * @param keys Loaded Amiibo key material.
 * @param out_plain Destination for decrypted Amiibo bytes.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_decrypt_dump(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_plain[AZ_DUMP_SIZE]) {
    if(!encrypted_dump || !keys || !keys->valid || !out_plain) return false;

    AzDerivedKey tag_key;
    AzDerivedKey data_key;
    if(!az_derive_key(keys->static_key, encrypted_dump, encrypted_dump + 17, encrypted_dump + 96, &tag_key)) {
        return false;
    }
    if(!az_derive_key(keys->data_key, encrypted_dump, encrypted_dump + 17, encrypted_dump + 96, &data_key)) {
        return false;
    }

    memcpy(out_plain, encrypted_dump, AZ_DUMP_SIZE);
    uint8_t crypt_input[392];
    uint8_t crypt_output[392];
    memcpy(crypt_input, encrypted_dump + 20, 32);
    memcpy(crypt_input + 32, encrypted_dump + 160, 360);
    if(!az_aes_ctr(data_key.aes_key, data_key.aes_iv, crypt_input, crypt_output, sizeof(crypt_input))) {
        return false;
    }
    memcpy(out_plain + 20, crypt_output, 32);
    memcpy(out_plain + 160, crypt_output + 32, 360);

    uint8_t expected_tag[32];
    uint8_t expected_data[32];
    if(!az_calculate_tag_hmac(out_plain, &tag_key, expected_tag)) return false;
    if(memcmp(expected_tag, encrypted_dump + 52, sizeof(expected_tag)) != 0) return false;
    if(!az_calculate_data_hmac(out_plain, &data_key, expected_tag, expected_data)) return false;
    if(memcmp(expected_data, encrypted_dump + 128, sizeof(expected_data)) != 0) return false;
    return true;
}

/**
 * @brief Assign a fresh standard UID to an encrypted dump and re-encrypt it.
 * @param encrypted_dump Encrypted Amiibo dump bytes.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_raw_uid Destination for the generated raw UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_rekey_dump_uid(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_raw_uid[9]) {
    if(!encrypted_dump || !keys || !out_dump || !out_raw_uid) return false;
    uint8_t plain[AZ_DUMP_SIZE];
    if(!az_decrypt_dump(encrypted_dump, keys, plain)) return false;
    az_random_raw_uid(out_raw_uid);
    memcpy(plain, out_raw_uid, 9);
    return az_encrypt_plain_dump(plain, keys, out_dump);
}

/**
 * @brief Assign a fresh version-3 UID to an encrypted dump and re-encrypt it.
 * @param encrypted_dump Encrypted Amiibo dump bytes.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_uid7 Destination for the generated seven-byte UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_rekey_v3_dump_uid(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_uid7[7]) {
    if(!encrypted_dump || !keys || !out_dump || !out_uid7) return false;
    uint8_t plain[AZ_DUMP_SIZE];
    if(!az_decrypt_dump(encrypted_dump, keys, plain)) return false;

    az_random_v3_uid(out_uid7);
    memcpy(plain, out_uid7, 7);
    plain[7] = 0x00;
    plain[8] = 0x44;
    plain[9] = 0x00;
    plain[10] = 0x0F;
    plain[11] = 0xE0;
    plain[12] = 0xF1;
    plain[13] = 0x10;
    plain[14] = 0xFF;
    plain[15] = 0xEE;
    plain[16] = 0xA5;
    plain[17] = 0x00;
    plain[18] = 0x00;
    plain[19] = 0x00;
    return az_encrypt_plain_dump(plain, keys, out_dump);
}

/**
 * @brief Generate an encrypted standard Amiibo dump for a figure identifier.
 * @param figure_id Eight-byte Amiibo figure identifier.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_raw_uid Destination for the generated raw UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_generate_dump(
    const uint8_t figure_id[8],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_raw_uid[9]) {
    if(!figure_id || !keys || !keys->valid || !out_dump || !out_raw_uid) return false;

    static const uint8_t prefix[20] = {
        0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48,
        0x0F, 0xE0, 0xF1, 0x10, 0xFF, 0xEE, 0xA5, 0x00, 0x00, 0x00,
    };
    static const uint8_t suffix[12] = {
        0x01, 0x00, 0x0F, 0xBD,
        0x00, 0x00, 0x00, 0x04,
        0x5F, 0x00, 0x00, 0x00,
    };

    uint8_t plain[AZ_DUMP_SIZE];
    memset(plain, 0, sizeof(plain));
    memcpy(plain, prefix, sizeof(prefix));
    az_random_raw_uid(out_raw_uid);
    memcpy(plain, out_raw_uid, 9);
    memcpy(plain + 84, figure_id, 8);
    furi_hal_random_fill_buf(plain + 96, 32);
    memcpy(plain + 520, suffix, sizeof(suffix));
    return az_encrypt_plain_dump(plain, keys, out_dump);
}

/**
 * @brief Generate an encrypted version-3 Amiibo dump and seven-byte UID.
 * @param figure_id Eight-byte Amiibo figure identifier.
 * @param keys Loaded Amiibo key material.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @param out_uid7 Destination for the generated seven-byte UID.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_generate_v3_dump(
    const uint8_t figure_id[8],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_uid7[7]) {
    if(!figure_id || !keys || !keys->valid || !out_dump || !out_uid7) return false;

    uint8_t plain[AZ_DUMP_SIZE];
    memset(plain, 0, sizeof(plain));
    az_random_v3_uid(out_uid7);

    memcpy(plain, out_uid7, 7);
    plain[7] = 0x00;
    plain[8] = 0x44;
    plain[9] = 0x00;
    plain[10] = 0x0F;
    plain[11] = 0xE0;
    plain[12] = 0xF1;
    plain[13] = 0x10;
    plain[14] = 0xFF;
    plain[15] = 0xEE;
    plain[16] = 0xA5;
    plain[17] = 0x00;
    plain[18] = 0x00;
    plain[19] = 0x00;

    memcpy(plain + 84, figure_id, 8);
    furi_hal_random_fill_buf(plain + 96, 32);
    return az_encrypt_plain_dump(plain, keys, out_dump);
}
