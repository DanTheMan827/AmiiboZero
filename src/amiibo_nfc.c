/**
 * @file amiibo_nfc.c
 * @brief Conversion between encrypted Amiibo dumps and Flipper native NTAG215 NFC data.
 */

#include "./amiibo_zero.h"

#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <string.h>

/**
 * @brief Generate fresh Amiibo data and install it into a Flipper NTAG215 device model.
 */
bool az_nfc_generate_device(NfcDevice* device, const AzFigure* figure, const AzKeys* keys) {
    if(!device || !figure || !keys || !keys->valid) return false;

    uint8_t dump[532];
    uint8_t raw_uid[9];
    if(!az_generate_dump(figure->id, keys, dump, raw_uid)) return false;

    MfUltralightData* data = mf_ultralight_alloc();
    if(!data) return false;

    data->type = MfUltralightTypeNTAG215;
    data->pages_total = 135;
    data->pages_read = 135;

    const uint8_t version[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
    memcpy(&data->version, version, sizeof(version));

    uint8_t uid7[7];
    az_raw_uid_to_nfc_uid(raw_uid, uid7);
    if(!mf_ultralight_set_uid(data, uid7, sizeof(uid7))) {
        mf_ultralight_free(data);
        return false;
    }
    data->iso14443_3a_data->atqa[0] = 0x44;
    data->iso14443_3a_data->atqa[1] = 0x00;
    data->iso14443_3a_data->sak = 0x00;

    memcpy(data->page, dump, sizeof(dump));
    /* AmiiTag stores the first tearing flag in dump byte 523; Flipper models
       tearing flags separately, so the physical dynamic-lock RFUI byte is 0. */
    data->page[130].data[3] = 0x00;
    for(size_t i = 0; i < MF_ULTRALIGHT_TEARING_FLAG_NUM; i++) {
        data->tearing_flag[i].data = MF_ULTRALIGHT_TEARING_FLAG_DEFAULT;
    }

    uint8_t password[4];
    az_tag_password(raw_uid, password);
    memcpy(data->page[133].data, password, sizeof(password));
    const uint8_t pack[4] = {0x80, 0x80, 0x00, 0x00};
    memcpy(data->page[134].data, pack, sizeof(pack));

    nfc_device_set_data(device, NfcProtocolMfUltralight, data);
    mf_ultralight_free(data);
    return true;
}

/**
 * @brief Validate that NFC device data has the expected NTAG215 Amiibo shape.
 */
bool az_nfc_validate_amiibo(const NfcDevice* device) {
    if(!device || nfc_device_get_protocol(device) != NfcProtocolMfUltralight) return false;
    const MfUltralightData* data =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    return data && data->type == MfUltralightTypeNTAG215 && data->pages_total >= 133;
}

/**
 * @brief Extract the eight-byte figure ID from an Amiibo NFC device.
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
 * @brief Load NFC device data from a native Flipper .nfc file.
 */
bool az_nfc_load_device(NfcDevice* device, const char* path) {
    if(!device || !path || !nfc_device_load(device, path)) return false;
    return az_nfc_validate_amiibo(device);
}
