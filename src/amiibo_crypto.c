/**
 * @file amiibo_crypto.c
 * @brief Amiibo key loading, key derivation, encryption, HMAC generation, and dump creation.
 */

#include "./amiibo_zero.h"

#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <furi_hal_random.h>
#include <string.h>

#define AZ_DUMP_SIZE 532

/**
 * @brief Compute one HMAC-SHA256 digest with mbedTLS.
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

/** Derived cryptographic material for one Amiibo data/static key block. */
typedef struct {
    uint8_t aes_key[16];  /**< AES-128 key used for CTR encryption. */
    uint8_t aes_iv[16];   /**< Initial AES-CTR counter block. */
    uint8_t hmac_key[16]; /**< Truncated KDF output used as the HMAC key. */
} AzDerivedKey;

/**
 * @brief Validate the bounded KDF metadata embedded in an 80-byte Amiibo key block.
 */
static bool az_tag_key_valid(const uint8_t key[80]) {
    return key[31] <= 16;
}

/**
 * @brief Derive AES key, AES IV, and HMAC key material for one Amiibo key block.
 */
static bool az_derive_key(
    const uint8_t key[80],
    const uint8_t raw_uid[9],
    const uint8_t write_counter[2],
    const uint8_t salt[32],
    AzDerivedKey* out) {
    if(!az_tag_key_valid(key) || !out) return false;

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
 * @brief Load and validate the user-supplied 160-byte key_retail.bin file.
 */
bool az_keys_load(Storage* storage, AzKeys* keys) {
    if(!storage || !keys) return false;
    memset(keys, 0, sizeof(*keys));

    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, AZ_KEYS_FILE, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok && storage_file_size(file) == 160) {
        uint8_t all[160];
        ok = storage_file_read(file, all, sizeof(all)) == sizeof(all);
        if(ok) {
            /* AmiiTag key_retail.bin layout: data key first, static/tag key second. */
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
 * @brief Convert Amiibo raw UID layout into the seven RF UID bytes used by NTAG215.
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
 */
void az_tag_password(const uint8_t raw_uid[9], uint8_t password[4]) {
    password[0] = 0xAA ^ (raw_uid[1] ^ raw_uid[4]);
    password[1] = 0x55 ^ (raw_uid[2] ^ raw_uid[5]);
    password[2] = 0xAA ^ (raw_uid[4] ^ raw_uid[6]);
    password[3] = 0x55 ^ (raw_uid[5] ^ raw_uid[7]);
}

/**
 * @brief Generate a fresh Nintendo-compatible raw UID with recomputed BCC bytes.
 */
static void az_random_raw_uid(uint8_t raw_uid[9]) {
    furi_hal_random_fill_buf(raw_uid, 9);
    raw_uid[0] = 0x04;
    raw_uid[3] = raw_uid[0] ^ raw_uid[1] ^ raw_uid[2] ^ 0x88;
    raw_uid[8] = raw_uid[4] ^ raw_uid[5] ^ raw_uid[6] ^ raw_uid[7];
}

/**
 * @brief Generate, authenticate, and encrypt a fresh 532-byte Amiibo dump.
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

    memset(out_dump, 0, AZ_DUMP_SIZE);
    memcpy(out_dump, prefix, sizeof(prefix));
    az_random_raw_uid(out_raw_uid);
    memcpy(out_dump, out_raw_uid, 9);
    memcpy(out_dump + 84, figure_id, 8);
    furi_hal_random_fill_buf(out_dump + 96, 32);
    memcpy(out_dump + 520, suffix, sizeof(suffix));

    const uint8_t* write_counter = out_dump + 17;
    const uint8_t* salt = out_dump + 96;
    AzDerivedKey tag_key;
    AzDerivedKey data_key;
    if(!az_derive_key(keys->static_key, out_raw_uid, write_counter, salt, &tag_key)) return false;
    if(!az_derive_key(keys->data_key, out_raw_uid, write_counter, salt, &data_key)) return false;

    uint8_t tag_input[52];
    memcpy(tag_input, out_dump, 8);
    memcpy(tag_input + 8, out_dump + 84, 44);
    if(!az_hmac_sha256(tag_key.hmac_key, 16, tag_input, sizeof(tag_input), out_dump + 52)) {
        return false;
    }

    uint8_t data_input[35 + 360 + 32 + 8 + 44];
    size_t p = 0;
    memcpy(data_input + p, out_dump + 17, 35);
    p += 35;
    memcpy(data_input + p, out_dump + 160, 360);
    p += 360;
    memcpy(data_input + p, out_dump + 52, 32);
    p += 32;
    memcpy(data_input + p, out_dump, 8);
    p += 8;
    memcpy(data_input + p, out_dump + 84, 44);
    p += 44;
    if(!az_hmac_sha256(data_key.hmac_key, 16, data_input, p, out_dump + 128)) return false;

    uint8_t plain[392];
    uint8_t encrypted[392];
    memcpy(plain, out_dump + 20, 32);
    memcpy(plain + 32, out_dump + 160, 360);
    if(!az_aes_ctr(data_key.aes_key, data_key.aes_iv, plain, encrypted, sizeof(plain))) {
        return false;
    }
    memcpy(out_dump + 20, encrypted, 32);
    memcpy(out_dump + 160, encrypted + 32, 360);
    return true;
}
