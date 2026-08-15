/**
 * @file amiibo_nfc.cpp
 * @brief NFC device construction and Amiibo emulation.
 * @details Creates standard and version-3 tag images, implements listener behavior, validates dumps, and extracts tag metadata.
 */

#include "amiibo_nfc.h"
#include "amiibo_crypto.h"

#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_listener.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <stdio.h>
#include <string.h>

/** @brief Constant used for version-3 shift start. */
#define AZ_V3_SHIFT_START 0x80U

/** @brief Constant used for version-3 shift size. */
#define AZ_V3_SHIFT_SIZE 0x40U

/**
 * @brief Return a display label for an Amiibo type code.
 * @param type Type or event code to interpret.
 * @return Pointer to the selected text, or NULL when no text is available.
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
 * @brief Determine whether an Amiibo identifier uses the version-3 layout.
 * @param id Eight-byte Amiibo identifier.
 * @return true when the tested condition is satisfied; false otherwise.
 */
bool az_figure_is_v3(const uint8_t id[8]) {
    return id && id[7] == 0x03;
}

/** @brief Constant used for NFC cmd read. */
#define AZ_NFC_CMD_READ 0x30U

/** @brief Constant used for NFC cmd write. */
#define AZ_NFC_CMD_WRITE 0xA2U

/** @brief Constant used for NFC cmd get version. */
#define AZ_NFC_CMD_GET_VERSION 0x60U

/** @brief Constant used for NFC cmd read sig. */
#define AZ_NFC_CMD_READ_SIG 0x3CU

/** @brief Constant used for NFC cmd pwd auth. */
#define AZ_NFC_CMD_PWD_AUTH 0x1BU

/** @brief Constant used for NFC cmd fast read. */
#define AZ_NFC_CMD_FAST_READ 0x3AU

/** @brief Constant used for NFC cmd fast write. */
#define AZ_NFC_CMD_FAST_WRITE 0xA6U

/** @brief Constant used for NFC cmd sector select. */
#define AZ_NFC_CMD_SECTOR_SELECT 0xC2U

/** @brief Constant used for NFC cmd comp write. */
#define AZ_NFC_CMD_COMP_WRITE 0xA0U

/** @brief Constant used for NFC cmd read cnt. */
#define AZ_NFC_CMD_READ_CNT 0x39U

/** @brief Constant used for NFC cmd incr cnt. */
#define AZ_NFC_CMD_INCR_CNT 0xA5U

/** @brief Constant used for NFC cmd check tearing. */
#define AZ_NFC_CMD_CHECK_TEARING 0x3EU

/** @brief Constant used for NFC cmd vcsl. */
#define AZ_NFC_CMD_VCSL 0x4BU

/** @brief Constant used for NFC cmd auth. */
#define AZ_NFC_CMD_AUTH 0x1AU

/** @brief Constant used for NFC ack. */
#define AZ_NFC_ACK 0x0AU

/** @brief Constant used for NFC nak. */
#define AZ_NFC_NAK 0x00U

/**
 * @brief Apply a standard Amiibo UID and ISO 14443-3A identity to tag data.
 * @param data Data buffer or tag state used by the operation.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 * @return true on success; false if the operation cannot be completed.
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
 * @brief Apply a version-3 UID, identity bytes, password, and PACK to tag data.
 * @param data Data buffer or tag state used by the operation.
 * @param uid7 Seven-byte NFC UID buffer.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_nfc_set_identity_v3(MfUltralightData* data, const uint8_t uid7[7]) {
    if(!data || !uid7 || !mf_ultralight_set_uid(data, uid7, 7U)) return false;

    data->iso14443_3a_data->atqa[0] = 0x44;
    data->iso14443_3a_data->atqa[1] = 0x00;
    data->iso14443_3a_data->sak = 0x00;
    memcpy(data->page[0].data, uid7, 7U);
    data->page[1].data[3] = data->iso14443_3a_data->sak;
    data->page[2].data[0] = data->iso14443_3a_data->atqa[0];
    data->page[2].data[1] = data->iso14443_3a_data->atqa[1];

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
 * @brief Reset all Ultralight tearing flags to their default state.
 * @param data Data buffer or tag state used by the operation.
 */
static void az_nfc_set_tearing_flags(MfUltralightData* data) {
    for(size_t i = 0; i < MF_ULTRALIGHT_TEARING_FLAG_NUM; i++) {
        data->tearing_flag[i].data = MF_ULTRALIGHT_TEARING_FLAG_DEFAULT;
    }
}

/**
 * @brief Install a standard Amiibo dump into an NTAG215 memory image.
 * @param data Data buffer or tag state used by the operation.
 * @param dump Amiibo dump bytes.
 * @param raw_uid Nine-byte raw Amiibo UID buffer.
 * @return true on success; false if the operation cannot be completed.
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
 * @brief Expand standard Amiibo memory into the version-3 page layout.
 * @param standard Standard NTAG215 memory image.
 * @param data Data buffer or tag state used by the operation.
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

    static const uint8_t page_e2[4] = {0x01, 0x00, 0xFF, 0x00};
    static const uint8_t page_e3[4] = {0x00, 0x00, 0x00, 0x04};
    static const uint8_t page_e4[4] = {0x07, 0x00, 0x00, 0x00};
    static const uint8_t page_e7[4] = {0x08, 0x00, 0x00, 0x00};
    static const uint8_t page_e8[4] = {0x01, 0x00, 0xF8, 0x48};
    static const uint8_t page_e9[4] = {0x08, 0x01, 0x00, 0x00};
    static const uint8_t page_ec[4] = {0x41, 0x00, 0xF8, 0x48};
    static const uint8_t page_ed[4] = {0x08, 0x01, 0x21, 0x00};
    memcpy(sector0 + 0x388, page_e2, sizeof(page_e2));
    memcpy(sector0 + 0x38C, page_e3, sizeof(page_e3));
    memcpy(sector0 + 0x390, page_e4, sizeof(page_e4));
    memset(sector0 + 0x394, 0, (0xECU - 0xE5U) * 4U);
    memcpy(sector0 + 0x39C, page_e7, sizeof(page_e7));
    memcpy(sector0 + 0x3A0, page_e8, sizeof(page_e8));
    memcpy(sector0 + 0x3A4, page_e9, sizeof(page_e9));
    memcpy(data->page[234].data, page_ec, sizeof(page_ec));
    memcpy(data->page[235].data, page_ed, sizeof(page_ed));
}

/**
 * @brief Install a generated Amiibo dump into a version-3 tag memory image.
 * @param data Data buffer or tag state used by the operation.
 * @param dump Amiibo dump bytes.
 * @param uid7 Seven-byte NFC UID buffer.
 * @return true on success; false if the operation cannot be completed.
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
 * @brief Extract the standard Amiibo dump bytes from tag memory.
 * @param data Data buffer or tag state used by the operation.
 * @param out_dump Destination for the resulting Amiibo dump.
 * @return true on success; false if the operation cannot be completed.
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
 * @brief Populate an NFC device with a newly generated Amiibo tag image.
 * @param device NFC device to inspect or modify.
 * @param figure Figure metadata.
 * @param keys Loaded Amiibo key material.
 * @return true on success; false if the operation cannot be completed.
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
 * @brief Determine whether an NFC device contains the version-3 tag layout.
 * @param device NFC device to inspect or modify.
 * @return true when the tested condition is satisfied; false otherwise.
 */
bool az_nfc_device_is_v3(const NfcDevice* device) {
    if(!az_nfc_validate_amiibo(device)) return false;
    const MfUltralightData* data =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    return data && data->type == MfUltralightTypeNTAGI2CPlus2K;
}

/** @brief Constant used for version-3 maximum response bytes. */
#define AZ_V3_MAX_RESPONSE_BYTES 256U

/** @brief Constant used for version-3 tx capacity. */
#define AZ_V3_TX_CAPACITY (AZ_V3_MAX_RESPONSE_BYTES + 2U)

/** @brief Constant used for version-3 dynamic lock page. */
#define AZ_V3_DYNAMIC_LOCK_PAGE 0xE2U

/** @brief Constant used for version-3 config page. */
#define AZ_V3_CONFIG_PAGE 0xE3U

/** @brief Constant used for version-3 pwd page. */
#define AZ_V3_PWD_PAGE 0xE5U

/** @brief Constant used for version-3 pack page. */
#define AZ_V3_PACK_PAGE 0xE6U

/** @brief Constant used for version-3 session page. */
#define AZ_V3_SESSION_PAGE 0xECU

/** @brief Constant used for version-3 ns reg page. */
#define AZ_V3_NS_REG_PAGE 0xEDU

/** @brief Constant used for version-3 SRAM first page. */
#define AZ_V3_SRAM_FIRST_PAGE 0xF0U

/** @brief Constant used for version-3 SRAM last page. */
#define AZ_V3_SRAM_LAST_PAGE 0xFFU

/** @brief Constant used for version-3 mirror session page. */
#define AZ_V3_MIRROR_SESSION_PAGE 0xF8U

/** @brief Constant used for version-3 mirror ns reg page. */
#define AZ_V3_MIRROR_NS_REG_PAGE 0xF9U

/** @brief Internal outcome of a version-3 NFC command before listener post-processing. */
typedef enum {
    AzV3CommandNotFound, /**< Received command is not handled by the version-3 dispatcher. */
    AzV3CommandProcessed, /**< Command completed with a normal response. */
    AzV3CommandProcessedAck, /**< Command completed and requires an ACK response. */
    AzV3CommandProcessedSilent, /**< Command completed without transmitting a response. */
    AzV3CommandNotProcessedNak, /**< Command was rejected with a generic NAK. */
    AzV3CommandNotProcessedAuthNak, /**< Command was rejected because authentication requirements were not met. */
} AzV3CommandResult;

/**
 * @brief Calculate the ISO 14443-A CRC for an NFC response.
 * @param data Data buffer or tag state used by the operation.
 * @param length Number of bytes to process.
 * @return The calculated ISO 14443-A CRC value.
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
 * @brief Transmit a version-3 response with an appended ISO 14443-A CRC.
 * @param app Application state.
 * @param data Data buffer or tag state used by the operation.
 * @param size Number of bytes in the supplied buffer or file.
 * @return true on success; false if the operation cannot be completed.
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
 * @brief Transmit a one-byte version-3 ACK or NAK response.
 * @param app Application state.
 * @param value Value to process or transmit.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_v3_send_short(AmiiboZeroApp* app, uint8_t value) {
    if(!app || !app->v3_tx_buffer) return false;
    bit_buffer_set_size(app->v3_tx_buffer, 4U);
    bit_buffer_set_byte(app->v3_tx_buffer, 0, (uint8_t)(value & 0x0FU));
    return nfc_listener_tx(app->nfc, app->v3_tx_buffer) == NfcErrorNone;
}

/**
 * @brief Return mutable Ultralight data for the current emulated NFC device.
 * @param app Application state.
 * @return Pointer to the requested object, or NULL when it is not available.
 */
static MfUltralightData* az_v3_data(AmiiboZeroApp* app) {
    if(!app || !app->nfc_device) return NULL;
    return (MfUltralightData*)nfc_device_get_data(app->nfc_device, NfcProtocolMfUltralight);
}

/**
 * @brief Check whether a version-3 page index is addressable.
 * @param app Application state.
 * @param page Logical tag page number.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_v3_page_valid(const AmiiboZeroApp* app, uint16_t page) {
    if(!app) return false;
    switch(app->v3_sector) {
    case 0:
        return page <= 0xE9U || (page >= AZ_V3_SESSION_PAGE && page <= AZ_V3_NS_REG_PAGE) ||
               (page >= AZ_V3_SRAM_FIRST_PAGE && page <= AZ_V3_SRAM_LAST_PAGE);
    case 1:
        return page <= 0xFFU;
    case 2:
        return false;
    case 3:
        return page >= AZ_V3_MIRROR_SESSION_PAGE && page <= AZ_V3_MIRROR_NS_REG_PAGE;
    default:
        return false;
    }
}

/**
 * @brief Check whether a version-3 inclusive page range is addressable.
 * @param app Application state.
 * @param start_page First logical page in the requested range.
 * @param end_page Last logical page in the requested range.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_v3_range_valid(const AmiiboZeroApp* app, uint16_t start_page, uint16_t end_page) {
    if(!app) return false;
    switch(app->v3_sector) {
    case 0:
        if(start_page <= 0xE9U && end_page <= 0xE9U) return true;
        if(start_page >= AZ_V3_SESSION_PAGE && end_page <= AZ_V3_NS_REG_PAGE) return true;
        return start_page >= AZ_V3_SRAM_FIRST_PAGE && end_page <= AZ_V3_SRAM_LAST_PAGE;
    case 1:
        return start_page <= 0xFFU && end_page <= 0xFFU;
    case 2:
        return false;
    case 3:

        return start_page >= AZ_V3_MIRROR_SESSION_PAGE &&
               start_page <= AZ_V3_MIRROR_NS_REG_PAGE;
    default:
        return false;
    }
}

/**
 * @brief Translate a version-3 logical page to the backing Ultralight page.
 * @param app Application state.
 * @param page Logical tag page number.
 * @param native_page Destination for the translated backing-page number.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_v3_native_page(const AmiiboZeroApp* app, uint16_t page, uint16_t* native_page) {
    if(!app || !native_page || !az_v3_page_valid(app, page)) return false;

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
        return false;
    case 1:
        *native_page = (uint16_t)(236U + page);
        return true;
    case 3:
        *native_page = page == AZ_V3_MIRROR_SESSION_PAGE ? 234U : 235U;
        return true;
    default:
        return false;
    }
}

/**
 * @brief Determine whether a logical page targets the version-3 NS register window.
 * @param app Application state.
 * @param page Logical tag page number.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_v3_is_ns_reg(const AmiiboZeroApp* app, uint16_t page) {
    return app && ((app->v3_sector == 0 && page == AZ_V3_NS_REG_PAGE) ||
                   (app->v3_sector == 3 && page == AZ_V3_MIRROR_NS_REG_PAGE));
}

/**
 * @brief Read one logical version-3 page into a four-byte output buffer.
 * @param app Application state.
 * @param data Data buffer or tag state used by the operation.
 * @param page Logical tag page number.
 * @param out Destination for the computed result.
 */
static void az_v3_read_page(
    AmiiboZeroApp* app,
    const MfUltralightData* data,
    uint16_t page,
    uint8_t out[4]) {
    memset(out, 0, 4U);
    if(!app || !data || !az_v3_page_valid(app, page)) return;

    if(app->v3_sector == 0 && page >= AZ_V3_SRAM_FIRST_PAGE && page <= AZ_V3_SRAM_LAST_PAGE) {
        const size_t offset = (size_t)(page - AZ_V3_SRAM_FIRST_PAGE) * 4U;
        memcpy(out, app->current_lockon_sram + offset, 4U);
        return;
    }

    if(page == AZ_V3_PWD_PAGE || page == AZ_V3_PACK_PAGE) return;

    uint16_t native_page = 0;
    if(!az_v3_native_page(app, page, &native_page) || native_page >= data->pages_total) return;
    memcpy(out, data->page[native_page].data, 4U);

    if(az_v3_is_ns_reg(app, page)) {
        if(app->v3_sram_ready)
            out[2] |= 0x08U;
        else
            out[2] &= (uint8_t)~0x08U;
    }
}

/**
 * @brief Check version-3 read or write access against authentication settings.
 * @param app Application state.
 * @param data Data buffer or tag state used by the operation.
 * @param start_page First logical page in the requested range.
 * @param write_op Whether the access check is for a write operation.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_v3_check_access(
    const AmiiboZeroApp* app,
    const MfUltralightData* data,
    uint16_t start_page,
    bool write_op) {
    if(!app || !data) return false;
    const MfUltralightConfigPages* config =
        (const MfUltralightConfigPages*)&data->page[AZ_V3_CONFIG_PAGE];

    if(!app->v3_authenticated && config->auth0 <= start_page && (config->access.prot || write_op)) {
        return false;
    }
    if(config->access.cfglck && write_op) {

        const uint16_t config_page_start = (uint16_t)(data->pages_total - 4U);
        if(start_page == config_page_start || start_page == config_page_start + 1U) return false;
    }
    return true;
}

/**
 * @brief Check whether a page is protected by static lock bits.
 * @param data Data buffer or tag state used by the operation.
 * @param page Logical tag page number.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_v3_static_page_locked(const MfUltralightData* data, uint16_t page) {
    if(!data || page < 3U || page > 15U) return false;
    const uint16_t locks = (uint16_t)data->page[2].data[2] |
                           ((uint16_t)data->page[2].data[3] << 8);
    return (locks & (uint16_t)(1U << page)) != 0U;
}

/**
 * @brief Check whether a page is protected by dynamic lock bits.
 * @param data Data buffer or tag state used by the operation.
 * @param page Logical tag page number.
 * @param sector Logical sector number.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_v3_dynamic_page_locked(
    const MfUltralightData* data,
    uint16_t page,
    uint8_t sector) {
    if(!data) return false;
    const uint16_t linear_page = (uint16_t)(page + (uint16_t)sector * 256U);
    if(linear_page < 16U || linear_page > 511U) return false;
    const uint8_t bit = (uint8_t)((linear_page - 16U) / 32U);
    const uint16_t locks = (uint16_t)data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[0] |
                           ((uint16_t)data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[1] << 8);
    return (locks & (uint16_t)(1U << bit)) != 0U;
}

/**
 * @brief Apply one-way updates to the static lock bytes.
 * @param data Data buffer or tag state used by the operation.
 * @param payload Four-byte page or command payload.
 */
static void az_v3_write_static_locks(MfUltralightData* data, const uint8_t payload[4]) {
    uint16_t current = (uint16_t)data->page[2].data[2] |
                       ((uint16_t)data->page[2].data[3] << 8);
    uint16_t incoming = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);

    if(current & (1U << 3)) incoming &= (uint16_t)~(1U << 3);
    if(current & 0x03F0U) incoming &= (uint16_t)~0x03F0U;
    if(current & 0xFC00U) incoming &= (uint16_t)~0xFC00U;
    current |= incoming;
    data->page[2].data[2] = (uint8_t)current;
    data->page[2].data[3] = (uint8_t)(current >> 8);
}

/**
 * @brief Apply one-way updates to the dynamic lock bytes.
 * @param data Data buffer or tag state used by the operation.
 * @param payload Four-byte page or command payload.
 */
static void az_v3_write_dynamic_locks(MfUltralightData* data, const uint8_t payload[4]) {
    uint32_t current = (uint32_t)data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[0] |
                       ((uint32_t)data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[1] << 8) |
                       ((uint32_t)data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[2] << 16) |
                       ((uint32_t)data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[3] << 24);
    uint32_t incoming = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                        ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    incoming &= 0x00FFFFFFUL;

    for(uint8_t i = 0; i < 8U; i++) {
        if(current & (1UL << (i + 16U))) {
            incoming &= ~(3UL << (i * 2U));
        }
    }
    current |= incoming;
    data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[0] = (uint8_t)current;
    data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[1] = (uint8_t)(current >> 8);
    data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[2] = (uint8_t)(current >> 16);
    data->page[AZ_V3_DYNAMIC_LOCK_PAGE].data[3] = (uint8_t)(current >> 24);
}

/**
 * @brief Apply a version-3 page write while enforcing lock and register behavior.
 * @param app Application state.
 * @param data Data buffer or tag state used by the operation.
 * @param page Logical tag page number.
 * @param payload Four-byte page or command payload.
 * @return The computed result value.
 */
static AzV3CommandResult az_v3_write_page(
    AmiiboZeroApp* app,
    MfUltralightData* data,
    uint16_t page,
    const uint8_t payload[4]) {
    if(!app || !data || !payload || !az_v3_page_valid(app, page)) return AzV3CommandNotProcessedNak;
    if(!az_v3_check_access(app, data, page, true)) return AzV3CommandNotProcessedNak;
    if(az_v3_static_page_locked(data, page) ||
       az_v3_dynamic_page_locked(data, page, app->v3_sector)) {
        return AzV3CommandNotProcessedNak;
    }

    if(app->v3_sector == 0 && page < 2U) return AzV3CommandNotProcessedNak;
    if(app->v3_sector == 0 && page == 2U) {
        az_v3_write_static_locks(data, payload);
        return AzV3CommandProcessedAck;
    }
    if(app->v3_sector == 0 && page == 3U) {
        for(size_t i = 0; i < 4U; i++) data->page[3].data[i] |= payload[i];
        return AzV3CommandProcessedAck;
    }
    if(app->v3_sector == 0 && page == AZ_V3_DYNAMIC_LOCK_PAGE) {
        az_v3_write_dynamic_locks(data, payload);
        return AzV3CommandProcessedAck;
    }
    if(app->v3_sector == 0 && page >= AZ_V3_SRAM_FIRST_PAGE && page <= AZ_V3_SRAM_LAST_PAGE) {

        return AzV3CommandProcessedAck;
    }

    uint16_t native_page = 0;
    if(!az_v3_native_page(app, page, &native_page) || native_page >= data->pages_total) {
        return AzV3CommandNotProcessedNak;
    }
    memcpy(data->page[native_page].data, payload, 4U);
    return AzV3CommandProcessedAck;
}

/**
 * @brief Determine whether configured authentication blocks the current operation.
 * @param data Data buffer or tag state used by the operation.
 * @param config Ultralight configuration-page state.
 * @param auth_success Whether authentication has succeeded for the current interaction.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_v3_auth_locked(
    MfUltralightData* data,
    const MfUltralightConfigPages* config,
    bool auth_success) {
    if(!data || !config || config->access.authlim == 0U) return false;
    const uint32_t limit = 1UL << config->access.authlim;
    if(data->auth_attempts >= limit) return true;
    if(auth_success)
        data->auth_attempts = 0U;
    else
        data->auth_attempts++;
    return data->auth_attempts >= limit;
}

/**
 * @brief Decode and execute one received version-3 NFC command.
 * @param app Application state.
 * @param data Data buffer or tag state used by the operation.
 * @param rx Received NFC command bytes.
 * @param size Number of bytes in the supplied buffer or file.
 * @return The computed result value.
 */
static AzV3CommandResult az_v3_dispatch_command(
    AmiiboZeroApp* app,
    MfUltralightData* data,
    const uint8_t* rx,
    size_t size) {
    if(!app || !data || !rx || size == 0U) return AzV3CommandNotFound;

    if(app->v3_sector_select_pending) {
        app->v3_sector_select_pending = false;
        if(size != 4U || rx[0] == 0xFFU) return AzV3CommandNotProcessedNak;
        app->v3_sector = rx[0];
        return AzV3CommandProcessedSilent;
    }

    switch(rx[0]) {
    case AZ_NFC_CMD_READ: {
        if(size != 2U) return AzV3CommandNotFound;
        const uint16_t start_page = rx[1];
        if(!az_v3_page_valid(app, start_page) || !az_v3_check_access(app, data, start_page, false)) {
            return AzV3CommandNotProcessedNak;
        }

        uint8_t response[16];
        for(uint16_t i = 0; i < 4U; i++) az_v3_read_page(app, data, start_page + i, response + i * 4U);
        az_v3_send_standard(app, response, sizeof(response));
        return AzV3CommandProcessed;
    }

    case AZ_NFC_CMD_FAST_READ: {
        if(size != 3U) return AzV3CommandNotFound;
        const uint16_t start_page = rx[1];
        const uint16_t end_page = rx[2];
        if(end_page < start_page || !az_v3_range_valid(app, start_page, end_page) ||
           !az_v3_check_access(app, data, start_page, false) ||
           !az_v3_check_access(app, data, end_page, false)) {
            return AzV3CommandNotProcessedNak;
        }

        const uint16_t page_count = (uint16_t)(end_page - start_page + 1U);
        if(page_count > 64U) return AzV3CommandNotProcessedNak;

        uint8_t response[AZ_V3_MAX_RESPONSE_BYTES];
        for(uint16_t i = 0; i < page_count; i++) {
            az_v3_read_page(app, data, start_page + i, response + i * 4U);
        }
        az_v3_send_standard(app, response, (size_t)page_count * 4U);
        return AzV3CommandProcessed;
    }

    case AZ_NFC_CMD_WRITE:
        if(size != 6U) return AzV3CommandNotFound;
        return az_v3_write_page(app, data, rx[1], rx + 2U);

    case AZ_NFC_CMD_FAST_WRITE:
        if(size != 67U) return AzV3CommandNotFound;
        if(rx[1] != AZ_V3_SRAM_FIRST_PAGE || rx[2] != AZ_V3_SRAM_LAST_PAGE) {
            return AzV3CommandNotProcessedNak;
        }

        app->v3_sram_ready = true;
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
        MfUltralightConfigPages* config = (MfUltralightConfigPages*)&data->page[AZ_V3_CONFIG_PAGE];
        const bool success = memcmp(config->password.data, rx + 1U, 4U) == 0;
        if(az_v3_auth_locked(data, config, success)) return AzV3CommandNotProcessedAuthNak;
        if(!success) return AzV3CommandNotProcessedNak;

        app->v3_authenticated = true;
        az_v3_send_standard(app, config->pack.data, sizeof(config->pack.data));
        return AzV3CommandProcessed;
    }

    case AZ_NFC_CMD_SECTOR_SELECT:
        if(size != 2U) return AzV3CommandNotFound;
        app->v3_sector_select_pending = true;
        return AzV3CommandProcessedAck;

    case AZ_NFC_CMD_COMP_WRITE:
        if(size != 2U) return AzV3CommandNotFound;
        return AzV3CommandProcessedSilent;
    case AZ_NFC_CMD_READ_CNT:
        if(size != 2U) return AzV3CommandNotFound;
        return AzV3CommandNotProcessedNak;
    case AZ_NFC_CMD_CHECK_TEARING:
        if(size != 2U) return AzV3CommandNotFound;
        return AzV3CommandNotProcessedNak;
    case AZ_NFC_CMD_INCR_CNT:
        if(size != 6U) return AzV3CommandNotFound;
        return AzV3CommandProcessedSilent;
    case AZ_NFC_CMD_VCSL:
        if(size != 21U) return AzV3CommandNotFound;
        return AzV3CommandProcessedSilent;
    case AZ_NFC_CMD_AUTH:
        if(size != 2U) return AzV3CommandNotFound;
        return AzV3CommandNotProcessedNak;

    default:
        return AzV3CommandNotFound;
    }
}

/**
 * @brief Convert a version-3 command result into listener control flow and output.
 * @param app Application state.
 * @param result Internal version-3 command result.
 * @return The computed result value.
 */
static NfcCommand az_v3_postprocess(AmiiboZeroApp* app, AzV3CommandResult result) {
    if(result == AzV3CommandProcessedAck) {
        az_v3_send_short(app, AZ_NFC_ACK);
        return NfcCommandContinue;
    }
    if(result == AzV3CommandProcessedSilent) return NfcCommandReset;
    if(result == AzV3CommandProcessed) return NfcCommandContinue;

    app->v3_authenticated = false;
    if(result == AzV3CommandNotProcessedNak)
        az_v3_send_short(app, AZ_NFC_NAK);
    else if(result == AzV3CommandNotProcessedAuthNak)
        az_v3_send_short(app, 0x04U);

    return NfcCommandSleep;
}

/**
 * @brief Reset version-3 transient listener state for a new interaction.
 * @param app Application state.
 * @return The computed result value.
 */
static NfcCommand az_v3_reset_listener_state(AmiiboZeroApp* app) {
    app->v3_sector_select_pending = false;
    app->v3_sector = 0U;
    app->v3_authenticated = false;
    app->v3_sram_ready = false;
    return NfcCommandSleep;
}

/**
 * @brief Handle NFC listener events for version-3 emulation.
 * @param event NFC event to handle.
 * @param context Caller-owned callback context.
 * @return The computed result value.
 */
static NfcCommand az_v3_listener_callback(NfcGenericEvent event, void* context) {
    auto* app = static_cast<AmiiboZeroApp*>(context);
    if(!app || event.protocol != NfcProtocolIso14443_3a || !event.event_data) return NfcCommandStop;

    const auto* iso_event = static_cast<const Iso14443_3aListenerEvent*>(event.event_data);
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

    if(iso_event->type == Iso14443_3aListenerEventTypeReceivedData ||
       iso_event->type == Iso14443_3aListenerEventTypeFieldOff ||
       iso_event->type == Iso14443_3aListenerEventTypeHalted) {
        return az_v3_reset_listener_state(app);
    }
    return NfcCommandContinue;
}

/**
 * @brief Start NFC emulation for the application's current device.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
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

    app->v3_sector = 0U;
    app->v3_sector_select_pending = false;
    app->v3_authenticated = false;
    app->v3_sram_ready = false;
    nfc_listener_start(app->listener, az_v3_listener_callback, app);
    app->v3_i2c_listener = true;
    app->emulating = true;
    return true;
}

/**
 * @brief Stop active emulation and synchronize mutable tag state back to the device.
 * @param app Application state.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_listener_pause_and_sync(AmiiboZeroApp* app) {
    if(!app || !app->emulating || !app->listener) return false;

    if(app->v3_i2c_listener) {
        nfc_listener_stop(app->listener);
        nfc_listener_free(app->listener);
        app->listener = NULL;
        if(app->v3_tx_buffer) bit_buffer_free(app->v3_tx_buffer);
        app->v3_tx_buffer = NULL;
        app->v3_i2c_listener = false;
        app->v3_sector_select_pending = false;
        app->v3_sector = 0U;
        app->v3_authenticated = false;
        app->v3_sram_ready = false;
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
 * @brief Replace the UID of the current Amiibo image while preserving encrypted content.
 * @param device NFC device to inspect or modify.
 * @param keys Loaded Amiibo key material.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_randomize_uid(NfcDevice* device, const AzKeys* keys) {
    if(!device || !keys || !keys->valid || !az_nfc_validate_amiibo(device)) return false;
    MfUltralightData* data =
        (MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    if(!data) return false;

    uint8_t old_dump[AZ_DUMP_SIZE];
    uint8_t new_dump[AZ_DUMP_SIZE];
    if(!az_nfc_extract_standard_dump(data, old_dump)) return false;

    if(data->type == MfUltralightTypeNTAG215) {
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

    uint8_t new_uid7[7];
    if(!az_rekey_v3_dump_uid(old_dump, keys, new_dump, new_uid7)) return false;
    uint8_t* bytes = (uint8_t*)data->page;
    memcpy(bytes, new_dump, AZ_V3_SHIFT_START);
    memcpy(
        bytes + AZ_V3_SHIFT_START + AZ_V3_SHIFT_SIZE,
        new_dump + AZ_V3_SHIFT_START,
        AZ_DUMP_SIZE - AZ_V3_SHIFT_START);
    return az_nfc_set_identity_v3(data, new_uid7);
}

/**
 * @brief Convert big-endian UTF-16 code units into a bounded UTF-8 string.
 * @param source Source object or buffer.
 * @param code_units Number of UTF-16 code units to decode.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 */
static void az_utf16be_to_utf8(
    const uint8_t* source,
    size_t code_units,
    char* out,
    size_t out_size) {
    if(!out || out_size == 0) return;
    out[0] = '\0';
    if(!source) return;
    size_t p = 0;
    for(size_t i = 0; i < code_units; i++) {
        uint16_t cp = (uint16_t)(((uint16_t)source[i * 2] << 8) | source[i * 2 + 1]);
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

/**
 * @brief Format a packed Amiibo date field for display.
 * @param bytes Input byte sequence.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
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
 * @brief Read an unsigned 64-bit big-endian integer.
 * @param bytes Input byte sequence.
 * @return The decoded unsigned 64-bit value.
 */
static uint64_t az_read_be64(const uint8_t bytes[8]) {
    uint64_t value = 0;
    for(size_t i = 0; i < 8; i++) value = (value << 8) | bytes[i];
    return value;
}

/**
 * @brief Decode user-facing Amiibo metadata from an NFC device.
 * @param device NFC device to inspect or modify.
 * @param keys Loaded Amiibo key material.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
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

    out->available = true;
    out->initialized = (plain[0x14] & 0x10U) != 0;
    out->app_data_initialized = (plain[0x14] & 0x20U) != 0;
    out->write_counter = (uint16_t)(((uint16_t)plain[0x11] << 8) | plain[0x12]);
    az_format_amiibo_date(plain + 0x18, out->init_date, sizeof(out->init_date));
    az_format_amiibo_date(plain + 0x1A, out->write_date, sizeof(out->write_date));
    if(out->initialized) {
        az_utf16be_to_utf8(plain + 0x20, 10, out->nickname, sizeof(out->nickname));
        az_utf16be_to_utf8(plain + 0xBA, 10, out->owner_mii, sizeof(out->owner_mii));
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
 * @brief Check whether an NFC device has a supported Amiibo memory layout.
 * @param device NFC device to inspect or modify.
 * @return true when the tested condition is satisfied; false otherwise.
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
 * @brief Extract the Amiibo figure identifier from an NFC device.
 * @param device NFC device to inspect or modify.
 * @param out_id Destination for the resulting id.
 * @return true on success; false if the operation cannot be completed.
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
 * @brief Persist an NFC device image to a file path.
 * @param device NFC device to inspect or modify.
 * @param path Filesystem path.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_save_device(NfcDevice* device, const char* path) {
    return device && path && nfc_device_save(device, path);
}

/**
 * @brief Load an NFC device image from a file path.
 * @param device NFC device to inspect or modify.
 * @param path Filesystem path.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_nfc_load_device(NfcDevice* device, const char* path) {
    if(!device || !path || !nfc_device_load(device, path)) return false;
    return az_nfc_validate_amiibo(device);
}
