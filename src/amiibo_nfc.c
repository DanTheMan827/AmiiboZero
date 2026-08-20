/**
 * @file amiibo_nfc.c
 * @brief Standard NTAG215 and type-3 NTAG I2C Plus 2K Amiibo device handling.
 */

#include "./amiibo_zero.h"
#include "./amiibo_db.h"

#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_listener.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Byte offset where the type-3 layout inserts its 0x40-byte nonce window. */
#define AZ_V3_SHIFT_START 0x80U
/** Size of the type-3 nonce window inserted into the physical tag layout. */
#define AZ_V3_SHIFT_SIZE 0x40U

/**
 * @brief Return a display label for one model type byte.
 */
const char* az_figure_type_name(uint8_t type) {
    switch(type) {
    case 0x00: return "Figure";
    case 0x01: return "Card";
    case 0x02: return "Yarn";
    default: return "Other";
    }
}

/**
 * @brief Identify the newer type-3/lock-on Amiibo format used by pixl.js.
 */
bool az_figure_is_v3(const uint8_t id[8]) {
    return id && id[7] == 0x03;
}

/** Type-2 READ command. */
#define AZ_NFC_CMD_READ 0x30U
/** Type-2 WRITE command. */
#define AZ_NFC_CMD_WRITE 0xA2U
/** NTAG GET_VERSION command. */
#define AZ_NFC_CMD_GET_VERSION 0x60U
/** NTAG READ_SIG command. */
#define AZ_NFC_CMD_READ_SIG 0x3CU
/** NTAG PWD_AUTH command. */
#define AZ_NFC_CMD_PWD_AUTH 0x1BU
/** NTAG FAST_READ command. */
#define AZ_NFC_CMD_FAST_READ 0x3AU
/** NTAG I2C Plus FAST_WRITE command used for the SRAM request. */
#define AZ_NFC_CMD_FAST_WRITE 0xA6U
/** NTAG I2C SECTOR_SELECT command. */
#define AZ_NFC_CMD_SECTOR_SELECT 0xC2U
/** MFUL compatible-write command (unsupported by I2C Plus, but state outcome is defined). */
#define AZ_NFC_CMD_COMP_WRITE 0xA0U
/** MFUL counter-read command (unsupported by I2C Plus). */
#define AZ_NFC_CMD_READ_CNT 0x39U
/** MFUL counter-increment command (unsupported by I2C Plus). */
#define AZ_NFC_CMD_INCR_CNT 0xA5U
/** MFUL tearing-flag command (unsupported by I2C Plus). */
#define AZ_NFC_CMD_CHECK_TEARING 0x3EU
/** MFUL VCSL command (unsupported by I2C Plus). */
#define AZ_NFC_CMD_VCSL 0x4BU
/** Ultralight-C authentication command (unsupported by I2C Plus). */
#define AZ_NFC_CMD_AUTH 0x1AU
/** Type-2 ACK nibble. */
#define AZ_NFC_ACK 0x0AU
/** Type-2 NAK nibble. */
#define AZ_NFC_NAK 0x00U
/**
 * @brief Fill common ISO14443A identity fields from a generated raw UID.
 * @param data Mutable Ultralight data model.
 * @param raw_uid Nine-byte physical Amiibo UID representation.
 * @return True when the firmware accepts the seven-byte RF UID.
 */
static bool az_nfc_set_identity(MfUltralightData* data, const uint8_t raw_uid[9]) {
    uint8_t uid7[7];
    az_raw_uid_to_nfc_uid(raw_uid, uid7);
    if(!mf_ultralight_set_uid(data, uid7, sizeof(uid7))) return false;
    data->iso14443_3a_data->atqa[0] = 0x44;
    data->iso14443_3a_data->atqa[1] = 0x00;
    data->iso14443_3a_data->sak = 0x00;
    return true;
}

/**
 * @brief Fill type-3 RF identity without applying NTAG215 BCC page rewriting.
 * @param data Mutable I2C Plus model whose physical UID bytes are already in v3 layout.
 * @param uid7 Seven-byte UID presented during ISO14443A anticollision.
 * @return True when the firmware accepts the seven-byte UID.
 */
static bool az_nfc_set_identity_v3(MfUltralightData* data, const uint8_t uid7[7]) {
    if(!data || !uid7 || !mf_ultralight_set_uid(data, uid7, 7U)) return false;

    /* Match Flipper's native NTAG I2C generator: establish the normal MFUL ISO14443A
     * identity first, then expose the I2C memory header with the seven UID bytes directly. */
    data->iso14443_3a_data->atqa[0] = 0x44;
    data->iso14443_3a_data->atqa[1] = 0x00;
    data->iso14443_3a_data->sak = 0x00;
    memcpy(data->page[0].data, uid7, 7U);
    data->page[1].data[3] = data->iso14443_3a_data->sak;
    data->page[2].data[0] = data->iso14443_3a_data->atqa[0];
    data->page[2].data[1] = data->iso14443_3a_data->atqa[1];

    /* PWD/PACK are unreadable over RF, so keep the real authentication values in the
     * native config pages while the listener returns zeroes for reads of E5/E6. */
    uint8_t* password = data->page[229].data;
    password[0] = (uint8_t)(0xAAU ^ (uid7[1] ^ uid7[3]));
    password[1] = (uint8_t)(0x55U ^ (uid7[2] ^ uid7[4]));
    password[2] = (uint8_t)(0xAAU ^ (uid7[3] ^ uid7[5]));
    password[3] = (uint8_t)(0x55U ^ (uid7[4] ^ uid7[6]));
    data->page[230].data[0] = 0x80U;
    data->page[230].data[1] = 0x80U;
    data->page[230].data[2] = 0x00U;
    data->page[230].data[3] = 0x00U;
    return true;
}

/**
 * @brief Initialize tearing flags to the value used by genuine/standard Amiibo tags.
 * @param data Mutable Ultralight data model.
 */
static void az_nfc_set_tearing_flags(MfUltralightData* data) {
    for(size_t i = 0; i < MF_ULTRALIGHT_TEARING_FLAG_NUM; i++) {
        data->tearing_flag[i].data = MF_ULTRALIGHT_TEARING_FLAG_DEFAULT;
    }
}

/**
 * @brief Install one standard 532-byte encrypted dump into an NTAG215 model.
 * @param data Mutable Ultralight data model.
 * @param dump Encrypted standard Amiibo dump.
 * @param raw_uid Raw UID used for password generation.
 * @return True on success.
 */
static bool az_nfc_install_standard(
    MfUltralightData* data,
    const uint8_t dump[AZ_DUMP_SIZE],
    const uint8_t raw_uid[9]) {
    static const uint8_t version[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
    data->type = MfUltralightTypeNTAG215;
    data->pages_total = 135;
    data->pages_read = 135;
    data->auth_attempts = 0;
    memcpy(&data->version, version, sizeof(version));
    memset(&data->signature, 0, sizeof(data->signature));
    memset(data->counter, 0, sizeof(data->counter));
    memset(data->page, 0, AZ_NTAG215_BYTES);
    memcpy(data->page, dump, AZ_DUMP_SIZE);
    if(!az_nfc_set_identity(data, raw_uid)) return false;

    /* Flipper models the tearing flag separately from physical page RFUI. */
    data->page[130].data[3] = 0x00;
    az_nfc_set_tearing_flags(data);

    uint8_t password[4];
    az_tag_password(raw_uid, password);
    memcpy(data->page[133].data, password, sizeof(password));
    static const uint8_t pack[4] = {0x80, 0x80, 0x00, 0x00};
    memcpy(data->page[134].data, pack, sizeof(pack));
    return true;
}

/**
 * @brief Expand a standard 540-byte Amiibo image into Flipper's native I2C Plus storage model.
 * @param standard Source standard page image including PWD/PACK.
 * @param data Mutable I2C Plus 2K model receiving rider/config/session data.
 *
 * Flipper intentionally compacts NTAG I2C Plus memory: sector-0 EEPROM pages 0-E9 stay at
 * their numeric indices, session pages EC/ED live at indices 234/235, and all 256 pages of
 * sector 1 live at indices 236-491.  Sector-0 SRAM F0-FF is therefore kept separately by
 * Amiibo Zero and supplied by the custom listener from the selected lock-on payload.
 */
static void az_nfc_expand_v3(
    const uint8_t standard[AZ_NTAG215_BYTES],
    MfUltralightData* data) {
    memset(data->page, 0, sizeof(data->page));
    uint8_t* sector0 = (uint8_t*)data->page;

    memcpy(sector0, standard, AZ_V3_SHIFT_START);
    memcpy(
        sector0 + AZ_V3_SHIFT_START + AZ_V3_SHIFT_SIZE,
        standard + AZ_V3_SHIFT_START,
        AZ_NTAG215_BYTES - AZ_V3_SHIFT_START);

    /* Match pixl.js's observed v3 sector-0 layout.  E2 is the dynamic-lock page,
     * E3/E4 are the two configuration pages, E5-EB are otherwise zero in the raw image,
     * and EC/ED are the mapped session registers.  PWD/PACK are installed separately by
     * az_nfc_set_identity_v3() because our listener serves the Type-2 auth handshake. */
    static const uint8_t page_e2[4] = {0x01, 0x00, 0xFF, 0x00};
    static const uint8_t page_e3[4] = {0x00, 0x00, 0x00, 0x04};
    static const uint8_t page_e4[4] = {0x07, 0x00, 0x00, 0x00};
    static const uint8_t page_ec[4] = {0x41, 0x00, 0xF8, 0x48};
    static const uint8_t page_ed[4] = {0x08, 0x01, 0x29, 0x00};
    memcpy(sector0 + 0x388, page_e2, sizeof(page_e2));
    memcpy(sector0 + 0x38C, page_e3, sizeof(page_e3));
    memcpy(sector0 + 0x390, page_e4, sizeof(page_e4));
    memset(sector0 + 0x394, 0, (0xECU - 0xE5U) * 4U);
    memcpy(data->page[234].data, page_ec, sizeof(page_ec));
    memcpy(data->page[235].data, page_ed, sizeof(page_ed));
}

/**
 * @brief Install an encrypted rider dump into a type-3 I2C Plus 2K native model.
 * @param data Mutable Ultralight data model.
 * @param dump Type-3 encrypted Amiibo crypto image before the 0x40-byte layout expansion.
 * @param uid7 Direct seven-byte RF UID used by the type-3 image.
 * @return True on success. Lock-on SRAM is intentionally external to this native model.
 */
static bool az_nfc_install_v3(
    MfUltralightData* data,
    const uint8_t dump[AZ_DUMP_SIZE],
    const uint8_t uid7[7]) {
    static const uint8_t version[8] = {0x00, 0x04, 0x04, 0x05, 0x02, 0x02, 0x15, 0x03};
    uint8_t standard[AZ_NTAG215_BYTES];
    memset(standard, 0, sizeof(standard));
    memcpy(standard, dump, AZ_DUMP_SIZE);

    data->type = MfUltralightTypeNTAGI2CPlus2K;
    data->pages_total = AZ_V3_PAGES;
    data->pages_read = AZ_V3_PAGES;
    data->auth_attempts = 0;
    memcpy(&data->version, version, sizeof(version));
    memset(&data->signature, 0, sizeof(data->signature));
    memset(data->counter, 0, sizeof(data->counter));
    az_nfc_expand_v3(standard, data);
    if(!az_nfc_set_identity_v3(data, uid7)) return false;

    az_nfc_set_tearing_flags(data);
    return true;
}

/**
 * @brief Extract a standard encrypted 532-byte dump from either supported physical layout.
 * @param data Source Ultralight data model.
 * @param out_dump Destination standard encrypted dump.
 * @return True for supported page geometry.
 */
static bool az_nfc_extract_standard_dump(
    const MfUltralightData* data,
    uint8_t out_dump[AZ_DUMP_SIZE]) {
    if(!data || !out_dump) return false;
    const uint8_t* bytes = (const uint8_t*)data->page;
    if(data->type == MfUltralightTypeNTAG215 && data->pages_total >= 133) {
        memcpy(out_dump, bytes, AZ_DUMP_SIZE);
        return true;
    }
    if(data->type == MfUltralightTypeNTAGI2CPlus2K && data->pages_total >= AZ_V3_PAGES) {
        memcpy(out_dump, bytes, AZ_V3_SHIFT_START);
        memcpy(
            out_dump + AZ_V3_SHIFT_START,
            bytes + AZ_V3_SHIFT_START + AZ_V3_SHIFT_SIZE,
            AZ_DUMP_SIZE - AZ_V3_SHIFT_START);
        return true;
    }
    return false;
}

/**
 * @brief Generate fresh standard or type-3 Amiibo data and install it into a Flipper device model.
 */
bool az_nfc_generate_device(NfcDevice* device, const AzFigure* figure, const AzKeys* keys) {
    if(!device || !figure || !keys || !keys->valid) return false;

    uint8_t dump[AZ_DUMP_SIZE];
    MfUltralightData* data = mf_ultralight_alloc();
    if(!data) return false;

    bool ok = false;
    if(az_figure_is_v3(figure->id)) {
        uint8_t uid7[7];
        ok = az_generate_v3_dump(figure->id, keys, dump, uid7) &&
             az_nfc_install_v3(data, dump, uid7);
    } else {
        uint8_t raw_uid[9];
        ok = az_generate_dump(figure->id, keys, dump, raw_uid) &&
             az_nfc_install_standard(data, dump, raw_uid);
    }

    if(ok) nfc_device_set_data(device, NfcProtocolMfUltralight, data);
    mf_ultralight_free(data);
    return ok;
}

/**
 * @brief Return whether a native device uses the supported type-3 geometry.
 */
bool az_nfc_device_is_v3(const NfcDevice* device) {
    if(!az_nfc_validate_amiibo(device)) return false;
    const MfUltralightData* data =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    return data && data->type == MfUltralightTypeNTAGI2CPlus2K;
}

/** Maximum standard-frame payload emitted by Flipper's MFUL listener. */
#define AZ_V3_MAX_RESPONSE_BYTES 1024U
/** Maximum transmitted bytes after appending CRC-A to a standard response. */
#define AZ_V3_TX_CAPACITY (AZ_V3_MAX_RESPONSE_BYTES + 2U)
/** First NTAG I2C Plus dynamic-lock page in sector 0. */
#define AZ_V3_DYNAMIC_LOCK_PAGE 0xE2U
/** First NTAG I2C Plus configuration page in sector 0. */
#define AZ_V3_CONFIG_PAGE 0xE3U
/** Hidden PWD page in sector 0. */
#define AZ_V3_PWD_PAGE 0xE5U
/** Hidden PACK page in sector 0. */
#define AZ_V3_PACK_PAGE 0xE6U
/** PT_I2C protection page in sector 0. */
#define AZ_V3_PT_I2C_PAGE 0xE7U
/** First persistent I2C Plus configuration-register page. */
#define AZ_V3_REGISTER_CONFIG_PAGE 0xE8U
/** Second persistent configuration-register page containing REG_LOCK. */
#define AZ_V3_REGISTER_LOCK_PAGE 0xE9U
/** First session-register page in sector 0. */
#define AZ_V3_SESSION_PAGE 0xECU
/** NS_REG session page in sector 0. */
#define AZ_V3_NS_REG_PAGE 0xEDU
/** First SRAM page in sector 0. */
#define AZ_V3_SRAM_FIRST_PAGE 0xF0U
/** Last SRAM page in sector 0. */
#define AZ_V3_SRAM_LAST_PAGE 0xFFU
/** Mirrored session-register page in sector 3. */
#define AZ_V3_MIRROR_SESSION_PAGE 0xF8U
/** Mirrored NS_REG page in sector 3. */
#define AZ_V3_MIRROR_NS_REG_PAGE 0xF9U

/**
 * @brief Internal result matching Flipper's MfUltralightCommand post-processing states.
 */
typedef enum {
    AzV3CommandNotFound,
    AzV3CommandProcessed,
    AzV3CommandProcessedAck,
    AzV3CommandProcessedSilent,
    AzV3CommandProcessedNoResponse,
    AzV3CommandNotProcessedNak,
    AzV3CommandNotProcessedAuthNak,
} AzV3CommandResult;

/**
 * @brief Compute ISO14443A CRC-A for standard response frames.
 */
static uint16_t az_crc_a(const uint8_t* data, size_t length) {
    uint16_t crc = 0x6363U;
    for(size_t i = 0; i < length; i++) {
        uint8_t byte = (uint8_t)(data[i] ^ (uint8_t)crc);
        byte ^= (uint8_t)(byte << 4);
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)byte << 8) ^ ((uint16_t)byte << 3) ^
                         (byte >> 4));
    }
    return crc;
}

/**
 * @brief Send a normal Type-2 response using Flipper's standard-frame wire format.
 *
 * The stock MfUltralight listener appends CRC-A before handing the frame to the ISO14443A
 * transport. Amiibo Zero uses the public raw transport here, so it performs that append itself.
 */
static bool az_v3_send_standard(AmiiboZeroApp* app, const uint8_t* data, size_t size) {
    if(!app || !app->v3_tx_buffer || !data || size > AZ_V3_MAX_RESPONSE_BYTES) return false;

    bit_buffer_copy_bytes(app->v3_tx_buffer, data, size);
    const uint16_t crc = az_crc_a(data, size);
    const uint8_t crc_bytes[2] = {(uint8_t)crc, (uint8_t)(crc >> 8)};
    bit_buffer_append_bytes(app->v3_tx_buffer, crc_bytes, sizeof(crc_bytes));
    return nfc_listener_tx(app->nfc, app->v3_tx_buffer) == NfcErrorNone;
}

/**
 * @brief Send Flipper's four-bit MFUL ACK/NAK response.
 */
static bool az_v3_send_short(AmiiboZeroApp* app, uint8_t value) {
    if(!app || !app->v3_tx_buffer) return false;
    bit_buffer_set_size(app->v3_tx_buffer, 4U);
    bit_buffer_set_byte(app->v3_tx_buffer, 0, (uint8_t)(value & 0x0FU));
    return nfc_listener_tx(app->nfc, app->v3_tx_buffer) == NfcErrorNone;
}

/**
 * @brief Return the mutable native I2C Plus data model used by the custom v3 listener.
 */
static MfUltralightData* az_v3_data(AmiiboZeroApp* app) {
    if(!app || !app->nfc_device) return NULL;
    return (MfUltralightData*)nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
}

/**
 * @brief Validate READ/FAST_READ starting pages against the NTAG I2C Plus command map.
 */
static bool az_v3_read_start_valid(const AmiiboZeroApp* app, uint16_t page) {
    if(!app || page > 0xFFU) return false;
    switch(app->v3_sector) {
    case 0:
        /* NXP permits READ/FAST_READ to start in EEPROM/config through E9, the two
         * session-register pages EC/ED, and (when mapped/pass-through is active) SRAM.
         * Amiibo v3 compatibility always exposes the precomputed F0-FF SRAM window. */
        return page <= AZ_V3_REGISTER_LOCK_PAGE ||
               (page >= AZ_V3_SESSION_PAGE && page <= AZ_V3_NS_REG_PAGE) ||
               (page >= AZ_V3_SRAM_FIRST_PAGE && page <= AZ_V3_SRAM_LAST_PAGE);
    case 1:
        return true;
    case 2:
        return false;
    case 3:
        return page >= AZ_V3_MIRROR_SESSION_PAGE && page <= AZ_V3_MIRROR_NS_REG_PAGE;
    default:
        return false;
    }
}

/**
 * @brief Return whether WRITE may address a persistent/compatibility page in the selected sector.
 */
static bool az_v3_write_start_valid(const AmiiboZeroApp* app, uint16_t page) {
    if(!app || page > 0xFFU) return false;
    switch(app->v3_sector) {
    case 0:
        /* NXP WRITE starts are 02-E9; F0-FF is accepted here only for the known v3
         * compatibility mailbox behavior. EC/ED are volatile read-only session registers. */
        return (page >= 0x02U && page <= AZ_V3_REGISTER_LOCK_PAGE) ||
               (page >= AZ_V3_SRAM_FIRST_PAGE && page <= AZ_V3_SRAM_LAST_PAGE);
    case 1:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Validate an inclusive FAST_READ range in the currently selected sector.
 *
 * NXP validates the start address. If a valid request subsequently traverses an invalid
 * address in the same sector, those bytes read as zero; the per-page reader below provides
 * that zero fill. Sector 3 only exposes the F8/F9 session-register mirror as a valid start.
 */
static bool az_v3_range_valid(const AmiiboZeroApp* app, uint16_t start_page, uint16_t end_page) {
    return app && start_page <= end_page && end_page <= 0xFFU &&
           az_v3_read_start_valid(app, start_page);
}

/**
 * @brief Map one logical RF page to Flipper's compact native I2C Plus page array.
 */
static bool az_v3_native_page(const AmiiboZeroApp* app, uint16_t page, uint16_t* native_page) {
    if(!app || !native_page || page > 0xFFU) return false;

    switch(app->v3_sector) {
    case 0:
        if(page <= 0xE9U) {
            *native_page = page;
            return true;
        }
        if(page == AZ_V3_SESSION_PAGE) {
            *native_page = 234U;
            return true;
        }
        if(page == AZ_V3_NS_REG_PAGE) {
            *native_page = 235U;
            return true;
        }
        return false; /* F0-FF is the external SRAM window. */
    case 1:
        *native_page = (uint16_t)(236U + page);
        return true;
    case 3:
        if(page == AZ_V3_MIRROR_SESSION_PAGE) {
            *native_page = 234U;
            return true;
        }
        if(page == AZ_V3_MIRROR_NS_REG_PAGE) {
            *native_page = 235U;
            return true;
        }
        return false;
    default:
        return false;
    }
}

/**
 * @brief Return true when one RF page names NS_REG in either mapped location.
 */
static bool az_v3_is_ns_reg(const AmiiboZeroApp* app, uint16_t page) {
    return app && ((app->v3_sector == 0 && page == AZ_V3_NS_REG_PAGE) ||
                   (app->v3_sector == 3 && page == AZ_V3_MIRROR_NS_REG_PAGE));
}

/**
 * @brief Read one logical page with Flipper's hidden-PWD/PACK and invalid-page behavior.
 */
static void az_v3_read_page(
    AmiiboZeroApp* app,
    const MfUltralightData* data,
    uint16_t page,
    uint8_t out[4]) {
    memset(out, 0, 4U);
    if(!app || !data || page > 0xFFU) return;

    if(app->v3_sector == 0 && page >= AZ_V3_SRAM_FIRST_PAGE && page <= AZ_V3_SRAM_LAST_PAGE) {
        const size_t offset = (size_t)(page - AZ_V3_SRAM_FIRST_PAGE) * 4U;
        memcpy(out, app->current_lockon_sram + offset, 4U);
        return;
    }

    /* PWD/PACK are special only in sector 0.  The same page numbers in sector 1 are
     * ordinary user memory on the 2K part. */
    if(app->v3_sector == 0U && (page == AZ_V3_PWD_PAGE || page == AZ_V3_PACK_PAGE)) return;

    uint16_t native_page = 0;
    if(!az_v3_native_page(app, page, &native_page) || native_page >= data->pages_total) return;
    memcpy(out, data->page[native_page].data, 4U);

    if(az_v3_is_ns_reg(app, page)) {
        /* pixl.js found that Joy-Con readers poll EC with a normal READ and wait for the
         * SRAM_RF_READY bit in the returned ED page.  Advertise the precomputed lock-on
         * response as ready on every NS_REG read instead of depending on command ordering. */
        out[2] |= 0x08U;
    }
}

/**
 * @brief Apply one pixl.js-compatible v3 WRITE to the compact native I2C Plus model.
 *
 * The working v3 path deliberately relaxes lock/auth enforcement for ordinary valid EEPROM
 * writes, matching the public Pixl compatibility behavior. Address validity still follows the
 * I2C Plus command map: invalid gaps and read-only session-register pages are rejected. Sector-0
 * SRAM remains external and the known v3 mailbox write is ACKed without replacing its response.
 */
static AzV3CommandResult az_v3_write_page(
    AmiiboZeroApp* app,
    MfUltralightData* data,
    uint16_t page,
    const uint8_t payload[4]) {
    if(!app || !data || !payload) return AzV3CommandNotProcessedNak;

    if(!az_v3_write_start_valid(app, page)) return AzV3CommandNotProcessedNak;

    /* Keep the precomputed lock-on SRAM response external to the native NFC device, exactly
     * as the UI-memory-fix baseline did.  Pixl ACKs the mailbox write without needing to make
     * it persistent tag data. */
    if(app->v3_sector == 0U && page >= AZ_V3_SRAM_FIRST_PAGE && page <= AZ_V3_SRAM_LAST_PAGE) {
        return AzV3CommandProcessedAck;
    }

    if(app->v3_sector == 0U && page == 2U) {
        /* Match pixl.js's special page-2 handling: only the two lock bytes are writable. */
        data->page[2].data[2] = payload[2];
        data->page[2].data[3] = payload[3];
        return AzV3CommandProcessedAck;
    }

    uint16_t native_page = 0U;
    if(!az_v3_native_page(app, page, &native_page) || native_page >= data->pages_total) {
        return AzV3CommandNotProcessedNak;
    }
    memcpy(data->page[native_page].data, payload, 4U);
    return AzV3CommandProcessedAck;
}

/**
 * @brief Dispatch one complete MFUL command with Flipper-compatible lengths and outcomes.
 */
static AzV3CommandResult az_v3_dispatch_command(
    AmiiboZeroApp* app,
    MfUltralightData* data,
    const uint8_t* rx,
    size_t size) {
    if(!app || !data || !rx || size == 0U) return AzV3CommandNotFound;

    if(app->v3_sector_select_pending) {
        app->v3_sector_select_pending = false;
        if(size != 4U || rx[1] != 0U || rx[2] != 0U || rx[3] != 0U) {
            return AzV3CommandNotProcessedNak;
        }
        app->v3_sector = rx[0];
        return AzV3CommandProcessedSilent;
    }

    switch(rx[0]) {
    case AZ_NFC_CMD_READ: {
        if(size != 2U) return AzV3CommandNotFound;
        const uint16_t start_page = rx[1];
        if(!az_v3_read_start_valid(app, start_page)) return AzV3CommandNotProcessedNak;

        uint8_t response[16];
        for(uint16_t i = 0U; i < 4U; i++) {
            az_v3_read_page(app, data, start_page + i, response + i * 4U);
        }
        az_v3_send_standard(app, response, sizeof(response));
        return AzV3CommandProcessed;
    }

    case AZ_NFC_CMD_FAST_READ: {
        if(size != 3U) return AzV3CommandNotFound;
        const uint16_t start_page = rx[1];
        const uint16_t end_page = rx[2];
        if(end_page < start_page || !az_v3_range_valid(app, start_page, end_page)) {
            return AzV3CommandNotProcessedNak;
        }

        const uint16_t page_count = (uint16_t)(end_page - start_page + 1U);
        if(page_count == 0U || !app->v3_response_buffer) return AzV3CommandNotProcessedNak;

        for(uint16_t i = 0U; i < page_count; i++) {
            az_v3_read_page(app, data, start_page + i, app->v3_response_buffer + i * 4U);
        }
        az_v3_send_standard(app, app->v3_response_buffer, (size_t)page_count * 4U);
        return AzV3CommandProcessed;
    }

    case AZ_NFC_CMD_WRITE:
        if(size != 6U) return AzV3CommandNotFound;
        return az_v3_write_page(app, data, rx[1], rx + 2U);

    case AZ_NFC_CMD_FAST_WRITE:
        if(size != 67U) return AzV3CommandNotFound;
        if(rx[1] != AZ_V3_SRAM_FIRST_PAGE || rx[2] != AZ_V3_SRAM_LAST_PAGE ||
           app->v3_sector != 0U) {
            return AzV3CommandNotProcessedNak;
        }
        /* Match the working pixl.js-compatible path: ACK FAST_WRITE without replacing the
         * selected precomputed SRAM response. */
        return AzV3CommandProcessedAck;

    case AZ_NFC_CMD_GET_VERSION:
        if(size != 1U) return AzV3CommandNotFound;
        az_v3_send_standard(app, (const uint8_t*)&data->version, sizeof(data->version));
        return AzV3CommandProcessed;

    case AZ_NFC_CMD_READ_SIG:
        if(size != 2U) return AzV3CommandNotFound;
        az_v3_send_standard(app, data->signature.data, sizeof(data->signature.data));
        return AzV3CommandProcessed;

    case AZ_NFC_CMD_PWD_AUTH: {
        if(size != 5U) return AzV3CommandNotFound;
        static const uint8_t pixl_pack[2] = {0x80U, 0x80U};
        app->v3_authenticated = true;
        data->auth_attempts = 0U;
        az_v3_send_standard(app, pixl_pack, sizeof(pixl_pack));
        return AzV3CommandProcessed;
    }

    case AZ_NFC_CMD_SECTOR_SELECT:
        if(size != 2U) return AzV3CommandNotFound;
        if(rx[1] != 0xFFU) return AzV3CommandNotProcessedNak;
        app->v3_sector_select_pending = true;
        return AzV3CommandProcessedAck;

    /* pixl.js leaves unsupported v3 commands unanswered and keeps listening.  Treating these
     * as MFUL NAK/faults was observable during normal console probing. */
    case AZ_NFC_CMD_COMP_WRITE:
    case AZ_NFC_CMD_INCR_CNT:
    case AZ_NFC_CMD_VCSL:
    case AZ_NFC_CMD_READ_CNT:
    case AZ_NFC_CMD_CHECK_TEARING:
    case AZ_NFC_CMD_AUTH:
    default:
        return AzV3CommandProcessedNoResponse;
    }
}

/**
 * @brief Apply the compact v3 command post-processing semantics.
 */
static NfcCommand az_v3_postprocess(AmiiboZeroApp* app, AzV3CommandResult result) {
    if(result == AzV3CommandProcessedAck) {
        az_v3_send_short(app, AZ_NFC_ACK);
        return NfcCommandContinue;
    }
    if(result == AzV3CommandProcessedSilent) return NfcCommandReset;
    if(result == AzV3CommandProcessedNoResponse) return NfcCommandContinue;
    if(result == AzV3CommandProcessed) return NfcCommandContinue;

    app->v3_authenticated = false;
    if(result == AzV3CommandNotProcessedNak)
        az_v3_send_short(app, AZ_NFC_NAK);
    else if(result == AzV3CommandNotProcessedAuthNak)
        az_v3_send_short(app, 0x04U);

    /* Stock MfUltralightListener sleeps after unsupported/malformed commands rather than merely
     * resetting RX. Readers depend on this state transition during tag probing. */
    return NfcCommandSleep;
}

/**
 * @brief Reset the same volatile MFUL state that Flipper clears on raw data, HALT, or field loss.
 */
static NfcCommand az_v3_reset_listener_state(AmiiboZeroApp* app) {
    app->v3_sector_select_pending = false;
    app->v3_sector = 0U;
    app->v3_authenticated = false;
    return NfcCommandSleep;
}

/**
 * @brief ISO14443A callback implementing Flipper's MFUL listener state machine plus I2C SRAM.
 */
static NfcCommand az_v3_listener_callback(NfcGenericEvent event, void* context) {
    AmiiboZeroApp* app = context;
    if(!app || event.protocol != NfcProtocolIso14443_3a || !event.event_data) return NfcCommandStop;

    const Iso14443_3aListenerEvent* iso_event = event.event_data;
    if(iso_event->type == Iso14443_3aListenerEventTypeReceivedStandardFrame) {
        if(!iso_event->data || !iso_event->data->buffer) return NfcCommandSleep;
        const BitBuffer* buffer = iso_event->data->buffer;
        if(bit_buffer_has_partial_byte(buffer)) return az_v3_reset_listener_state(app);

        const size_t payload_size = bit_buffer_get_size_bytes(buffer);
        if(payload_size == 0U) return az_v3_reset_listener_state(app);
        MfUltralightData* data = az_v3_data(app);
        if(!data) return NfcCommandStop;
        return az_v3_postprocess(
            app,
            az_v3_dispatch_command(app, data, bit_buffer_get_data(buffer), payload_size));
    }

    if(iso_event->type == Iso14443_3aListenerEventTypeReceivedData) {
        /* The second SECTOR_SELECT frame is intentionally sent without a normal Type-2 CRC,
         * so Flipper surfaces it as raw ISO14443A data.  Do not mistake that frame for a
         * listener reset: it selects sector 0/1 for the commands that follow. */
        if(app->v3_sector_select_pending && iso_event->data && iso_event->data->buffer) {
            const BitBuffer* buffer = iso_event->data->buffer;
            if(!bit_buffer_has_partial_byte(buffer)) {
                const size_t payload_size = bit_buffer_get_size_bytes(buffer);
                if(payload_size) {
                    MfUltralightData* data = az_v3_data(app);
                    if(!data) return NfcCommandStop;
                    return az_v3_postprocess(
                        app,
                        az_v3_dispatch_command(
                            app, data, bit_buffer_get_data(buffer), payload_size));
                }
            }
        }
        return az_v3_reset_listener_state(app);
    }
    if(iso_event->type == Iso14443_3aListenerEventTypeFieldOff ||
       iso_event->type == Iso14443_3aListenerEventTypeHalted) {
        return az_v3_reset_listener_state(app);
    }
    return NfcCommandContinue;
}

/**
 * @brief Start stock MFUL emulation for v1/v2 or the stock-derived I2C Plus listener for v3.
 */
bool az_nfc_listener_start(AmiiboZeroApp* app) {
    if(!app || app->emulating || app->listener || app->v3_i2c_listener) return false;

    const bool is_v3 = az_nfc_device_is_v3(app->nfc_device);
    const NfcDeviceData* device_data =
        nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
    if(!device_data) return false;

    if(!is_v3) {
        app->listener = nfc_listener_alloc(app->nfc, NfcProtocolMfUltralight, device_data);
        if(!app->listener) return false;
        nfc_listener_start(app->listener, NULL, NULL);
        app->emulating = true;
        return true;
    }

    if(!app->current_lockon_valid) return false;
    MfUltralightData* data = (MfUltralightData*)device_data;
    app->listener = nfc_listener_alloc(app->nfc, NfcProtocolIso14443_3a, data->iso14443_3a_data);
    if(!app->listener) return false;

    app->v3_tx_buffer = bit_buffer_alloc(AZ_V3_TX_CAPACITY);
    if(!app->v3_tx_buffer) {
        nfc_listener_free(app->listener);
        app->listener = NULL;
        return false;
    }
    app->v3_response_buffer = malloc(AZ_V3_MAX_RESPONSE_BYTES);
    if(!app->v3_response_buffer) {
        bit_buffer_free(app->v3_tx_buffer);
        app->v3_tx_buffer = NULL;
        nfc_listener_free(app->listener);
        app->listener = NULL;
        return false;
    }

    app->v3_sector = 0U;
    app->v3_sector_select_pending = false;
    app->v3_authenticated = false;
    nfc_listener_start(app->listener, az_v3_listener_callback, app);
    app->v3_i2c_listener = true;
    app->emulating = true;
    return true;
}

/**
 * @brief Stop either emulation backend while retaining writes made directly to nfc_device.
 */
bool az_nfc_listener_pause_and_sync(AmiiboZeroApp* app) {
    if(!app || !app->emulating || !app->listener) return false;

    if(app->v3_i2c_listener) {
        nfc_listener_stop(app->listener);
        nfc_listener_free(app->listener);
        app->listener = NULL;
        if(app->v3_tx_buffer) bit_buffer_free(app->v3_tx_buffer);
        app->v3_tx_buffer = NULL;
        free(app->v3_response_buffer);
        app->v3_response_buffer = NULL;
        app->v3_i2c_listener = false;
        app->v3_sector_select_pending = false;
        app->v3_sector = 0U;
        app->v3_authenticated = false;
        app->emulating = false;
        return true;
    }

    nfc_listener_stop(app->listener);
    const NfcDeviceData* latest = nfc_listener_get_data(app->listener, NfcProtocolMfUltralight);
    if(latest) nfc_device_set_data(app->nfc_device, NfcProtocolMfUltralight, latest);
    nfc_listener_free(app->listener);
    app->listener = NULL;
    app->emulating = false;
    return latest != NULL;
}

/**
 * @brief Randomize a standard NTAG215 Amiibo UID by decrypting and re-encrypting it.
 *
 * Type-3 UID randomization is intentionally disabled: real games bind v3 figures to their
 * original identity and changing it makes an otherwise valid figure unrecognizable.
 */
bool az_nfc_randomize_uid(NfcDevice* device, const AzKeys* keys) {
    if(!device || !keys || !keys->valid || !az_nfc_validate_amiibo(device)) return false;
    MfUltralightData* data =
        (MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    if(!data || data->type != MfUltralightTypeNTAG215) return false;

    uint8_t old_dump[AZ_DUMP_SIZE];
    uint8_t new_dump[AZ_DUMP_SIZE];
    if(!az_nfc_extract_standard_dump(data, old_dump)) return false;

    uint8_t new_raw_uid[9];
    if(!az_rekey_dump_uid(old_dump, keys, new_dump, new_raw_uid)) return false;
    memcpy(data->page, new_dump, AZ_DUMP_SIZE);
    data->page[130].data[3] = 0x00;
    uint8_t password[4];
    az_tag_password(new_raw_uid, password);
    memcpy(data->page[133].data, password, sizeof(password));
    static const uint8_t pack[4] = {0x80, 0x80, 0x00, 0x00};
    memcpy(data->page[134].data, pack, sizeof(pack));
    return az_nfc_set_identity(data, new_raw_uid);
}

/**
 * @brief Convert one bounded UTF-16 field into UTF-8 for display.
 */
static void az_utf16_to_utf8(
    const uint8_t* source,
    size_t code_units,
    bool big_endian,
    char* out,
    size_t out_size) {
    if(!out || out_size == 0) return;
    out[0] = '\0';
    if(!source) return;
    size_t p = 0;
    for(size_t i = 0; i < code_units; i++) {
        const uint8_t a = source[i * 2U];
        const uint8_t b = source[i * 2U + 1U];
        const uint16_t cp = big_endian ? (uint16_t)(((uint16_t)a << 8) | b) :
                                         (uint16_t)(((uint16_t)b << 8) | a);
        if(cp == 0 || cp == 0xFFFFU) break;
        if(cp < 0x80U) {
            if(p + 1 >= out_size) break;
            out[p++] = (char)cp;
        } else if(cp < 0x800U) {
            if(p + 2 >= out_size) break;
            out[p++] = (char)(0xC0U | (cp >> 6));
            out[p++] = (char)(0x80U | (cp & 0x3FU));
        } else if(cp < 0xD800U || cp > 0xDFFFU) {
            if(p + 3 >= out_size) break;
            out[p++] = (char)(0xE0U | (cp >> 12));
            out[p++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
            out[p++] = (char)(0x80U | (cp & 0x3FU));
        }
    }
    out[p] = '\0';
}

static void az_utf16be_to_utf8(
    const uint8_t* source,
    size_t code_units,
    char* out,
    size_t out_size) {
    az_utf16_to_utf8(source, code_units, true, out, out_size);
}

static void az_utf16le_to_utf8(
    const uint8_t* source,
    size_t code_units,
    char* out,
    size_t out_size) {
    az_utf16_to_utf8(source, code_units, false, out, out_size);
}

/**
 * @brief Format Nintendo's packed Amiibo date into YYYY-MM-DD.
 * @param bytes Two-byte big-endian packed date.
 * @param out Destination text buffer.
 * @param out_size Destination capacity.
 */
static void az_format_amiibo_date(const uint8_t bytes[2], char* out, size_t out_size) {
    uint16_t value = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    unsigned year = 2000U + ((value & 0xFE00U) >> 9);
    unsigned month = (value & 0x01E0U) >> 5;
    unsigned day = value & 0x001FU;
    if(month >= 1U && month <= 12U && day >= 1U && day <= 31U) {
        snprintf(out, out_size, "%04u-%02u-%02u", year, month, day);
    } else {
        az_str_copy(out, out_size, "-");
    }
}

/**
 * @brief Read one big-endian 64-bit value without alignment assumptions.
 * @param bytes Eight source bytes.
 * @return Decoded value.
 */
static uint64_t az_read_be64(const uint8_t bytes[8]) {
    uint64_t value = 0;
    for(size_t i = 0; i < 8; i++) value = (value << 8) | bytes[i];
    return value;
}

/**
 * @brief Decode authenticated Amiibo metadata from a standard or type-3 device image.
 */
bool az_nfc_read_details(const NfcDevice* device, const AzKeys* keys, AzAmiiboDetails* out) {
    if(!out) return false;
    memset(out, 0, sizeof(*out));
    if(!device || !keys || !keys->valid || !az_nfc_validate_amiibo(device)) return false;
    const MfUltralightData* data =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    uint8_t encrypted[AZ_DUMP_SIZE];
    uint8_t plain[AZ_DUMP_SIZE];
    if(!az_nfc_extract_standard_dump(data, encrypted) || !az_decrypt_dump(encrypted, keys, plain)) {
        return false;
    }

    /* az_decrypt_dump() keeps the physical tag byte layout.  The commonly
     * documented decrypted "internal" Amiibo structure relocates physical
     * ranges before decryption, so its offsets cannot be used directly here.
     * These offsets are the equivalent fields in the physical 532-byte image. */
    out->available = true;
    out->initialized = (plain[0x14] & 0x10U) != 0;
    out->app_data_initialized = (plain[0x14] & 0x20U) != 0;
    out->write_counter = (uint16_t)(((uint16_t)plain[0x11] << 8) | plain[0x12]);
    az_format_amiibo_date(plain + 0x18, out->init_date, sizeof(out->init_date));
    az_format_amiibo_date(plain + 0x1A, out->write_date, sizeof(out->write_date));
    if(out->initialized) {
        az_utf16be_to_utf8(plain + 0x20, 10, out->nickname, sizeof(out->nickname));
        az_utf16le_to_utf8(plain + 0xBA, 10, out->owner_mii, sizeof(out->owner_mii));
    }
    if(out->app_data_initialized) {
        out->application_id = az_read_be64(plain + 0x100);
        out->application_write_counter =
            (uint16_t)(((uint16_t)plain[0x108] << 8) | plain[0x109]);
        out->application_area_id =
            ((uint32_t)plain[0x10A] << 24) | ((uint32_t)plain[0x10B] << 16) |
            ((uint32_t)plain[0x10C] << 8) | (uint32_t)plain[0x10D];
    }
    return true;
}

/**
 * @brief Validate that NFC device data has supported standard or type-3 Amiibo geometry.
 */
bool az_nfc_validate_amiibo(const NfcDevice* device) {
    if(!device || nfc_device_get_protocol(device) != NfcProtocolMfUltralight) return false;
    const MfUltralightData* data =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    if(!data) return false;
    if(data->type == MfUltralightTypeNTAG215) return data->pages_total >= 133;
    if(data->type == MfUltralightTypeNTAGI2CPlus2K) return data->pages_total >= AZ_V3_PAGES;
    return false;
}

/**
 * @brief Extract the eight-byte figure ID from either supported Amiibo device layout.
 */
bool az_nfc_extract_figure_id(const NfcDevice* device, uint8_t out_id[8]) {
    if(!out_id || !az_nfc_validate_amiibo(device)) return false;
    const MfUltralightData* data =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    const uint8_t* bytes = (const uint8_t*)data->page;
    memcpy(out_id, bytes + 84, 8);
    return true;
}

/**
 * @brief Persist NFC device data to a native Flipper .nfc file.
 */
bool az_nfc_save_device(NfcDevice* device, const char* path) {
    return device && path && nfc_device_save(device, path);
}

/**
 * @brief Load and validate NFC device data from a native Flipper .nfc file.
 */
bool az_nfc_load_device(NfcDevice* device, const char* path) {
    if(!device || !path || !nfc_device_load(device, path)) return false;
    return az_nfc_validate_amiibo(device);
}

/**
 * @brief Export only the standard v2 NTAG215 encrypted image.
 */
bool az_nfc_export_v2_dump(const NfcDevice* device, uint8_t out_dump[AZ_DUMP_SIZE]) {
    if(!device || !out_dump || nfc_device_get_protocol(device) != NfcProtocolMfUltralight) {
        return false;
    }
    const MfUltralightData* data =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    if(!data || data->type != MfUltralightTypeNTAG215 || data->pages_total < 133U) return false;
    memcpy(out_dump, data->page, AZ_DUMP_SIZE);
    return true;
}

/** Exact NXP/Flipper version tuple constraints needed to call a target NTAG215. */
static bool az_tag_version_is_ntag215(MfUltralightVersion* version) {
    if(!version) return false;
    return version->vendor_id == 0x04U && version->prod_type == 0x04U &&
           version->storage_size == 0x11U &&
           mf_ultralight_get_type_by_version(version) == MfUltralightTypeNTAG215;
}

/** Store one callback result and stop the current NFC phase. */
static NfcCommand az_tag_phase_finish(AmiiboZeroApp* app, AzTagResult result) {
    if(app) {
        app->tag_result = result;
        app->tag_phase_done = true;
    }
    return NfcCommandStop;
}

/** Write one physical NTAG page and translate the result to a boolean. */
static bool az_tag_write_page(
    MfUltralightPoller* poller,
    uint8_t page_num,
    const uint8_t page_bytes[4]) {
    MfUltralightPage page;
    memcpy(page.data, page_bytes, sizeof(page.data));
    return mf_ultralight_poller_write_page(poller, page_num, &page) == MfUltralightErrorNone;
}

/** Authenticate a retail-style Amiibo using the password derived from its physical UID. */
static bool az_tag_authenticate(MfUltralightPoller* poller, const uint8_t raw_uid[9]) {
    MfUltralightPollerAuthContext auth;
    memset(&auth, 0, sizeof(auth));
    az_tag_password(raw_uid, auth.password.data);
    if(mf_ultralight_poller_auth_pwd(poller, &auth) != MfUltralightErrorNone) return false;
    return auth.pack.data[0] == 0x80U && auth.pack.data[1] == 0x80U;
}

/**
 * @brief Validate a blank physical NTAG215 and capture its exact raw UID.
 *
 * Static and dynamic lock bits must still be zero. AUTH0 must retain the factory-disabled
 * value so a previously configured/partially programmed tag cannot be mistaken for blank.
 */
static AzTagResult az_tag_scan_blank(
    AmiiboZeroApp* app,
    MfUltralightPoller* poller,
    uint8_t raw_uid[9],
    uint8_t page2_prefix[2]) {
    UNUSED(app);
    MfUltralightVersion version;
    if(mf_ultralight_poller_read_version(poller, &version) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }
    if(!az_tag_version_is_ntag215(&version)) return AzTagResultWrongTag;

    MfUltralightPageReadCommandData head;
    if(mf_ultralight_poller_read_page(poller, 0U, &head) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }
    const uint8_t* first = (const uint8_t*)head.page;
    memcpy(raw_uid, first, 9U);
    if(page2_prefix) memcpy(page2_prefix, head.page[2].data, 2U);

    MfUltralightPageReadCommandData tail;
    if(mf_ultralight_poller_read_page(poller, 130U, &tail) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }

    const bool static_locked = head.page[2].data[2] != 0U || head.page[2].data[3] != 0U;
    const bool dynamic_locked = tail.page[0].data[0] != 0U || tail.page[0].data[1] != 0U ||
                                tail.page[0].data[2] != 0U;
    const bool configured = tail.page[1].data[3] != 0xFFU;
    if(static_locked || dynamic_locked || configured) return AzTagResultNotBlank;
    return AzTagResultSuccess;
}

/** Program all ordinary Amiibo pages first and irreversible lock bits last. */
static AzTagResult az_tag_program_blank(AmiiboZeroApp* app, MfUltralightPoller* poller) {
    uint8_t live_uid[9];
    uint8_t page2_prefix[2];
    AzTagResult scan = az_tag_scan_blank(app, poller, live_uid, page2_prefix);
    if(scan != AzTagResultSuccess) return scan;
    if(memcmp(live_uid, app->tag_target_uid, sizeof(live_uid)) != 0) {
        return AzTagResultUidChanged;
    }
    if(!app->tag_work_dump) return AzTagResultCryptoFailed;

    const uint8_t* dump = app->tag_work_dump;
    const uint16_t total_steps = 127U + 4U + 2U; /* pages 3-129, PACK/PWD/config x2, locks x2 */
    uint16_t done = 0U;

    for(uint16_t page = 3U; page <= 129U; page++) {
        if(!az_tag_write_page(poller, (uint8_t)page, dump + page * 4U)) {
            return AzTagResultWriteFailed;
        }
        done++;
        app->tag_progress = (uint8_t)((done * 100U) / total_steps);
    }

    static const uint8_t pack[4] = {0x80U, 0x80U, 0x00U, 0x00U};
    if(!az_tag_write_page(poller, 134U, pack)) return AzTagResultWriteFailed;
    done++;

    uint8_t password[4];
    az_tag_password(app->tag_target_uid, password);
    if(!az_tag_write_page(poller, 133U, password)) return AzTagResultWriteFailed;
    done++;

    /* Program AUTH0 before ACCESS/CFGLCK. Once AUTH0 takes effect, authenticate before
     * writing ACCESS so the final config page can safely set CFGLCK. */
    if(!az_tag_write_page(poller, 131U, dump + 131U * 4U)) return AzTagResultWriteFailed;
    done++;
    if(!az_tag_authenticate(poller, app->tag_target_uid)) return AzTagResultAuthFailed;
    if(!az_tag_write_page(poller, 132U, dump + 132U * 4U)) return AzTagResultWriteFailed;
    done++;

    /* Irreversible static and dynamic lock bits are deliberately the final two writes.
     * Preserve the destination tag's manufacturer/BCC bytes in page 2. */
    uint8_t static_lock[4] = {page2_prefix[0], page2_prefix[1], 0x0FU, 0xE0U};
    if(!az_tag_write_page(poller, 2U, static_lock)) return AzTagResultWriteFailed;
    done++;
    static const uint8_t dynamic_lock[4] = {0x01U, 0x00U, 0x0FU, 0x00U};
    if(!az_tag_write_page(poller, 130U, dynamic_lock)) return AzTagResultWriteFailed;
    done++;
    app->tag_progress = 100U;
    return AzTagResultSuccess;
}

/** Rewrite only the permanently-unlocked Amiibo state pages on an existing retail-style tag. */
static AzTagResult az_tag_program_clear(AmiiboZeroApp* app, MfUltralightPoller* poller) {
    MfUltralightVersion version;
    if(mf_ultralight_poller_read_version(poller, &version) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }
    if(!az_tag_version_is_ntag215(&version)) return AzTagResultWrongTag;

    MfUltralightPageReadCommandData head;
    if(mf_ultralight_poller_read_page(poller, 0U, &head) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }
    uint8_t live_uid[9];
    memcpy(live_uid, head.page, sizeof(live_uid));
    if(memcmp(live_uid, app->tag_target_uid, sizeof(live_uid)) != 0) {
        return AzTagResultUidChanged;
    }
    if(!az_tag_authenticate(poller, live_uid)) return AzTagResultAuthFailed;
    if(!app->tag_work_dump) return AzTagResultCryptoFailed;

    const uint16_t total_steps = 9U + 98U; /* pages 4-12 plus 32-129 */
    uint16_t done = 0U;
    for(uint16_t page = 4U; page <= 12U; page++) {
        if(!az_tag_write_page(poller, (uint8_t)page, app->tag_work_dump + page * 4U)) {
            return AzTagResultWriteFailed;
        }
        done++;
        app->tag_progress = (uint8_t)((done * 100U) / total_steps);
    }
    for(uint16_t page = 32U; page <= 129U; page++) {
        if(!az_tag_write_page(poller, (uint8_t)page, app->tag_work_dump + page * 4U)) {
            return AzTagResultWriteFailed;
        }
        done++;
        app->tag_progress = (uint8_t)((done * 100U) / total_steps);
    }
    app->tag_progress = 100U;
    return AzTagResultSuccess;
}

static AzTagResult az_tag_read_amiibo(AmiiboZeroApp* app, MfUltralightPoller* poller);

/** Extended poller callback used for blank validation, direct read, ordered write, and clear-write phases. */
static NfcCommand az_tag_extended_callback(NfcGenericEventEx event, void* context) {
    AmiiboZeroApp* app = context;
    if(!app || !event.poller || !event.parent_event_data) return NfcCommandStop;
    Iso14443_3aPollerEvent* iso_event = event.parent_event_data;
    if(iso_event->type == Iso14443_3aPollerEventTypeError) {
        if(iso_event->data && iso_event->data->error == Iso14443_3aErrorNotPresent) {
            return NfcCommandContinue;
        }
        return az_tag_phase_finish(app, AzTagResultReadFailed);
    }
    if(iso_event->type != Iso14443_3aPollerEventTypeReady) return NfcCommandContinue;

    MfUltralightPoller* poller = event.poller;
    if(app->tag_stage == AzTagStageScanBlank) {
        uint8_t uid[9];
        AzTagResult result = az_tag_scan_blank(app, poller, uid, NULL);
        if(result == AzTagResultSuccess) memcpy(app->tag_target_uid, uid, sizeof(uid));
        return az_tag_phase_finish(app, result);
    }
    if(app->tag_stage == AzTagStageReading) {
        return az_tag_phase_finish(app, az_tag_read_amiibo(app, poller));
    }
    if(app->tag_stage == AzTagStageWriting) {
        return az_tag_phase_finish(app, az_tag_program_blank(app, poller));
    }
    if(app->tag_stage == AzTagStageClearing) {
        return az_tag_phase_finish(app, az_tag_program_clear(app, poller));
    }
    return az_tag_phase_finish(app, AzTagResultReadFailed);
}

/**
 * @brief Read the complete Amiibo-relevant NTAG215 page image without running Flipper's
 *        generic MFUL discovery extras.
 *
 * The stock MFUL reader probes READ_SIG, counters, tearing flags, and default passwords as part
 * of a general-purpose Ultralight scan. Amiibo physical-tag operations only need the NTAG215
 * version, live UID, authenticated pages 0-132, and the known Amiibo PWD/PACK values. Reading
 * those directly also avoids turning an unrelated READ_SIG failure into "Tag read failed".
 */
static AzTagResult az_tag_read_amiibo(AmiiboZeroApp* app, MfUltralightPoller* poller) {
    if(!app || !poller || !app->tag_poller) return AzTagResultReadFailed;

    MfUltralightVersion version;
    memset(&version, 0, sizeof(version));
    if(mf_ultralight_poller_read_version(poller, &version) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }
    if(!az_tag_version_is_ntag215(&version)) return AzTagResultWrongTag;

    MfUltralightPageReadCommandData head;
    memset(&head, 0, sizeof(head));
    if(mf_ultralight_poller_read_page(poller, 0U, &head) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }

    uint8_t raw_uid[9];
    memcpy(raw_uid, head.page, sizeof(raw_uid));
    if(!az_tag_authenticate(poller, raw_uid)) return AzTagResultAuthFailed;

    MfUltralightData* data =
        (MfUltralightData*)nfc_poller_get_data(app->tag_poller);
    if(!data) return AzTagResultReadFailed;

    memset(data->page, 0, sizeof(data->page));
    memcpy(&data->version, &version, sizeof(version));
    data->type = MfUltralightTypeNTAG215;
    data->pages_total = 135U;
    data->pages_read = 0U;
    data->auth_attempts = 0U;
    memset(&data->signature, 0, sizeof(data->signature));
    memset(data->counter, 0, sizeof(data->counter));
    az_nfc_set_tearing_flags(data);

    memcpy(&data->page[0], head.page, sizeof(head.page));
    data->pages_read = 4U;

    /* Page reads return four pages at a time. Read aligned groups through page 131. */
    for(uint16_t page = 4U; page <= 128U; page += 4U) {
        MfUltralightPageReadCommandData block;
        memset(&block, 0, sizeof(block));
        if(mf_ultralight_poller_read_page(poller, (uint8_t)page, &block) !=
           MfUltralightErrorNone) {
            return AzTagResultReadFailed;
        }
        memcpy(&data->page[page], block.page, sizeof(block.page));
        data->pages_read = (uint16_t)(page + 4U);
        app->tag_progress = (uint8_t)((data->pages_read * 95U) / 133U);
    }

    /* READ 129 yields 129-132 without relying on end-of-memory rollover. */
    MfUltralightPageReadCommandData config;
    memset(&config, 0, sizeof(config));
    if(mf_ultralight_poller_read_page(poller, 129U, &config) != MfUltralightErrorNone) {
        return AzTagResultReadFailed;
    }
    data->page[132] = config.page[3];
    data->pages_read = 133U;

    /* PWD and PACK are not readable as ordinary memory. Reconstruct the values Amiibo uses. */
    uint8_t password[4];
    az_tag_password(raw_uid, password);
    memcpy(data->page[133].data, password, sizeof(password));
    static const uint8_t pack[4] = {0x80U, 0x80U, 0x00U, 0x00U};
    memcpy(data->page[134].data, pack, sizeof(pack));

    /* Preserve the originality signature when available, but never make this optional
     * metadata a prerequisite for reading/saving or clearing a valid Amiibo. */
    MfUltralightSignature signature;
    memset(&signature, 0, sizeof(signature));
    if(mf_ultralight_poller_read_signature(poller, &signature) == MfUltralightErrorNone) {
        data->signature = signature;
    }

    /* The tail poller's ISO data is allocated with malloc and the normal MFUL state machine
     * is deliberately bypassed, so initialize it before installing the live UID/ATQA/SAK. */
    memset(data->iso14443_3a_data, 0, sizeof(*data->iso14443_3a_data));
    if(!az_nfc_set_identity(data, raw_uid)) return AzTagResultReadFailed;

    data->pages_read = 135U;
    app->tag_progress = 100U;
    return AzTagResultSuccess;
}

/** Allocate and start one physical-tag poller phase using direct MFUL commands. */
static bool az_tag_start_poller(AmiiboZeroApp* app) {
    if(!app || app->tag_poller) return false;
    app->tag_phase_done = false;
    app->tag_result = AzTagResultNone;
    app->tag_poller = nfc_poller_alloc(app->nfc, NfcProtocolMfUltralight);
    if(!app->tag_poller) return false;
    nfc_poller_start_ex(app->tag_poller, az_tag_extended_callback, app);
    return true;
}

/** Stop/free the poller after its callback returned NfcCommandStop. */
static void az_tag_release_poller(AmiiboZeroApp* app) {
    if(!app || !app->tag_poller) return;
    nfc_poller_stop(app->tag_poller);
    nfc_poller_free(app->tag_poller);
    app->tag_poller = NULL;
}

/** Build the native saved representation for one just-read retail NTAG215. */
static bool az_tag_save_read_device(AmiiboZeroApp* app, const MfUltralightData* read_data) {
    if(!app || !read_data || read_data->type != MfUltralightTypeNTAG215 ||
       read_data->pages_total < 133U || !app->keys.valid) {
        return false;
    }

    MfUltralightData* data = (MfUltralightData*)read_data;

    uint8_t raw_uid[9];
    memcpy(raw_uid, data->page, sizeof(raw_uid));
    uint8_t password[4];
    az_tag_password(raw_uid, password);
    memcpy(data->page[133].data, password, sizeof(password));
    static const uint8_t pack[4] = {0x80U, 0x80U, 0x00U, 0x00U};
    memcpy(data->page[134].data, pack, sizeof(pack));
    data->pages_total = 135U;
    data->pages_read = 135U;

    uint8_t encrypted[AZ_DUMP_SIZE];
    uint8_t plain[AZ_DUMP_SIZE];
    if(!az_nfc_export_v2_dump(app->nfc_device, encrypted) ||
       !az_decrypt_dump(encrypted, &app->keys, plain)) {
        return false;
    }

    AzFigure figure;
    memset(&figure, 0, sizeof(figure));
    memcpy(figure.id, encrypted + 84U, sizeof(figure.id));
    if(!az_db_find_by_id(app->storage, figure.id, &figure)) {
        memcpy(figure.id, encrypted + 84U, sizeof(figure.id));
        figure.category = figure.id[6];
        figure.type = figure.id[3];
        az_str_copy(figure.name, sizeof(figure.name), "Scanned Amiibo");
    }

    char path[AZ_PATH_MAX];
    path[0] = '\0';
    az_make_unique_save_path(
        app->storage,
        &figure,
        figure.name[0] ? figure.name : "Scanned Amiibo",
        path,
        sizeof(path));
    if(!path[0] || !az_nfc_save_device(app->nfc_device, path)) return false;
    const char* slash = strrchr(path, '/');
    az_str_copy(app->tag_saved_filename, sizeof(app->tag_saved_filename), slash ? slash + 1 : path);
    return true;
}

/** Begin a two-phase blank-tag write. */
bool az_tag_write_begin(AmiiboZeroApp* app, const uint8_t encrypted_dump[AZ_DUMP_SIZE]) {
    if(!app || !encrypted_dump || !app->keys.valid || app->tag_poller || app->tag_work_dump) {
        return false;
    }
    app->tag_work_dump = malloc(AZ_DUMP_SIZE);
    if(!app->tag_work_dump) return false;
    memcpy(app->tag_work_dump, encrypted_dump, AZ_DUMP_SIZE);
    memset(app->tag_target_uid, 0, sizeof(app->tag_target_uid));
    app->tag_saved_filename[0] = '\0';
    app->tag_operation = AzTagOperationWrite;
    app->tag_stage = AzTagStageScanBlank;
    app->tag_progress = 0U;
    if(!az_tag_start_poller(app)) {
        free(app->tag_work_dump);
        app->tag_work_dump = NULL;
        app->tag_operation = AzTagOperationNone;
        app->tag_stage = AzTagStageIdle;
        return false;
    }
    return true;
}

/** Begin reading one physical NTAG215 Amiibo for native saving. */
bool az_tag_read_save_begin(AmiiboZeroApp* app) {
    if(!app || !app->keys.valid || app->tag_poller || app->tag_work_dump) return false;
    app->tag_saved_filename[0] = '\0';
    app->tag_operation = AzTagOperationReadSave;
    app->tag_stage = AzTagStageReading;
    app->tag_progress = 0U;
    return az_tag_start_poller(app);
}

/** Begin read/authenticate/clear for one existing retail NTAG215 Amiibo. */
bool az_tag_clear_begin(AmiiboZeroApp* app) {
    if(!app || !app->keys.valid || app->tag_poller || app->tag_work_dump) return false;
    app->tag_work_dump = malloc(AZ_DUMP_SIZE);
    if(!app->tag_work_dump) return false;
    memset(app->tag_target_uid, 0, sizeof(app->tag_target_uid));
    app->tag_saved_filename[0] = '\0';
    app->tag_operation = AzTagOperationClear;
    app->tag_stage = AzTagStageReading;
    app->tag_progress = 0U;
    if(!az_tag_start_poller(app)) {
        free(app->tag_work_dump);
        app->tag_work_dump = NULL;
        app->tag_operation = AzTagOperationNone;
        app->tag_stage = AzTagStageIdle;
        return false;
    }
    return true;
}

/** Cancel a physical-tag operation and release all transient memory. */
void az_tag_operation_cancel(AmiiboZeroApp* app) {
    if(!app) return;
    if(app->tag_poller) az_tag_release_poller(app);
    free(app->tag_work_dump);
    app->tag_work_dump = NULL;
    app->tag_operation = AzTagOperationNone;
    app->tag_stage = AzTagStageIdle;
    app->tag_result = AzTagResultNone;
    app->tag_phase_done = false;
    app->tag_progress = 0U;
    memset(app->tag_target_uid, 0, sizeof(app->tag_target_uid));
    app->tag_saved_filename[0] = '\0';
}

/** Finalize one operation without erasing its user-visible result. */
static void az_tag_operation_done(AmiiboZeroApp* app, AzTagResult result) {
    free(app->tag_work_dump);
    app->tag_work_dump = NULL;
    app->tag_result = result;
    app->tag_stage = AzTagStageDone;
    app->tag_phase_done = false;
    app->tag_progress = result == AzTagResultSuccess ? 100U : app->tag_progress;
}

/**
 * @brief Advance physical-tag workflows outside the NFC worker thread.
 *
 * Crypto and filesystem work deliberately run here on the application/UI thread, not inside
 * the NFC worker callback, keeping the NFC thread stack limited to protocol transactions.
 */
void az_tag_operation_tick(AmiiboZeroApp* app) {
    if(!app || !app->tag_phase_done || !app->tag_poller) return;

    const AzTagStage completed_stage = app->tag_stage;
    const AzTagResult phase_result = app->tag_result;
    const MfUltralightData* read_data = NULL;
    if(completed_stage == AzTagStageReading && phase_result == AzTagResultSuccess) {
        read_data = (const MfUltralightData*)nfc_poller_get_data(app->tag_poller);
        if(read_data) nfc_device_set_data(app->nfc_device, NfcProtocolMfUltralight, read_data);
    }
    az_tag_release_poller(app);
    app->tag_phase_done = false;

    if(phase_result != AzTagResultSuccess) {
        az_tag_operation_done(app, phase_result);
        return;
    }

    if(app->tag_operation == AzTagOperationWrite && completed_stage == AzTagStageScanBlank) {
        app->tag_stage = AzTagStagePreparing;
        app->tag_result = AzTagResultNone;
        if(!az_rekey_dump_uid_to(
               app->tag_work_dump,
               &app->keys,
               app->tag_target_uid,
               app->tag_work_dump)) {
            az_tag_operation_done(app, AzTagResultCryptoFailed);
            return;
        }
        app->tag_stage = AzTagStageWriting;
        app->tag_progress = 0U;
        if(!az_tag_start_poller(app)) az_tag_operation_done(app, AzTagResultWriteFailed);
        return;
    }

    if(app->tag_operation == AzTagOperationWrite && completed_stage == AzTagStageWriting) {
        az_tag_operation_done(app, AzTagResultSuccess);
        return;
    }

    if(app->tag_operation == AzTagOperationReadSave && completed_stage == AzTagStageReading) {
        const MfUltralightData* copied =
            (const MfUltralightData*)nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
        if(!copied || copied->type != MfUltralightTypeNTAG215) {
            az_tag_operation_done(app, AzTagResultWrongTag);
        } else if(!az_tag_save_read_device(app, copied)) {
            uint8_t encrypted[AZ_DUMP_SIZE];
            uint8_t plain[AZ_DUMP_SIZE];
            if(!az_nfc_export_v2_dump(app->nfc_device, encrypted) ||
               !az_decrypt_dump(encrypted, &app->keys, plain)) {
                az_tag_operation_done(app, AzTagResultNotAmiibo);
            } else {
                az_tag_operation_done(app, AzTagResultSaveFailed);
            }
        } else {
            az_tag_operation_done(app, AzTagResultSuccess);
        }
        return;
    }

    if(app->tag_operation == AzTagOperationClear && completed_stage == AzTagStageReading) {
        const MfUltralightData* copied =
            (const MfUltralightData*)nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
        if(!copied || copied->type != MfUltralightTypeNTAG215 || copied->pages_total < 133U) {
            az_tag_operation_done(app, AzTagResultWrongTag);
            return;
        }
        const uint8_t* bytes = (const uint8_t*)copied->page;
        memcpy(app->tag_target_uid, bytes, sizeof(app->tag_target_uid));
        if(!az_clear_user_data_dump(bytes, &app->keys, app->tag_work_dump)) {
            az_tag_operation_done(app, AzTagResultNotAmiibo);
            return;
        }
        app->tag_stage = AzTagStageClearing;
        app->tag_result = AzTagResultNone;
        app->tag_progress = 0U;
        if(!az_tag_start_poller(app)) az_tag_operation_done(app, AzTagResultWriteFailed);
        return;
    }

    if(app->tag_operation == AzTagOperationClear && completed_stage == AzTagStageClearing) {
        az_tag_operation_done(app, AzTagResultSuccess);
        return;
    }

    az_tag_operation_done(app, AzTagResultReadFailed);
}
