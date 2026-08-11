/**
 * @file amiibo_storage.c
 * @brief Persistent saved-figure directory scanning, naming, sorting, and deletion.
 */

#include "./amiibo_zero.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

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
 * @brief Create the application data and saved-figure directories when missing.
 */
void az_storage_init(Storage* storage) {
    if(!storage) return;
    storage_common_mkdir(storage, AZ_DATA_DIR);
    storage_common_mkdir(storage, AZ_FIGURES_DIR);
}

/**
 * @brief Scan, validate, resolve, and alphabetize saved native NFC files.
 */
uint16_t az_saved_scan(Storage* storage, AzSavedEntry* out, uint16_t max_entries) {
    if(!storage || !out || max_entries == 0) return 0;

    File* dir = storage_file_alloc(storage);
    if(!storage_dir_open(dir, AZ_FIGURES_DIR)) {
        storage_dir_close(dir);
        storage_file_free(dir);
        return 0;
    }

    NfcDevice* device = nfc_device_alloc();
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

        AzFigure figure;
        if(entry->valid && az_db_find_by_id(storage, entry->id, &figure)) {
            az_str_copy(entry->display_name, sizeof(entry->display_name), figure.name);
        } else {
            az_str_copy(entry->display_name, sizeof(entry->display_name), name);
            char* dot = strrchr(entry->display_name, '.');
            if(dot) *dot = 0;
        }
        count++;
    }

    nfc_device_free(device);
    storage_dir_close(dir);
    storage_file_free(dir);
    az_saved_sort(out, count);
    return count;
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
 * @brief Delete one saved figure after rejecting path traversal characters.
 */
bool az_saved_delete(Storage* storage, const char* filename) {
    if(!storage || !filename || strchr(filename, '/')) return false;
    char path[AZ_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", AZ_FIGURES_DIR, filename);
    return storage_common_remove(storage, path) == FSE_OK;
}
