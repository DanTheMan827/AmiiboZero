/**
 * @file amiibo_zero.h
 * @brief Shared models, constants, feature flags, and public module APIs for Amiibo Zero.
 */

#pragma once

#include <furi.h>
#include <storage/storage.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>
#include <string.h>

/** Stable Flipper application identifier. */
#define AZ_APP_ID "amiibo_zero"
/** Human-readable application name. */
#define AZ_APP_NAME "Amiibo Zero"
/** Application semantic version. */
#define AZ_APP_VERSION "0.2.6"

/**
 * @brief Enable the legacy on-device `.idx` library cache.
 *
 * The default is disabled: browsing and search stream `amiibo.json` directly
 * through lwJSON. Set to 1 at build time to retain the optional index-backed
 * path for experimentation or slower SD cards.
 */
#ifndef AZ_FEATURE_JSON_INDEX
#define AZ_FEATURE_JSON_INDEX 0
#endif

/** App-owned SD-card directory. */
#define AZ_DATA_DIR EXT_PATH("apps_data/amiibo_zero")
/** Directory containing persistent native NFC figure files. */
#define AZ_FIGURES_DIR AZ_DATA_DIR "/figures"
/** Raw AmiiboAPI figure database processed on-device. */
#define AZ_AMIIBO_JSON AZ_DATA_DIR "/amiibo.json"
/** Raw AmiiboAPI game-compatibility database processed on-device. */
#define AZ_GAMES_JSON AZ_DATA_DIR "/games_info.json"
/** Optional feature-flagged figure index. */
#define AZ_INDEX_FILE AZ_DATA_DIR "/amiibo.idx"
/** Temporary path used while atomically rebuilding the optional index. */
#define AZ_INDEX_TMP AZ_DATA_DIR "/amiibo.idx.tmp"
/** Temporary unsorted rows used by the optional index builder. */
#define AZ_INDEX_RAW AZ_DATA_DIR "/amiibo.raw.tmp"
/** User-supplied retail Amiibo key file. */
#define AZ_KEYS_FILE AZ_DATA_DIR "/key_retail.bin"

/** Maximum user-visible Amiibo/category/game name bytes including NUL. */
#define AZ_NAME_MAX 64
/** Maximum app-owned filesystem path bytes including NUL. */
#define AZ_PATH_MAX 192
/** Maximum compatibility usage text bytes including NUL. */
#define AZ_USAGE_MAX 128
/** Maximum search-query bytes including NUL. */
#define AZ_QUERY_MAX 32
/** Number of list rows visible on the 128x64 display. */
#define AZ_LIST_ROWS 4
/** Maximum compatibility records retained for one selected figure. */
#define AZ_MAX_GAMES 128
/** Maximum saved NFC files retained in the saved-figure menu. */
#define AZ_MAX_SAVED 128
/** Maximum Amiibo series/categories retained in the small category cache. */
#define AZ_MAX_CATEGORIES 96

/**
 * @brief Copy a C string into a bounded destination.
 * @param dst Destination buffer.
 * @param dst_size Destination capacity in bytes.
 * @param src Source string; NULL is treated as empty.
 */
static inline void az_str_copy(char* dst, size_t dst_size, const char* src) {
    if(!dst || dst_size == 0) return;
    if(!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if(len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/** One Amiibo figure decoded from the raw API database. */
typedef struct {
    uint8_t id[8];              /**< Full binary Amiibo identifier. */
    char id_hex[17];            /**< Lowercase 16-digit hexadecimal identifier. */
    char name[AZ_NAME_MAX];     /**< Human-readable figure name. */
    char release_na[12];        /**< North-American release date when present. */
    uint8_t category;           /**< Amiibo series byte, derived from id[6]. */
} AzFigure;

/** One category/series displayed before its figures. */
typedef struct {
    uint8_t id;                 /**< Amiibo series/category byte. */
    char name[AZ_NAME_MAX];     /**< AmiiboAPI series display name. */
    uint16_t count;             /**< Number of figures belonging to the category. */
} AzCategory;

/** One game-compatibility usage row. */
typedef struct {
    char platform[9];           /**< Short platform label such as "Switch 2". */
    char name[AZ_NAME_MAX];     /**< Game title. */
    char usage[AZ_USAGE_MAX];   /**< Amiibo usage description. */
    bool writes;                /**< True when the game can write Amiibo state. */
} AzGame;

/** One validated saved native NFC file. */
typedef struct {
    char filename[96];          /**< Basename inside AZ_FIGURES_DIR. */
    char display_name[AZ_NAME_MAX]; /**< Resolved Amiibo name or filename fallback. */
    uint8_t id[8];              /**< Figure ID extracted from NFC data. */
    bool valid;                 /**< True when the NFC file is a usable Amiibo. */
} AzSavedEntry;

/** Decrypted retail key material loaded from key_retail.bin. */
typedef struct {
    uint8_t data_key[80];       /**< Amiibo data-key derivation block. */
    uint8_t static_key[80];     /**< Amiibo tag/static-key derivation block. */
    bool valid;                 /**< True after an exact-size key file was loaded. */
} AzKeys;

/** Application screens rendered by the single custom main view. */
typedef enum {
    AzScreenHome,              /**< Top-level application menu. */
    AzScreenCategories,        /**< Alphabetical Amiibo category list. */
    AzScreenFigures,           /**< Alphabetical figures in the selected category. */
    AzScreenSearchResults,     /**< Search results from raw JSON or optional index. */
    AzScreenFigure,            /**< Selected figure details/actions. */
    AzScreenSaved,             /**< Saved native NFC figures. */
    AzScreenGames,             /**< Compatibility detail browser. */
    AzScreenEmulate,           /**< Active NFC emulation status. */
    AzScreenStatus,            /**< Setup/database/key status. */
    AzScreenAbout,             /**< About and controls help. */
    AzScreenConfirmDelete,     /**< Saved-figure deletion confirmation. */
} AzScreen;

/** Forward declaration of the application-owned runtime state. */
typedef struct AmiiboZeroApp AmiiboZeroApp;

/** Lock-protected snapshot consumed by the GUI draw callback. */
typedef struct {
    AmiiboZeroApp* app;                         /**< Owning application used by drawing helpers. */
    AzScreen screen;                            /**< Screen represented by this snapshot. */
    uint16_t selection;                         /**< Absolute selected row/action. */
    uint16_t count;                             /**< Total rows in the current list. */
    uint16_t window_start;                      /**< Absolute ordinal of visible row zero. */
    AzCategory category_rows[AZ_LIST_ROWS];     /**< Visible category rows. */
    AzFigure figure_rows[AZ_LIST_ROWS];         /**< Visible figure/search rows. */
    uint8_t row_count;                          /**< Number of valid visible rows. */
    char query[AZ_QUERY_MAX];                   /**< Active search query. */
    AzCategory category;                        /**< Selected category snapshot. */
    AzFigure figure;                            /**< Selected figure snapshot. */
    bool figure_saved;                          /**< True when selected figure came from Saved. */
    char saved_filename[96];                    /**< Persistent saved basename when applicable. */
    AzGame games[AZ_LIST_ROWS];                 /**< Compatibility snapshot; currently row zero is used. */
    uint16_t games_total;                       /**< Number of compatibility records. */
    char status_line[64];                       /**< Temporary toast overriding the header. */
    uint8_t animation;                          /**< Monotonic scroll/marquee tick. */
} AzViewModel;

/** Complete runtime state owned by amiibo_zero_app(). */
struct AmiiboZeroApp {
    Gui* gui;                                   /**< Borrowed GUI service. */
    Storage* storage;                           /**< Borrowed Storage service. */
    ViewDispatcher* dispatcher;                 /**< GUI dispatcher owned by the app. */
    View* main_view;                            /**< Custom full-screen main view. */
    TextInput* text_input;                      /**< Search text-input module. */
    FuriString* ui_scratch;                     /**< Reusable drawing string to avoid transient allocations. */

    AzScreen screen;                            /**< Current screen. */
    AzScreen return_screen;                     /**< Screen restored after figure details. */
    uint16_t return_selection;                  /**< Selection restored with return_screen. */
    uint16_t selection;                         /**< Current list/action selection. */
    uint16_t list_count;                        /**< Total rows in the active list. */
    uint16_t detail_scroll;                     /**< Vertical wrapped-text scroll position. */
    char query[AZ_QUERY_MAX];                   /**< Current search query. */
    char text_buffer[AZ_NAME_MAX];              /**< TextInput destination buffer. */

    AzCategory current_category;                /**< Category currently being browsed. */
    AzFigure current_figure;                    /**< Figure currently selected/emulated. */
    bool current_is_saved;                      /**< Whether current_figure maps to a saved NFC file. */
    char current_saved_filename[96];            /**< Saved basename for current_figure. */

    AzGame game_entries[AZ_MAX_GAMES];          /**< Bounded compatibility results for current figure. */
    uint16_t game_count;                        /**< Number of valid game_entries. */
    AzSavedEntry saved_entries[AZ_MAX_SAVED];   /**< Bounded saved-file catalog. */
    uint16_t saved_count;                       /**< Number of valid saved_entries. */

    AzKeys keys;                                /**< Loaded Amiibo retail key material. */
    bool index_ready;                           /**< Database/index preparation status; name kept for ABI simplicity. */
    uint32_t index_count;                       /**< Number of figures discovered/prepared. */

    Nfc* nfc;                                   /**< NFC worker owned by the app. */
    NfcDevice* nfc_device;                      /**< Mutable native NFC device data. */
    NfcListener* listener;                      /**< Active emulation listener or NULL. */
    bool emulating;                             /**< True while listener is actively emulating. */
    bool emulation_persistent;                  /**< Save synchronized writes to emulation_path. */
    char emulation_path[AZ_PATH_MAX];           /**< Exact persistent NFC path for write-back. */
    char toast[64];                             /**< Temporary header toast text. */
    uint8_t toast_ticks;                        /**< Remaining 250ms toast ticks. */
    uint8_t animation;                          /**< UI animation/marquee tick counter. */
};

/**
 * @brief Load exactly one key_retail.bin into bounded key structures.
 * @param storage Open Flipper Storage service.
 * @param keys Destination key structure, cleared before loading.
 * @return True only when the file exists and is exactly 160 bytes.
 */
bool az_keys_load(Storage* storage, AzKeys* keys);
/**
 * @brief Generate a fresh encrypted 532-byte Amiibo dump for a figure.
 * @param figure_id Eight-byte Amiibo figure identifier.
 * @param keys Valid retail key material.
 * @param out_dump Destination for the complete 532-byte encrypted dump.
 * @param out_raw_uid Destination for the generated nine-byte raw UID representation.
 * @return True when key derivation, HMAC generation, and encryption all succeed.
 */
bool az_generate_dump(
    const uint8_t figure_id[8],
    const AzKeys* keys,
    uint8_t out_dump[532],
    uint8_t out_raw_uid[9]);
/**
 * @brief Convert the 9-byte Amiibo raw UID representation to the 7 RF UID bytes.
 * @param raw_uid Nine-byte Amiibo raw UID representation including BCC bytes.
 * @param uid7 Destination seven-byte RF UID.
 */
void az_raw_uid_to_nfc_uid(const uint8_t raw_uid[9], uint8_t uid7[7]);
/**
 * @brief Derive the four-byte NTAG password from a raw Amiibo UID.
 * @param raw_uid Nine-byte Amiibo raw UID representation.
 * @param password Destination four-byte NTAG password.
 */
void az_tag_password(const uint8_t raw_uid[9], uint8_t password[4]);

/**
 * @brief Prepare database access; optionally build the feature-flagged `.idx` cache.
 * @param storage Open Flipper Storage service.
 * @param force Force category refresh or optional-index rebuild.
 * @param out_count Optional destination for total figure count.
 * @return True when the raw database is usable and preparation succeeds.
 */
bool az_db_ensure_index(Storage* storage, bool force, uint32_t* out_count);
/**
 * @brief Load a four-row alphabetical category window.
 * @param storage Open Flipper Storage service.
 * @param selection Absolute selected category ordinal.
 * @param out_rows Destination visible category rows.
 * @param out_row_count Destination number of valid rows.
 * @param out_window_start Destination absolute ordinal represented by out_rows[0].
 * @param out_total Destination total category count.
 * @return True when the category data was read successfully.
 */
bool az_db_get_category_window(
    Storage* storage,
    uint16_t selection,
    AzCategory out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total);
/**
 * @brief Fetch one alphabetical category by ordinal.
 * @param storage Open Flipper Storage service.
 * @param category_index Zero-based alphabetical category ordinal.
 * @param out Destination category record.
 * @return True when the ordinal exists.
 */
bool az_db_get_category(Storage* storage, uint16_t category_index, AzCategory* out);
/**
 * @brief Load a four-row alphabetical figure window for one category.
 * @param storage Open Flipper Storage service.
 * @param category Amiibo series/category byte.
 * @param selection Absolute selected figure ordinal within the category.
 * @param out_rows Destination visible figure rows.
 * @param out_row_count Destination number of valid rows.
 * @param out_window_start Destination absolute ordinal represented by out_rows[0].
 * @param out_total Destination total figures in the category.
 * @return True when the raw JSON or optional index was read successfully.
 */
bool az_db_get_figure_window(
    Storage* storage,
    uint8_t category,
    uint16_t selection,
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total);
/**
 * @brief Fetch one figure by its alphabetical ordinal within a category.
 * @param storage Open Flipper Storage service.
 * @param category Amiibo series/category byte.
 * @param figure_index Zero-based alphabetical ordinal within the category.
 * @param out Destination figure record.
 * @return True when the ordinal exists.
 */
bool az_db_get_figure(Storage* storage, uint8_t category, uint16_t figure_index, AzFigure* out);
/**
 * @brief Load a four-row alphabetical search-result window.
 * @param storage Open Flipper Storage service.
 * @param query Case-insensitive name substring or hexadecimal-ID search text.
 * @param selection Absolute selected match ordinal.
 * @param out_rows Destination visible figure rows.
 * @param out_row_count Destination number of valid rows.
 * @param out_window_start Destination absolute ordinal represented by out_rows[0].
 * @param out_total Destination total matching figure count.
 * @return True when search completed successfully.
 */
bool az_db_search_window(
    Storage* storage,
    const char* query,
    uint16_t selection,
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total);
/**
 * @brief Fetch one search result by its alphabetical filtered ordinal.
 * @param storage Open Flipper Storage service.
 * @param query Case-insensitive name substring or hexadecimal-ID search text.
 * @param match_index Zero-based alphabetical match ordinal.
 * @param out Destination figure record.
 * @return True when the requested match exists.
 */
bool az_db_search_get(Storage* storage, const char* query, uint16_t match_index, AzFigure* out);
/**
 * @brief Resolve one exact figure ID from raw JSON or the optional index.
 * @param storage Open Flipper Storage service.
 * @param id Exact eight-byte Amiibo identifier.
 * @param out Destination figure record.
 * @return True when the figure ID exists.
 */
bool az_db_find_by_id(Storage* storage, const uint8_t id[8], AzFigure* out);
/**
 * @brief Stream games_info.json and collect compatibility rows matching one figure ID.
 * @param storage Open Flipper Storage service.
 * @param id Exact eight-byte Amiibo identifier.
 * @param out_games Caller-owned bounded compatibility array.
 * @param max_games Capacity of out_games in entries.
 * @param out_count Destination number of populated compatibility entries.
 * @return True when games_info.json parsed successfully.
 */
bool az_db_load_games(
    Storage* storage,
    const uint8_t id[8],
    AzGame* out_games,
    uint16_t max_games,
    uint16_t* out_count);

/**
 * @brief Create required app-data directories.
 * @param storage Open Flipper Storage service.
 */
void az_storage_init(Storage* storage);
/**
 * @brief Scan saved native NFC files into a bounded caller array.
 * @param storage Open Flipper Storage service.
 * @param out Destination array for validated saved figures.
 * @param max_entries Capacity of out in entries.
 * @return Number of populated saved-figure entries.
 */
uint16_t az_saved_scan(Storage* storage, AzSavedEntry* out, uint16_t max_entries);
/**
 * @brief Construct a unique persistent save path for a figure.
 * @param storage Open Flipper Storage service.
 * @param figure Figure whose name/ID seed the filename.
 * @param out Destination path buffer.
 * @param out_size Capacity of out in bytes.
 */
void az_make_unique_save_path(Storage* storage, const AzFigure* figure, char* out, size_t out_size);
/**
 * @brief Delete one saved basename after path-safety validation.
 * @param storage Open Flipper Storage service.
 * @param filename Basename within AZ_FIGURES_DIR; path separators are rejected.
 * @return True when the validated file was removed.
 */
bool az_saved_delete(Storage* storage, const char* filename);

/**
 * @brief Generate and install fresh NTAG215 Amiibo data into an NFC device.
 * @param device Destination Flipper NFC device.
 * @param figure Figure metadata containing the eight-byte ID.
 * @param keys Valid retail key material.
 * @return True when a valid native NTAG215 device image was installed.
 */
bool az_nfc_generate_device(NfcDevice* device, const AzFigure* figure, const AzKeys* keys);
/**
 * @brief Extract the Amiibo figure ID from native NFC device data.
 * @param device NFC device containing MIFARE Ultralight data.
 * @param out_id Destination eight-byte Amiibo figure ID.
 * @return True when the device has a valid Amiibo-shaped NTAG215 payload.
 */
bool az_nfc_extract_figure_id(const NfcDevice* device, uint8_t out_id[8]);
/**
 * @brief Validate that device data represents an NTAG215-shaped Amiibo payload.
 * @param device NFC device to validate.
 * @return True when the device contains the expected NTAG215 page geometry.
 */
bool az_nfc_validate_amiibo(const NfcDevice* device);
/**
 * @brief Save an NFC device using Flipper's native `.nfc` serializer.
 * @param device NFC device to serialize.
 * @param path Exact destination path.
 * @return True when the native save operation succeeds.
 */
bool az_nfc_save_device(NfcDevice* device, const char* path);
/**
 * @brief Load an NFC device from a native `.nfc` file.
 * @param device NFC device receiving the loaded data.
 * @param path Exact source path.
 * @return True when the native load operation succeeds.
 */
bool az_nfc_load_device(NfcDevice* device, const char* path);

/**
 * @brief Allocate and register all UI modules and callbacks.
 * @param app Application instance receiving owned UI objects.
 */
void az_ui_init(AmiiboZeroApp* app);
/**
 * @brief Remove and free all UI modules owned by the app.
 * @param app Application instance whose UI objects are released.
 */
void az_ui_deinit(AmiiboZeroApp* app);
/**
 * @brief Refresh the lock-protected view model from current runtime state.
 * @param app Application instance used as the source snapshot.
 */
void az_ui_refresh(AmiiboZeroApp* app);
/**
 * @brief Change the active screen and switch to the custom main view.
 * @param app Application instance.
 * @param screen Screen to activate.
 */
void az_ui_show(AmiiboZeroApp* app, AzScreen screen);
/**
 * @brief Display a short-lived header toast and request redraw.
 * @param app Application instance.
 * @param text Toast text copied into the bounded app buffer.
 */
void az_ui_toast(AmiiboZeroApp* app, const char* text);
/**
 * @brief Stop emulation, synchronize reader writes, and persist when configured.
 * @param app Application instance owning the NFC listener/device.
 */
void az_emulation_stop(AmiiboZeroApp* app);
