/**
 * @file amiibo_storage.c
 * @brief Persistent file and Lock-On storage helpers.
 * @details Scans saved tags and Lock-On payloads, validates companion data, and manages file naming, rename, and deletion operations.
 */

#include "amiibo_storage.h"
#include "amiibo_nfc.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/**
 * @brief Refresh presence information for the external application data files.
 * @param storage Storage service used for file operations.
 * @param out Destination status structure.
 */
void az_storage_check_data_files(Storage* storage, AzDataFiles* out) {
    if(!out) return;
    memset(out, 0, sizeof(*out));
    if(!storage) return;

    out->key_retail = storage_file_exists(storage, AZ_KEYS_FILE);
    out->amiibo_json = storage_file_exists(storage, AZ_AMIIBO_JSON);
    out->games_json = storage_file_exists(storage, AZ_GAMES_JSON);
}

/**
 * @brief Return whether every required external data file is present.
 * @param files Data-file presence state to inspect.
 * @return true when key_retail.bin and amiibo.json are both present.
 */
bool az_storage_required_data_present(const AzDataFiles* files) {
    return files && files->key_retail && files->amiibo_json;
}

/**
 * @brief Build the absolute figures-directory path for a saved filename.
 * @param filename Filename relative to the relevant application data directory.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_saved_full_path(const char* filename, char* out, size_t out_size);

/**
 * @brief Test whether a byte is an ASCII letter or digit.
 * @param c ASCII character or byte to inspect.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_ascii_isalnum(unsigned char c) {
    return ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'Z')) ||
           ((c >= 'a') && (c <= 'z'));
}

/**
 * @brief Append a string to a bounded NUL-terminated buffer.
 * @param dst Destination string buffer.
 * @param dst_size Capacity of the destination string buffer, including the terminator.
 * @param src Source string; NULL is treated as empty.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_append(char* dst, size_t dst_size, const char* src) {
    if(!dst || !src || dst_size == 0) return false;

    size_t used = strlen(dst);
    if(used >= dst_size) {
        dst[dst_size - 1] = '\0';
        return false;
    }

    const size_t available = dst_size - used;
    size_t len = strlen(src);
    if(len >= available) {
        len = available - 1;
        if(len > 0) memcpy(dst + used, src, len);
        dst[used + len] = '\0';
        return false;
    }

    memcpy(dst + used, src, len + 1);
    return true;
}

/**
 * @brief Assemble a saved-figure path from a sanitized name, identifier, and suffix.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 * @param safe_name Sanitized filename component.
 * @param id_hex Canonical hexadecimal Amiibo identifier.
 * @param suffix Filename suffix to append.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_build_save_path(
    char* out,
    size_t out_size,
    const char* safe_name,
    const char* id_hex,
    const char* suffix) {
    if(!out || out_size == 0) return false;
    out[0] = '\0';
    return az_append(out, out_size, AZ_FIGURES_DIR) && az_append(out, out_size, "/") &&
           az_append(out, out_size, safe_name) && az_append(out, out_size, "_") &&
           az_append(out, out_size, id_hex) && az_append(out, out_size, suffix);
}

/**
 * @brief Check whether a filename has the expected NFC file extension.
 * @param name Display name.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_has_nfc_extension(const char* name) {
    const char* dot = strrchr(name, '.');
    if(!dot) return false;
    return strcasecmp(dot, ".nfc") == 0;
}

/**
 * @brief Compare saved-figure entries for catalog ordering.
 * @param a Left comparison operand.
 * @param b Right comparison operand.
 * @return A negative, zero, or positive value when the left operand sorts before, equal to, or after the right operand.
 */
static int az_saved_compare(const AzSavedEntry* a, const AzSavedEntry* b) {
    return strcasecmp(a->display_name, b->display_name);
}

/**
 * @brief Sort saved-figure catalog entries.
 * @param entries Entry array to sort or update.
 * @param count Number of records or elements.
 */
static void az_saved_sort(AzSavedEntry* entries, uint16_t count) {
    for(uint16_t i = 1; i < count; i++) {
        AzSavedEntry current = entries[i];
        uint16_t j = i;
        while(j > 0 && az_saved_compare(&entries[j - 1], &current) > 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        if(j != i) entries[j] = current;
    }
}

/**
 * @brief Compare Lock-On entries for catalog ordering.
 * @param a Left comparison operand.
 * @param b Right comparison operand.
 * @return A negative, zero, or positive value when the left operand sorts before, equal to, or after the right operand.
 */
static int az_lockon_compare(const AzLockOnEntry* a, const AzLockOnEntry* b) {
    return strcasecmp(a->display_name, b->display_name);
}

/**
 * @brief Sort Lock-On catalog entries.
 * @param entries Entry array to sort or update.
 * @param count Number of records or elements.
 */
static void az_lockon_sort(AzLockOnEntry* entries, uint16_t count) {
    for(uint16_t i = 1; i < count; i++) {
        AzLockOnEntry current = entries[i];
        uint16_t j = i;
        while(j > 0 && az_lockon_compare(&entries[j - 1], &current) > 0) {
            entries[j] = entries[j - 1];
            j--;
        }
        if(j != i) entries[j] = current;
    }
}

/**
 * @brief Calculate the CRC-16 used by Lock-On payload files.
 * @param data Data buffer or tag state used by the operation.
 * @param length Number of bytes to process.
 * @return The computed result value.
 */
static uint16_t az_lockon_crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFFU;
    for(size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for(uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0x8408U) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/**
 * @brief Check whether a Lock-On file size is within the supported payload range.
 * @param size Number of bytes in the supplied buffer or file.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_lockon_size_valid(uint64_t size) {
    return (size >= 1U && size <= AZ_LOCKON_PAYLOAD_MAX) || size == AZ_LOCKON_SRAM_SIZE;
}

/**
 * @brief Create the application data directories required for persistent storage.
 * @param storage Storage service used for file operations.
 */
void az_storage_init(Storage* storage) {
    if(!storage) return;
    storage_common_mkdir(storage, AZ_DATA_DIR);
    storage_common_mkdir(storage, AZ_FIGURES_DIR);
    storage_common_mkdir(storage, AZ_LOCKON_DIR);
}

/**
 * @brief Enumerate saved Amiibo NFC files into a sorted catalog.
 * @param storage Storage service used for file operations.
 * @param out Destination for the computed result.
 * @param max_entries Maximum supported entries.
 * @return The resulting count.
 */
uint16_t az_saved_scan(Storage* storage, AzSavedEntry* out, uint16_t max_entries) {
    if(!storage || !out || max_entries == 0) return 0;

    File* dir = storage_file_alloc(storage);
    if(!dir) return 0;
    if(!storage_dir_open(dir, AZ_FIGURES_DIR)) {
        storage_file_free(dir);
        return 0;
    }

    NfcDevice* device = nfc_device_alloc();
    if(!device) {
        storage_dir_close(dir);
        storage_file_free(dir);
        return 0;
    }
    uint16_t count = 0;
    FileInfo info;
    char name[96];
    while(count < max_entries && storage_dir_read(dir, &info, name, sizeof(name))) {
        if((info.flags & FSF_DIRECTORY) || !az_has_nfc_extension(name)) continue;

        AzSavedEntry* entry = &out[count];
        memset(entry, 0, sizeof(*entry));
        az_str_copy(entry->filename, sizeof(entry->filename), name);

        char path[AZ_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, name);
        nfc_device_clear(device);
        entry->valid = az_nfc_load_device(device, path) && az_nfc_extract_figure_id(device, entry->id);
        if(!entry->valid) continue;

        az_str_copy(entry->display_name, sizeof(entry->display_name), name);
        char* dot = strrchr(entry->display_name, '.');
        if(dot) *dot = 0;
        count++;
    }

    nfc_device_free(device);
    storage_dir_close(dir);
    storage_file_free(dir);
    az_saved_sort(out, count);
    return count;
}

/**
 * @brief Enumerate valid Lock-On payload files into a sorted catalog.
 * @param storage Storage service used for file operations.
 * @param out Destination for the computed result.
 * @param max_entries Maximum supported entries.
 * @return The resulting count.
 */
uint16_t az_lockon_scan(Storage* storage, AzLockOnEntry* out, uint16_t max_entries) {
    if(!storage || !out || max_entries == 0) return 0;

    File* dir = storage_file_alloc(storage);
    if(!dir) return 0;
    if(!storage_dir_open(dir, AZ_LOCKON_DIR)) {
        storage_file_free(dir);
        return 0;
    }

    uint16_t count = 0;
    FileInfo info;
    char name[96];
    while(count < max_entries && storage_dir_read(dir, &info, name, sizeof(name))) {
        if((info.flags & FSF_DIRECTORY) || !az_lockon_size_valid(info.size)) continue;
        if(strchr(name, '/') || strchr(name, '\\')) continue;

        AzLockOnEntry* entry = &out[count];
        memset(entry, 0, sizeof(*entry));
        az_str_copy(entry->filename, sizeof(entry->filename), name);
        az_str_copy(entry->display_name, sizeof(entry->display_name), name);
        char* dot = strrchr(entry->display_name, '.');
        if(dot && dot != entry->display_name) *dot = '\0';
        entry->source_size = (uint8_t)info.size;
        count++;
    }

    storage_dir_close(dir);
    storage_file_free(dir);
    az_lockon_sort(out, count);
    return count;
}

/**
 * @brief Load and validate a Lock-On SRAM payload by filename.
 * @param storage Storage service used for file operations.
 * @param filename Filename relative to the relevant application data directory.
 * @param out_sram Destination Lock-On SRAM buffer.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_lockon_load(
    Storage* storage,
    const char* filename,
    uint8_t out_sram[AZ_LOCKON_SRAM_SIZE]) {
    if(!storage || !filename || !out_sram || strchr(filename, '/') || strchr(filename, '\\')) {
        return false;
    }

    char path[AZ_PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%s", AZ_LOCKON_DIR, filename);
    if(length <= 0 || (size_t)length >= sizeof(path)) return false;

    File* file = storage_file_alloc(storage);
    if(!file) return false;
    bool ok = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(file);
        if(az_lockon_size_valid(size)) {
            memset(out_sram, 0, AZ_LOCKON_SRAM_SIZE);
            size_t read_size = (size_t)size;
            if(storage_file_read(file, out_sram, read_size) == read_size) {
                uint16_t crc = az_lockon_crc16(out_sram, AZ_LOCKON_PAYLOAD_MAX);
                out_sram[AZ_LOCKON_PAYLOAD_MAX] = (uint8_t)(crc >> 8);
                out_sram[AZ_LOCKON_PAYLOAD_MAX + 1U] = (uint8_t)crc;
                ok = true;
            }
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Build the companion Lock-On path for a saved figure filename.
 * @param saved_filename Saved figure filename.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_saved_lockon_path(const char* saved_filename, char* out, size_t out_size) {
    if(!saved_filename || !out || out_size == 0) return false;
    char nfc_path[AZ_PATH_MAX];
    if(!az_saved_full_path(saved_filename, nfc_path, sizeof(nfc_path))) return false;
    const char* suffix = ".lockon";
    size_t base_len = strlen(nfc_path);
    size_t suffix_len = strlen(suffix);
    if(base_len + suffix_len + 1U > out_size) return false;
    memcpy(out, nfc_path, base_len);
    memcpy(out + base_len, suffix, suffix_len + 1U);
    return true;
}

/**
 * @brief Build a temporary or alternate companion Lock-On work path.
 * @param saved_filename Saved figure filename.
 * @param suffix Filename suffix to append.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_saved_lockon_work_path(
    const char* saved_filename,
    const char* suffix,
    char* out,
    size_t out_size) {
    if(!saved_filename || !suffix || !out || out_size == 0) return false;
    char base[AZ_PATH_MAX];
    if(!az_saved_lockon_path(saved_filename, base, sizeof(base))) return false;
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    if(base_len + suffix_len + 1U > out_size) return false;
    memcpy(out, base, base_len);
    memcpy(out + base_len, suffix, suffix_len + 1U);
    return true;
}

/**
 * @brief Load the Lock-On companion payload associated with a saved figure.
 * @param storage Storage service used for file operations.
 * @param saved_filename Saved figure filename.
 * @param out_sram Destination Lock-On SRAM buffer.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_lockon_load(
    Storage* storage,
    const char* saved_filename,
    uint8_t out_sram[AZ_LOCKON_SRAM_SIZE]) {
    if(!storage || !saved_filename || !out_sram) return false;
    char path[AZ_PATH_MAX];
    if(!az_saved_lockon_path(saved_filename, path, sizeof(path))) return false;

    File* file = storage_file_alloc(storage);
    if(!file) return false;
    bool ok = false;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_size(file) == AZ_LOCKON_SRAM_SIZE &&
       storage_file_read(file, out_sram, AZ_LOCKON_SRAM_SIZE) == AZ_LOCKON_SRAM_SIZE) {
        uint16_t crc = az_lockon_crc16(out_sram, AZ_LOCKON_PAYLOAD_MAX);
        ok = out_sram[AZ_LOCKON_PAYLOAD_MAX] == (uint8_t)(crc >> 8) &&
             out_sram[AZ_LOCKON_PAYLOAD_MAX + 1U] == (uint8_t)crc;
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Persist a Lock-On companion payload for a saved figure.
 * @param storage Storage service used for file operations.
 * @param saved_filename Saved figure filename.
 * @param sram Lock-On SRAM payload.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_lockon_save(
    Storage* storage,
    const char* saved_filename,
    const uint8_t sram[AZ_LOCKON_SRAM_SIZE]) {
    if(!storage || !saved_filename || !sram) return false;
    uint16_t crc = az_lockon_crc16(sram, AZ_LOCKON_PAYLOAD_MAX);
    if(sram[AZ_LOCKON_PAYLOAD_MAX] != (uint8_t)(crc >> 8) ||
       sram[AZ_LOCKON_PAYLOAD_MAX + 1U] != (uint8_t)crc) {
        return false;
    }

    char path[AZ_PATH_MAX];
    char tmp_path[AZ_PATH_MAX];
    char backup_path[AZ_PATH_MAX];
    if(!az_saved_lockon_path(saved_filename, path, sizeof(path)) ||
       !az_saved_lockon_work_path(saved_filename, ".tmp", tmp_path, sizeof(tmp_path)) ||
       !az_saved_lockon_work_path(saved_filename, ".bak", backup_path, sizeof(backup_path))) {
        return false;
    }

    if(storage_common_exists(storage, tmp_path)) storage_common_remove(storage, tmp_path);
    File* file = storage_file_alloc(storage);
    if(!file) return false;
    bool written = false;
    if(storage_file_open(file, tmp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        written = storage_file_write(file, sram, AZ_LOCKON_SRAM_SIZE) == AZ_LOCKON_SRAM_SIZE &&
                  storage_file_sync(file);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!written) {
        storage_common_remove(storage, tmp_path);
        return false;
    }

    bool had_old = storage_common_exists(storage, path);
    if(storage_common_exists(storage, backup_path)) storage_common_remove(storage, backup_path);
    if(had_old && storage_common_rename(storage, path, backup_path) != FSE_OK) {
        storage_common_remove(storage, tmp_path);
        return false;
    }
    if(storage_common_rename(storage, tmp_path, path) != FSE_OK) {
        if(had_old) storage_common_rename(storage, backup_path, path);
        storage_common_remove(storage, tmp_path);
        return false;
    }
    if(had_old && storage_common_exists(storage, backup_path)) {
        storage_common_remove(storage, backup_path);
    }
    return true;
}

/**
 * @brief Convert arbitrary display text into a filesystem-safe save-name component.
 * @param in Input text to sanitize.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 */
static void az_sanitize_filename(const char* in, char* out, size_t out_size) {
    size_t p = 0;
    bool last_sep = false;
    for(const char* s = in; *s && p + 1 < out_size; ++s) {
        unsigned char c = (unsigned char)*s;
        if(az_ascii_isalnum(c)) {
            out[p++] = (char)c;
            last_sep = false;
        } else if(!last_sep && p > 0 && p + 1 < out_size) {
            out[p++] = '_';
            last_sep = true;
        }
    }
    while(p > 0 && out[p - 1] == '_') p--;
    if(p == 0) {
        az_str_copy(out, out_size, "amiibo");
        return;
    }
    out[p] = 0;
}

/**
 * @brief Build an unused save path derived from figure metadata.
 * @param storage Storage service used for file operations.
 * @param figure Figure metadata.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 */
void az_make_unique_save_path(Storage* storage, const AzFigure* figure, char* out, size_t out_size) {
    if(!storage || !figure || !out || out_size == 0) return;
    char safe[48];
    az_sanitize_filename(figure->name, safe, sizeof(safe));

    if(!az_build_save_path(out, out_size, safe, figure->id_hex, ".nfc")) return;
    if(!storage_common_exists(storage, out)) return;

    for(unsigned i = 2; i < 1000; i++) {
        char suffix[16];
        snprintf(suffix, sizeof(suffix), "-%u.nfc", i);
        if(!az_build_save_path(out, out_size, safe, figure->id_hex, suffix)) return;
        if(!storage_common_exists(storage, out)) return;
    }
}

/**
 * @brief Normalize a requested saved-figure name before rename processing.
 * @param in Input text to sanitize.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 */
static void az_sanitize_rename(const char* in, char* out, size_t out_size) {
    if(!out || out_size == 0) return;
    out[0] = '\0';
    if(!in) return;
    size_t input_len = strlen(in);
    if(input_len >= 4 && strcasecmp(in + input_len - 4, ".nfc") == 0) input_len -= 4;
    size_t p = 0;
    bool last_sep = false;
    for(size_t i = 0; i < input_len && p + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if(az_ascii_isalnum(c) || c == '-' || c == '_' || c == '(' || c == ')') {
            out[p++] = (char)c;
            last_sep = false;
        } else if(c == ' ' || c == '.' || c == '\t') {
            if(!last_sep && p > 0) {
                out[p++] = '_';
                last_sep = true;
            }
        } else if(!last_sep && p > 0) {
            out[p++] = '_';
            last_sep = true;
        }
    }
    while(p > 0 && (out[p - 1] == '_' || out[p - 1] == '.')) p--;
    if(p == 0) az_str_copy(out, out_size, "amiibo");
    else out[p] = '\0';
}

/**
 * @brief Build the absolute figures-directory path for a saved filename.
 * @param filename Filename relative to the relevant application data directory.
 * @param out Destination for the computed result.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_saved_full_path(const char* filename, char* out, size_t out_size) {
    if(!filename || !out || out_size == 0 || strchr(filename, '/') || strchr(filename, '\\')) return false;
    const char* prefix = AZ_FIGURES_DIR "/";
    const size_t prefix_len = strlen(prefix);
    const size_t name_len = strlen(filename);
    if(prefix_len + name_len + 1 > out_size) return false;
    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, filename, name_len + 1);
    return true;
}

/**
 * @brief Check whether a saved filename is currently unused.
 * @param storage Storage service used for file operations.
 * @param filename Filename relative to the relevant application data directory.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_saved_name_available(Storage* storage, const char* filename) {
    if(!storage || !filename) return false;
    char nfc_path[AZ_PATH_MAX];
    char lockon_path[AZ_PATH_MAX];
    if(!az_saved_full_path(filename, nfc_path, sizeof(nfc_path)) ||
       !az_saved_lockon_path(filename, lockon_path, sizeof(lockon_path))) {
        return false;
    }
    return !storage_common_exists(storage, nfc_path) &&
           !storage_common_exists(storage, lockon_path);
}

/**
 * @brief Rename a saved figure and its companion Lock-On data.
 * @param storage Storage service used for file operations.
 * @param old_filename Current saved figure filename.
 * @param requested_name User-requested replacement display name.
 * @param out_filename Destination for the final saved filename.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_rename(
    Storage* storage,
    const char* old_filename,
    const char* requested_name,
    char* out_filename,
    size_t out_size) {
    if(!storage || !old_filename || !requested_name || !out_filename || out_size == 0) return false;
    if(strchr(old_filename, '/') || strchr(old_filename, '\\')) return false;

    char safe[72];
    az_sanitize_rename(requested_name, safe, sizeof(safe));
    char old_path[AZ_PATH_MAX];
    if(!az_saved_full_path(old_filename, old_path, sizeof(old_path))) return false;

    char candidate[96];
    int written = snprintf(candidate, sizeof(candidate), "%s.nfc", safe);
    if(written <= 0 || (size_t)written >= sizeof(candidate)) return false;
    char new_path[AZ_PATH_MAX];
    if(!az_saved_full_path(candidate, new_path, sizeof(new_path))) return false;

    if(strcmp(candidate, old_filename) != 0 && !az_saved_name_available(storage, candidate)) {
        bool found = false;
        for(unsigned suffix = 2; suffix < 1000; suffix++) {
            written = snprintf(candidate, sizeof(candidate), "%s-%u.nfc", safe, suffix);
            if(written <= 0 || (size_t)written >= sizeof(candidate)) return false;
            if(!az_saved_full_path(candidate, new_path, sizeof(new_path))) return false;
            if(az_saved_name_available(storage, candidate)) {
                found = true;
                break;
            }
        }
        if(!found) return false;
    }

    if(strcmp(candidate, old_filename) == 0) {
        az_str_copy(out_filename, out_size, candidate);
        return true;
    }

    char old_lockon[AZ_PATH_MAX];
    char new_lockon[AZ_PATH_MAX];
    bool has_lockon = az_saved_lockon_path(old_filename, old_lockon, sizeof(old_lockon)) &&
                      storage_common_exists(storage, old_lockon);
    if(has_lockon && !az_saved_lockon_path(candidate, new_lockon, sizeof(new_lockon))) return false;

    if(storage_common_rename(storage, old_path, new_path) != FSE_OK) return false;
    if(has_lockon && storage_common_rename(storage, old_lockon, new_lockon) != FSE_OK) {

        storage_common_rename(storage, new_path, old_path);
        return false;
    }
    az_str_copy(out_filename, out_size, candidate);
    return true;
}

/**
 * @brief Delete a saved figure and any companion Lock-On data.
 * @param storage Storage service used for file operations.
 * @param filename Filename relative to the relevant application data directory.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_saved_delete(Storage* storage, const char* filename) {
    if(!storage || !filename || strchr(filename, '/') || strchr(filename, '\\')) return false;
    char path[AZ_PATH_MAX];
    if(!az_saved_full_path(filename, path, sizeof(path))) return false;

    if(storage_common_remove(storage, path) != FSE_OK) return false;

    char lockon_path[AZ_PATH_MAX];
    if(az_saved_lockon_path(filename, lockon_path, sizeof(lockon_path)) &&
       storage_common_exists(storage, lockon_path) &&
       storage_common_remove(storage, lockon_path) != FSE_OK) {
        return false;
    }
    return true;
}
