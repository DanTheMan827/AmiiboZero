/**
 * @file amiibo_storage.c
 * @brief Saved-figure and type-3 lock-on storage, naming, sorting, and lifecycle handling.
 */

#include "./amiibo_zero.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/**
 * @brief Build a full saved-figure path from a trusted basename.
 */
static bool az_saved_full_path(const char* filename, char* out, size_t out_size);

/**
 * @brief Test whether one byte is an ASCII letter or decimal digit.
 */
static bool az_ascii_isalnum(unsigned char c) {
    return ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'Z')) ||
           ((c >= 'a') && (c <= 'z'));
}

/**
 * @brief Append a string to a bounded destination without truncation overflow.
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
 * @brief Assemble one bounded saved-figure path from sanitized components.
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
 * @brief Check whether a filename ends in .nfc case-insensitively.
 */
static bool az_has_nfc_extension(const char* name) {
    const char* dot = strrchr(name, '.');
    if(!dot) return false;
    return strcasecmp(dot, ".nfc") == 0;
}

/**
 * @brief Compare saved entries alphabetically by their display names.
 */
static int az_saved_compare(const AzSavedEntry* a, const AzSavedEntry* b) {
    return strcasecmp(a->display_name, b->display_name);
}

/**
 * @brief Insertion-sort the bounded saved-entry array alphabetically.
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
 * @brief Compare lock-on entries alphabetically by display name.
 */
static int az_lockon_compare(const AzLockOnEntry* a, const AzLockOnEntry* b) {
    return strcasecmp(a->display_name, b->display_name);
}

/**
 * @brief Insertion-sort the bounded lock-on catalog alphabetically.
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
 * @brief Compute CRC16-MCRF4XX for one normalized lock-on response.
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
 * @brief Return whether a lock-on source size is supported.
 */
static bool az_lockon_size_valid(uint64_t size) {
    return (size >= 1U && size <= AZ_LOCKON_PAYLOAD_MAX) || size == AZ_LOCKON_SRAM_SIZE;
}

/**
 * @brief Create the application data and saved-figure directories when missing.
 */
void az_storage_init(Storage* storage) {
    if(!storage) return;
    storage_common_mkdir(storage, AZ_DATA_DIR);
    storage_common_mkdir(storage, AZ_FIGURES_DIR);
    storage_common_mkdir(storage, AZ_LOCKON_DIR);
}

/**
 * @brief Scan, validate, resolve, and alphabetize saved native NFC files.
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

        /* Saved-menu labels deliberately follow the filename so user renames are visible. */
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
 * @brief Scan small user-supplied lock-on payloads without retaining their contents in RAM.
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
 * @brief Load a lock-on payload and synthesize/repair its two-byte transport CRC.
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
 * @brief Build the companion lock-on sidecar path for one saved .nfc basename.
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
 * @brief Append one internal suffix to a validated saved lock-on path.
 * @param saved_filename Saved .nfc basename.
 * @param suffix Internal suffix such as .tmp or .bak.
 * @param out Destination path.
 * @param out_size Destination capacity.
 * @return True when the path fit without truncation.
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
 * @brief Load one app-owned saved lock-on sidecar and verify its CRC.
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
 * @brief Write one normalized 64-byte lock-on response beside a saved .nfc file.
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

    /* Write and sync a complete replacement before touching the current sidecar. */
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
 * @brief Convert a figure name into a filesystem-safe filename component.
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
 * @brief Choose a non-colliding persistent path for a newly saved figure.
 */
void az_make_unique_save_path(
    Storage* storage,
    const AzFigure* figure,
    const char* display_name,
    char* out,
    size_t out_size) {
    if(!storage || !figure || !out || out_size == 0) return;
    char safe[48];
    az_sanitize_filename(display_name && display_name[0] ? display_name : "amiibo", safe, sizeof(safe));
    char id_hex[17];
    static const char hex[] = "0123456789abcdef";
    for(size_t i = 0U; i < 8U; i++) {
        id_hex[i * 2U] = hex[figure->id[i] >> 4];
        id_hex[i * 2U + 1U] = hex[figure->id[i] & 0x0FU];
    }
    id_hex[16] = '\0';

    if(!az_build_save_path(out, out_size, safe, id_hex, ".nfc")) return;
    if(!storage_common_exists(storage, out)) return;

    for(unsigned i = 2; i < 1000; i++) {
        char suffix[16];
        snprintf(suffix, sizeof(suffix), "-%u.nfc", i);
        if(!az_build_save_path(out, out_size, safe, id_hex, suffix)) return;
        if(!storage_common_exists(storage, out)) return;
    }
}


/**
 * @brief Sanitize a user-entered saved-file basename while preserving readable separators.
 * @param in User-entered text, optionally ending in .nfc.
 * @param out Destination basename without extension.
 * @param out_size Destination capacity.
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
 * @brief Build a full saved-figure path from a trusted basename.
 * @param filename Basename only.
 * @param out Destination path.
 * @param out_size Destination capacity.
 * @return True when the path fit without truncation.
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
 * @brief Return whether a candidate saved basename is free of both NFC and lock-on artifacts.
 * @param storage Open Storage service.
 * @param filename Candidate .nfc basename.
 * @return True when neither the NFC file nor its companion sidecar already exists.
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
 * @brief Rename one saved NFC file to a sanitized non-colliding basename.
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
        /* Keep the pair together when possible; failure still reports the rename as unsuccessful. */
        storage_common_rename(storage, new_path, old_path);
        return false;
    }
    az_str_copy(out_filename, out_size, candidate);
    return true;
}

/**
 * @brief Delete one saved figure and its optional lock-on sidecar after path validation.
 */
bool az_saved_delete(Storage* storage, const char* filename) {
    if(!storage || !filename || strchr(filename, '/') || strchr(filename, '\\')) return false;
    char path[AZ_PATH_MAX];
    if(!az_saved_full_path(filename, path, sizeof(path))) return false;

    /* Delete the primary object first.  A failed sidecar cleanup can only leave an
     * ignored orphan, whereas deleting the sidecar first could strand a type-3 NFC file. */
    if(storage_common_remove(storage, path) != FSE_OK) return false;

    char lockon_path[AZ_PATH_MAX];
    if(az_saved_lockon_path(filename, lockon_path, sizeof(lockon_path)) &&
       storage_common_exists(storage, lockon_path) &&
       storage_common_remove(storage, lockon_path) != FSE_OK) {
        return false;
    }
    return true;
}
