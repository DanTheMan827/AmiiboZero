/**
 * @file amiibo_crypto.c
 * @brief Amiibo key loading, authenticated decryption, UID re-keying, and dump generation.
 */

#include "./amiibo_zero.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <furi_hal_random.h>
#include <string.h>

/** Derived cryptographic material for one Amiibo data/static key block. */
typedef struct {
    uint8_t aes_key[16];  /**< AES-128 key used for CTR encryption. */
    uint8_t aes_iv[16];   /**< Initial AES-CTR counter block. */
    uint8_t hmac_key[16]; /**< Truncated KDF output used as the HMAC key. */
} AzDerivedKey;

/**
 * @brief Compute one HMAC-SHA256 digest directly on the SHA-256 primitive.
 * @details AmiiboZero only uses short 16-byte HMAC keys. Avoiding mbedtls_md keeps the generic
 *          digest dispatcher, MD5, and SHA-1 implementations out of the FAP.
 * @param key HMAC key bytes.
 * @param key_len HMAC key length; must not exceed the SHA-256 64-byte block size.
 * @param data Message bytes.
 * @param data_len Message length.
 * @param out Destination 32-byte digest.
 * @return True on success.
 */
static bool az_hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[32]) {
    if(!key || !out || (!data && data_len) || key_len > 64U) return false;

    uint8_t pad[64] = {0};
    uint8_t inner[32];
    memcpy(pad, key, key_len);
    for(size_t i = 0; i < sizeof(pad); i++) pad[i] ^= 0x36U;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, pad, sizeof(pad));
    if(data_len) mbedtls_sha256_update(&sha, data, data_len);
    mbedtls_sha256_finish(&sha, inner);

    for(size_t i = 0; i < sizeof(pad); i++) pad[i] ^= (0x36U ^ 0x5CU);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, pad, sizeof(pad));
    mbedtls_sha256_update(&sha, inner, sizeof(inner));
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
    return true;
}

/**
 * @brief Validate bounded KDF metadata embedded in one 80-byte Amiibo master-key block.
 * @param key Master-key block.
 * @return True when the variable magic-byte size is safe to consume.
 */
static bool az_tag_key_valid(const uint8_t key[80]) {
    return key && key[31] <= 16;
}

/**
 * @brief Derive AES key, AES IV, and HMAC key material for one Amiibo master-key block.
 * @param key 80-byte master-key block.
 * @param raw_uid Nine-byte physical Amiibo UID representation.
 * @param write_counter Two-byte Amiibo write counter.
 * @param salt Thirty-two-byte key-generation salt.
 * @param out Destination derived-key structure.
 * @return True when both KDF HMAC rounds succeed.
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
 * @brief Encrypt or decrypt a byte range with AES-128 CTR.
 * @param key AES-128 key.
 * @param iv Initial counter block.
 * @param input Source bytes.
 * @param output Destination bytes; may alias input.
 * @param length Number of bytes to process.
 * @return True when mbedTLS completes successfully.
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
 * @brief Generate a fresh Nintendo/NXP-style raw UID and recompute both BCC bytes.
 * @param raw_uid Destination nine-byte physical UID representation.
 */
static void az_random_raw_uid(uint8_t raw_uid[9]) {
    furi_hal_random_fill_buf(raw_uid, 9);
    raw_uid[0] = 0x04;
    raw_uid[3] = raw_uid[0] ^ raw_uid[1] ^ raw_uid[2] ^ 0x88;
    raw_uid[8] = raw_uid[4] ^ raw_uid[5] ^ raw_uid[6] ^ raw_uid[7];
}

/**
 * @brief Generate a fresh seven-byte UID for a type-3 tag.
 * @param uid7 Destination direct RF UID.
 */
static void az_random_v3_uid(uint8_t uid7[7]) {
    furi_hal_random_fill_buf(uid7, 7);
    uid7[0] = 0x04;
}

/**
 * @brief Calculate tag HMAC over the UID/model/salt portion of a plaintext-layout dump.
 * @param plain Plaintext-layout 532-byte dump.
 * @param tag_key Derived static/tag key.
 * @param out Destination digest.
 * @return True when HMAC generation succeeds.
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
 * @brief Calculate data HMAC over all authenticated plaintext sections and tag HMAC.
 * @param plain Plaintext-layout 532-byte dump.
 * @param data_key Derived data key.
 * @param tag_hmac Tag HMAC to include in the data digest.
 * @param out Destination digest.
 * @return True when HMAC generation succeeds.
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
 * @brief Sign and encrypt a plaintext-layout standard Amiibo dump.
 * @param plain Source plaintext-layout dump with raw UID, model ID, counter, and salt already set.
 * @param keys Valid retail keys.
 * @param out_dump Destination encrypted dump.
 * @return True when KDF, HMAC, and AES operations all succeed.
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
 * @brief Load and validate the user-supplied 160-byte key_retail.bin file.
 * @param storage Open Storage service.
 * @param keys Destination key structure.
 * @return True only for a complete, structurally valid key file.
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
 * @brief Convert Amiibo raw UID layout into the seven RF UID bytes used by Ultralight tags.
 * @param raw_uid Nine-byte physical UID representation including BCC bytes.
 * @param uid7 Destination seven RF UID bytes.
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
 * @brief Derive the NTAG password associated with an Amiibo raw UID.
 * @param raw_uid Nine-byte physical UID representation.
 * @param password Destination four-byte NTAG password.
 */
void az_tag_password(const uint8_t raw_uid[9], uint8_t password[4]) {
    password[0] = 0xAA ^ (raw_uid[1] ^ raw_uid[4]);
    password[1] = 0x55 ^ (raw_uid[2] ^ raw_uid[5]);
    password[2] = 0xAA ^ (raw_uid[4] ^ raw_uid[6]);
    password[3] = 0x55 ^ (raw_uid[5] ^ raw_uid[7]);
}

/**
 * @brief Authenticate and decrypt one standard encrypted Amiibo dump.
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
 * @brief Re-sign an authenticated dump around a newly generated UID while preserving user/game data.
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
 * @brief Re-sign an authenticated dump around an exact physical NTAG215 UID.
 */
bool az_rekey_dump_uid_to(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    const uint8_t raw_uid[9],
    uint8_t out_dump[AZ_DUMP_SIZE]) {
    if(!encrypted_dump || !keys || !raw_uid || !out_dump) return false;
    uint8_t plain[AZ_DUMP_SIZE];
    if(!az_decrypt_dump(encrypted_dump, keys, plain)) return false;
    memcpy(plain, raw_uid, 9U);
    return az_encrypt_plain_dump(plain, keys, out_dump);
}

/**
 * @brief Reset the writable Amiibo user-state plaintext and re-encrypt it.
 *
 * A retail locked Amiibo keeps the model identity and key-generation salt in locked pages.
 * The writable state consists of the write/settings area (bytes 17-51) and the 360-byte
 * application/user area (bytes 160-519).  The data HMAC is regenerated by the normal
 * encryption path; the tag HMAC remains consistent because UID/model/salt are preserved.
 */
bool az_clear_user_data_dump(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE]) {
    if(!encrypted_dump || !keys || !out_dump) return false;
    uint8_t plain[AZ_DUMP_SIZE];
    if(!az_decrypt_dump(encrypted_dump, keys, plain)) return false;

    memset(plain + 17U, 0, 35U);
    memset(plain + 160U, 0, 360U);
    return az_encrypt_plain_dump(plain, keys, out_dump);
}

/**
 * @brief Re-sign one type-3 crypto image around a fresh direct seven-byte UID.
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
 * @brief Generate, authenticate, and encrypt a fresh standard Amiibo dump.
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
 * @brief Generate, authenticate, and encrypt a fresh type-3 Amiibo crypto image.
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

    /* Real v3 tags expose the seven UID bytes directly in bytes 0..6 rather than the
     * NTAG215 BCC-interleaved layout. Byte 7 is RFU and page 2 begins 44 00 0F E0. */
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
