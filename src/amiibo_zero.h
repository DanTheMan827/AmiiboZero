/**
 * @file amiibo_zero.h
 * @brief Shared models, constants, and documented public APIs for Amiibo Zero.
 */

#pragma once

#include <furi.h>
#include <storage/storage.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>
#include <nfc/nfc_poller.h>
#include <string.h>

/** Stable Flipper application identifier. */
#define AZ_APP_ID "amiibo_zero"
/** Human-readable application name. */
#define AZ_APP_NAME "Amiibo Zero"
/** Application semantic version. */
#define AZ_APP_VERSION "0.1.0"

/** Compile-time heap overlay; enabled by default for memory-debug builds. */
#ifndef AZ_DISABLE_MEMORY_OVERLAY
#define AZ_DEBUG_MEMORY_OVERLAY
#endif

/** App-owned SD-card directory. */
#define AZ_DATA_DIR EXT_PATH("apps_data/amiibo_zero")
/** Directory containing persistent native NFC figure files. */
#define AZ_FIGURES_DIR AZ_DATA_DIR "/figures"
/** Directory containing user-supplied type-3 lock-on/vehicle payload files. */
#define AZ_LOCKON_DIR AZ_DATA_DIR "/lock_on"
/** Raw AmiiboAPI figure database processed on-device. */
#define AZ_AMIIBO_JSON AZ_DATA_DIR "/amiibo.json"
/** Raw AmiiboAPI game-compatibility database processed on-device. */
#define AZ_GAMES_JSON AZ_DATA_DIR "/games_info.json"
/** Unified binary seek index covering both JSON databases. */
#define AZ_INDEX_FILE AZ_DATA_DIR "/amiibo.idx"
/** Transactional temporary path for the unified seek index. */
#define AZ_INDEX_TMP AZ_DATA_DIR "/amiibo.idx.tmp"
/** Backup path used while atomically replacing a stale index. */
#define AZ_INDEX_BACKUP AZ_DATA_DIR "/amiibo.idx.bak"
/** Temporary unsorted figure-record stream used while rebuilding the legacy seek index. */
#define AZ_INDEX_RAW AZ_DATA_DIR "/amiibo.raw.tmp"
/** Temporary sorted-run stream used by the legacy seek-index fallback sorter. */
#define AZ_INDEX_SORT_RUNS AZ_DATA_DIR "/amiibo.sort.tmp"
/** Temporary generalized games_info object-start records during index construction. */
#define AZ_INDEX_GAME_RAW AZ_DATA_DIR "/games.raw.tmp"
/** User-supplied retail Amiibo key file. */
#define AZ_KEYS_FILE AZ_DATA_DIR "/key_retail.bin"

/** Encrypted Amiibo tag bytes through page 132, excluding PWD/PACK. */
#define AZ_DUMP_SIZE 532U
/** Complete NTAG215 page image including PWD/PACK pages. */
#define AZ_NTAG215_BYTES 540U
/** Number of pages emulated for NTAG I2C Plus 2K lock-on figures. */
#define AZ_V3_PAGES 492U
/** Bytes represented by AZ_V3_PAGES. */
#define AZ_V3_BYTES (AZ_V3_PAGES * 4U)
/** Complete type-3 SRAM response exposed at logical pages F0-FF. */
#define AZ_LOCKON_SRAM_SIZE 64U
/** Maximum payload bytes before the two-byte CRC16-MCRF4XX transport checksum. */
#define AZ_LOCKON_PAYLOAD_MAX 62U

/** Maximum bounded UI/detail name bytes including NUL; catalog names use variable-length storage. */
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
/** Maximum lock-on payload files shown by the on-device picker. */
#define AZ_MAX_LOCKONS 64
/** Maximum Amiibo series/categories retained while building the index. */
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

/** One Amiibo figure decoded from the database/index. */
typedef struct {
    uint8_t id[8];              /**< Full binary Amiibo identifier. */
    char id_hex[17];            /**< Lowercase 16-digit hexadecimal identifier. */
    char name[AZ_NAME_MAX];     /**< Human-readable figure name. */
    char release_na[12];        /**< North-American release date when present. */
    uint8_t category;           /**< Amiibo series byte, derived from id[6]. */
    uint8_t type;               /**< Amiibo model type byte, derived from id[3]. */
    uint32_t json_offset;       /**< Byte offset of this figure's object in amiibo.json. */
    uint32_t json_length;       /**< Byte length of this figure's object in amiibo.json. */
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
    char display_name[AZ_NAME_MAX]; /**< Filename stem shown in the Saved menu. */
    uint8_t id[8];              /**< Figure ID extracted from NFC data. */
    bool valid;                 /**< True when the NFC file is a usable Amiibo. */
} AzSavedEntry;

/** One user-supplied type-3 lock-on/vehicle payload file. */
typedef struct {
    char filename[96];              /**< Basename inside AZ_LOCKON_DIR. */
    char display_name[AZ_NAME_MAX]; /**< Filename stem shown by the picker. */
    uint8_t source_size;            /**< Original payload size in bytes. */
} AzLockOnEntry;

/** Decrypted retail key material loaded from key_retail.bin. */
typedef struct {
    uint8_t data_key[80];       /**< Amiibo data-key derivation block. */
    uint8_t static_key[80];     /**< Amiibo tag/static-key derivation block. */
    bool valid;                 /**< True after an exact-size key file was loaded. */
} AzKeys;

/** Best-effort metadata decoded from an authenticated saved Amiibo dump. */
typedef struct {
    bool available;             /**< True when decryption and both HMAC checks succeeded. */
    bool initialized;           /**< Amiibo register/nickname data has been initialized. */
    bool app_data_initialized;  /**< Application area has been initialized by a game. */
    char nickname[48];          /**< Amiibo nickname decoded from UTF-16BE when present. */
    char owner_mii[48];         /**< Owner Mii name decoded from UTF-16BE when present. */
    char init_date[12];         /**< Registration date as YYYY-MM-DD when valid. */
    char write_date[12];        /**< Last-write date as YYYY-MM-DD when valid. */
    uint16_t write_counter;     /**< Main Amiibo write counter. */
    uint64_t application_id;    /**< Application/title identifier when app data exists. */
    uint32_t application_area_id; /**< Application-area identifier when app data exists. */
    uint16_t application_write_counter; /**< Application-area write counter. */
} AzAmiiboDetails;

/** Application screens rendered by the custom main view. */
typedef enum {
    AzScreenHome,               /**< Top-level application menu. */
    AzScreenCategories,         /**< Alphabetical Amiibo category list. */
    AzScreenFigures,            /**< Alphabetical figures in the selected category. */
    AzScreenSearchResults,      /**< Search results from the unified binary index. */
    AzScreenFigure,             /**< Selected figure actions. */
    AzScreenDumpInfo,           /**< Scrollable saved-dump metadata. */
    AzScreenSaved,              /**< Saved native NFC figures. */
    AzScreenAdvanced,           /**< Advanced tools menu. */
    AzScreenTagOperation,       /**< Physical NTAG215 read/write/clear workflow. */
    AzScreenLockOn,             /**< Type-3 lock-on/vehicle payload picker. */
    AzScreenGames,              /**< Compatibility detail browser. */
    AzScreenEmulate,            /**< Active NFC emulation status. */
    AzScreenStatus,             /**< Setup/database/key status. */
    AzScreenAbout,              /**< Concise application information. */
    AzScreenConfirmDelete,      /**< Saved-figure deletion confirmation. */
    AzScreenWorking,            /**< Animated wait screen used during index generation. */
    AzScreenCount,              /**< Number of screen enum values used for selection memory. */
} AzScreen;

/** Purpose currently assigned to the shared TextInput module. */
typedef enum {
    AzTextInputSearch,          /**< Search library by name/ID. */
    AzTextInputRename,          /**< Rename a saved native NFC file. */
} AzTextInputMode;

/** Deferred action resumed after the user chooses a type-3 lock-on payload. */
typedef enum {
    AzLockOnActionNone,               /**< No pending lock-on-dependent action. */
    AzLockOnActionGenerateTemporary,  /**< Generate and emulate without saving. */
    AzLockOnActionGeneratePersistent, /**< Generate, save, and emulate a fresh copy. */
    AzLockOnActionEmulateSaved,       /**< Attach missing lock-on data, then emulate a saved figure. */
    AzLockOnActionReplaceSaved,       /**< Replace the saved figure's lock-on sidecar. */
} AzLockOnAction;

/** Physical NTAG215 operation selected from the UI. */
typedef enum {
    AzTagOperationNone,        /**< No physical-tag operation is active. */
    AzTagOperationWrite,       /**< Write the selected v2 Amiibo to a blank NTAG215. */
    AzTagOperationReadSave,    /**< Read an NTAG215 Amiibo and save it as a native NFC file. */
    AzTagOperationClear,       /**< Reset writable Amiibo user-data pages on an existing tag. */
} AzTagOperation;

/** Current user-visible stage of a physical tag operation. */
typedef enum {
    AzTagStageIdle,            /**< No operation. */
    AzTagStageScanBlank,       /**< Validate a blank NTAG215 and capture its UID. */
    AzTagStageReading,         /**< Read an existing NTAG215 Amiibo. */
    AzTagStagePreparing,       /**< Re-key or rebuild encrypted data for the detected UID. */
    AzTagStageWriting,         /**< Program a blank NTAG215. */
    AzTagStageClearing,        /**< Rewrite only unlocked Amiibo user-data pages. */
    AzTagStageDone,            /**< Operation completed or failed; waiting for user acknowledgement. */
} AzTagStage;

/** Final/phase result for a physical tag operation. */
typedef enum {
    AzTagResultNone,          /**< No result yet. */
    AzTagResultSuccess,       /**< Requested operation completed. */
    AzTagResultWrongTag,      /**< Detected tag is not an NTAG215. */
    AzTagResultNotBlank,      /**< Destination has lock/config state and is not blank. */
    AzTagResultNotAmiibo,     /**< Read payload failed Amiibo authentication/format validation. */
    AzTagResultUidChanged,    /**< A different physical UID appeared between workflow phases. */
    AzTagResultAuthFailed,    /**< PWD_AUTH was rejected or returned an unexpected PACK. */
    AzTagResultReadFailed,    /**< NFC read/activation transaction failed. */
    AzTagResultCryptoFailed,  /**< Existing Amiibo crypto operation failed. */
    AzTagResultWriteFailed,   /**< One or more NTAG page writes failed. */
    AzTagResultSaveFailed,    /**< Native NFC file could not be saved. */
} AzTagResult;

/** Database preparation stages reported by the background index worker. */
typedef enum {
    AzDbProgressChecking,       /**< Check source metadata/sample identity and validate any existing index. */
    AzDbProgressAmiibo,         /**< Stream amiibo.json and collect figure/category metadata. */
    AzDbProgressSorting,        /**< Sort bounded category batches in RAM and stream index order. */
    AzDbProgressGames,          /**< Stream games_info.json and append compatibility offsets. */
    AzDbProgressFinalizing,     /**< Finish structural checks and promote the completed index. */
    AzDbProgressDone,           /**< Preparation completed successfully. */
} AzDbProgressStage;

/**
 * @brief Callback used by database preparation to publish coarse progress.
 * @param context Caller-owned callback context.
 * @param stage Current preparation stage.
 * @param percent Overall completion percentage in the inclusive range 0..100.
 */
typedef void (*AzDbProgressCallback)(void* context, AzDbProgressStage stage, uint8_t percent);

/** Forward declaration of the application-owned runtime state. */
typedef struct AmiiboZeroApp AmiiboZeroApp;

/** Lock-protected snapshot consumed by the GUI draw callback. */
typedef struct {
    AmiiboZeroApp* app;                         /**< Owning application used by drawing helpers. */
    AzScreen screen;                            /**< Screen represented by this snapshot. */
    uint16_t selection;                         /**< Absolute selected row/action. */
    uint16_t count;                             /**< Total rows in the current list. */
    uint16_t window_start;                      /**< Absolute ordinal of visible row zero. */
    AzCategory category_rows[AZ_LIST_ROWS];     /**< Visible category rows read from the seek index. */
    AzFigure figure_rows[AZ_LIST_ROWS];         /**< Visible figure/search rows read from the seek index. */
    const char* figure_row_names[AZ_LIST_ROWS]; /**< Convenience pointers into figure_rows[].name. */
    uint8_t row_count;                          /**< Number of valid visible rows. */
    char query[AZ_QUERY_MAX];                   /**< Active search query. */
    AzCategory category;                        /**< Selected category snapshot. */
    AzFigure figure;                            /**< Selected figure snapshot. */
    FuriString* figure_name;                    /**< Selected display-name snapshot/fallback for detail screens. */
    bool figure_saved;                          /**< True when selected figure came from Saved. */
    char saved_filename[96];                    /**< Persistent saved basename when applicable. */
    AzGame games[AZ_LIST_ROWS];                 /**< Compatibility snapshot; row zero is used. */
    uint16_t games_total;                       /**< Number of compatibility records. */
    char status_line[64];                       /**< Temporary toast overriding the header. */
    uint8_t animation;                          /**< Monotonic scroll/hourglass tick. */
    uint8_t db_progress;                        /**< Overall database preparation completion 0..100. */
    AzDbProgressStage db_progress_stage;        /**< Current database preparation stage. */
} AzViewModel;

/** Complete runtime state owned by amiibo_zero_app(). */
struct AmiiboZeroApp {
    Gui* gui;                                   /**< Borrowed GUI service. */
    Storage* storage;                           /**< Borrowed Storage service. */
    ViewDispatcher* dispatcher;                 /**< GUI dispatcher owned by the app. */
    View* main_view;                            /**< Custom full-screen main view. */
    TextInput* text_input;                      /**< Search/rename text-input module. */
    ByteInput* byte_input;                      /**< Eight-byte hexadecimal manual-ID editor. */
    FuriString* ui_scratch;                     /**< Reusable drawing string to avoid transient allocations. */

    AzScreen screen;                            /**< Current screen. */
    uint16_t screen_selection[AzScreenCount];   /**< Last selected row for each screen. */
    AzScreen return_screen;                     /**< Screen restored after figure details. */
    uint16_t return_selection;                  /**< Selection restored with return_screen. */
    uint16_t selection;                         /**< Current list/action selection. */
    uint16_t list_count;                        /**< Total rows in the active list. */
    uint16_t detail_scroll;                     /**< Vertical wrapped-text scroll position. */
    char query[AZ_QUERY_MAX];                   /**< Current search query. */
    char text_buffer[AZ_NAME_MAX];              /**< TextInput destination buffer. */
    AzTextInputMode text_input_mode;            /**< Current TextInput completion behavior. */
    uint8_t manual_id[8];                       /**< Eight-byte manual figure ID edited as hex bytes. */

    AzCategory current_category;                /**< Category currently being browsed. */
    AzFigure current_figure;                    /**< Figure currently selected/emulated. */
    FuriString* current_figure_name;             /**< Selected display name, including saved-file fallbacks. */
    bool current_is_saved;                      /**< Whether current_figure maps to a saved NFC file. */
    char current_saved_filename[96];            /**< Saved basename for current_figure. */
    AzAmiiboDetails current_details;             /**< Decrypted metadata for current saved dump. */

    AzGame* game_entries;                       /**< On-demand bounded compatibility results, or NULL. */
    uint16_t game_count;                        /**< Number of valid game_entries. */
    AzSavedEntry* saved_entries;                /**< On-demand bounded saved-file catalog, or NULL. */
    uint16_t saved_count;                       /**< Number of valid saved_entries. */
    AzLockOnEntry* lockon_entries;              /**< On-demand lock-on file catalog, or NULL. */
    uint16_t lockon_count;                      /**< Number of valid lockon_entries. */
    uint8_t current_lockon_sram[AZ_LOCKON_SRAM_SIZE]; /**< Selected/runtime 64-byte SRAM response. */
    bool current_lockon_valid;                  /**< True when current_lockon_sram is available. */
    char current_lockon_filename[96];           /**< Selected source basename; empty when loaded from a saved sidecar. */
    AzLockOnAction lockon_action;               /**< Action resumed after lock-on selection. */

    AzKeys keys;                                /**< Loaded Amiibo retail key material. */
    bool index_ready;                           /**< Unified index passed validation. */
    uint32_t index_count;                       /**< Number of indexed figures. */

    FuriThread* db_thread;                      /**< Background index-generation worker or NULL. */
    volatile bool db_thread_done;               /**< Worker completion flag polled by the GUI tick. */
    bool db_thread_result;                      /**< Index result produced by the worker. */
    uint32_t db_thread_count;                   /**< Figure count produced by the worker. */
    bool db_thread_force;                       /**< Force rebuild requested for the active worker. */
    AzScreen db_thread_return_screen;           /**< Screen shown after the worker completes. */
    volatile uint8_t db_progress;               /**< Overall worker progress in percent. */
    volatile AzDbProgressStage db_progress_stage; /**< Worker stage copied into the GUI snapshot. */

    NfcPoller* tag_poller;                      /**< Active physical-tag poller or NULL. */
    AzTagOperation tag_operation;                /**< Requested physical NTAG215 action. */
    volatile AzTagStage tag_stage;               /**< Current physical-tag workflow stage. */
    volatile AzTagResult tag_result;             /**< Last physical-tag phase/final result. */
    volatile bool tag_phase_done;                /**< Set by NFC callback when the current phase stops. */
    volatile uint8_t tag_progress;               /**< Page-write progress in percent. */
    uint8_t* tag_work_dump;                      /**< 532-byte encrypted source/prepared dump while active. */
    uint8_t tag_target_uid[9];                   /**< Raw nine-byte UID captured from the physical NTAG215. */
    AzScreen tag_return_screen;                  /**< Screen restored after tag operation. */
    uint16_t tag_return_selection;               /**< Selection restored with tag_return_screen. */
    char tag_saved_filename[96];                 /**< Basename created by Read & save. */

    Nfc* nfc;                                   /**< NFC worker owned by the app. */
    NfcDevice* nfc_device;                      /**< Mutable native NFC device data. */
    NfcListener* listener;                      /**< Active emulation listener or NULL. */
    BitBuffer* v3_tx_buffer;                    /**< Reusable standard/short response buffer for v3 emulation. */
    bool v3_i2c_listener;                       /**< True when the stock-derived I2C Plus listener is active. */
    bool v3_sector_select_pending;              /**< Waiting for the second SECTOR_SELECT frame. */
    bool v3_authenticated;                      /**< Current PWD_AUTH state, reset like Flipper's MFUL listener. */
    bool v3_sram_ready;                         /**< Live NS_REG.SRAM_RF_READY state for the lock-on mailbox. */
    uint8_t v3_sector;                          /**< Current NTAG I2C logical sector selected over NFC. */
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
#ifdef __cplusplus
extern "C" {
#endif

bool az_keys_load(Storage* storage, AzKeys* keys);

/**
 * @brief Generate a fresh encrypted standard Amiibo dump for a figure.
 * @param figure_id Eight-byte Amiibo figure identifier.
 * @param keys Valid retail key material.
 * @param out_dump Destination for the 532 encrypted data bytes.
 * @param out_raw_uid Destination for the generated nine-byte raw UID representation.
 * @return True when key derivation, HMAC generation, and encryption all succeed.
 */
bool az_generate_dump(
    const uint8_t figure_id[8],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_raw_uid[9]);

/**
 * @brief Generate, authenticate, and encrypt a fresh type-3 Amiibo crypto image.
 * @param figure_id Eight-byte Amiibo figure identifier.
 * @param keys Valid retail key material.
 * @param out_dump Destination for the 532 encrypted data bytes before the 0x40-byte v3 insertion.
 * @param out_uid7 Destination seven-byte RF UID used directly by type-3 tags.
 * @return True when key derivation, HMAC generation, and encryption all succeed.
 */
bool az_generate_v3_dump(
    const uint8_t figure_id[8],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_uid7[7]);

/**
 * @brief Authenticate and decrypt one standard encrypted Amiibo dump.
 * @param encrypted_dump Source 532-byte encrypted dump.
 * @param keys Valid retail key material.
 * @param out_plain Destination 532-byte plaintext-layout dump.
 * @return True only when AES decryption succeeds and both stored HMACs verify.
 */
bool az_decrypt_dump(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_plain[AZ_DUMP_SIZE]);

/**
 * @brief Re-sign an existing encrypted dump around a newly randomized UID.
 * @param encrypted_dump Source 532-byte encrypted dump containing reader-written state.
 * @param keys Valid retail key material.
 * @param out_dump Destination re-encrypted dump.
 * @param out_raw_uid Destination new nine-byte raw UID representation.
 * @return True when the source authenticates and the new UID image is fully re-signed.
 */
bool az_rekey_dump_uid(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_raw_uid[9]);

/**
 * @brief Re-sign an authenticated standard Amiibo dump for a caller-supplied physical UID.
 * @param encrypted_dump Existing encrypted 532-byte Amiibo image.
 * @param keys Valid retail keys.
 * @param raw_uid Exact nine-byte raw UID/BCC layout read from the destination NTAG215.
 * @param out_dump Destination encrypted image bound to raw_uid.
 * @return True when authentication, UID replacement, HMAC generation, and encryption succeed.
 */
bool az_rekey_dump_uid_to(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    const uint8_t raw_uid[9],
    uint8_t out_dump[AZ_DUMP_SIZE]);

/**
 * @brief Build the canonical empty Amiibo user state while preserving UID/model/salt.
 * @param encrypted_dump Existing authenticated standard Amiibo image.
 * @param keys Valid retail keys.
 * @param out_dump Destination encrypted image with writable user state reset.
 * @return True when the source authenticates and the empty state is re-signed/encrypted.
 */
bool az_clear_user_data_dump(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE]);

/**
 * @brief Re-sign one type-3 crypto image around a fresh direct seven-byte UID.
 * @param encrypted_dump Existing authenticated 532-byte v3 crypto image with the nonce window removed.
 * @param keys Valid retail key material.
 * @param out_dump Destination re-encrypted image.
 * @param out_uid7 Destination new seven-byte RF UID.
 * @return True when the existing image authenticates and re-encryption succeeds.
 */
bool az_rekey_v3_dump_uid(
    const uint8_t encrypted_dump[AZ_DUMP_SIZE],
    const AzKeys* keys,
    uint8_t out_dump[AZ_DUMP_SIZE],
    uint8_t out_uid7[7]);

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
 * @brief Return a display label for the Amiibo model type byte.
 * @param type Amiibo model type byte from the figure ID.
 * @return Static human-readable type label.
 */
const char* az_figure_type_name(uint8_t type);

/**
 * @brief Return whether a figure uses the newer type-3 lock-on layout.
 * @param id Eight-byte Amiibo figure identifier.
 * @return True when the identifier's final byte selects the type-3 layout.
 */
bool az_figure_is_v3(const uint8_t id[8]);

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
 * @brief Scan user-supplied lock-on payload files into a bounded caller array.
 * @param storage Open Flipper Storage service.
 * @param out Destination array for valid lock-on files.
 * @param max_entries Capacity of out in entries.
 * @return Number of populated lock-on entries.
 */
uint16_t az_lockon_scan(Storage* storage, AzLockOnEntry* out, uint16_t max_entries);

/**
 * @brief Load and normalize one lock-on file into the 64-byte SRAM response.
 * @param storage Open Flipper Storage service.
 * @param filename Basename within AZ_LOCKON_DIR; path separators are rejected.
 * @param out_sram Destination 64-byte SRAM response including CRC16-MCRF4XX.
 * @return True for a 1..62-byte payload or a complete 64-byte response.
 */
bool az_lockon_load(
    Storage* storage,
    const char* filename,
    uint8_t out_sram[AZ_LOCKON_SRAM_SIZE]);

/**
 * @brief Load the normalized lock-on response associated with one saved type-3 figure.
 * @param storage Open Flipper Storage service.
 * @param saved_filename Saved .nfc basename within AZ_FIGURES_DIR.
 * @param out_sram Destination 64-byte SRAM response.
 * @return True when the companion sidecar exists, is exactly 64 bytes, and has a valid CRC.
 */
bool az_saved_lockon_load(
    Storage* storage,
    const char* saved_filename,
    uint8_t out_sram[AZ_LOCKON_SRAM_SIZE]);

/**
 * @brief Persist the normalized lock-on response associated with one saved type-3 figure.
 * @param storage Open Flipper Storage service.
 * @param saved_filename Saved .nfc basename within AZ_FIGURES_DIR.
 * @param sram Complete normalized 64-byte SRAM response.
 * @return True when the 64-byte companion sidecar was written and synchronized.
 */
bool az_saved_lockon_save(
    Storage* storage,
    const char* saved_filename,
    const uint8_t sram[AZ_LOCKON_SRAM_SIZE]);

/**
 * @brief Construct a unique persistent save path for a figure.
 * @param storage Open Flipper Storage service.
 * @param figure Figure whose name/ID seed the filename.
 * @param out Destination path buffer.
 * @param out_size Capacity of out in bytes.
 */
void az_make_unique_save_path(
    Storage* storage, const AzFigure* figure, const char* display_name, char* out, size_t out_size);

/**
 * @brief Rename one saved file using a sanitized user-visible basename.
 * @param storage Open Flipper Storage service.
 * @param old_filename Existing basename within AZ_FIGURES_DIR.
 * @param requested_name User-entered new basename, with or without .nfc.
 * @param out_filename Destination for the actual non-colliding basename used.
 * @param out_size Capacity of out_filename in bytes.
 * @return True when the file was renamed successfully.
 */
bool az_saved_rename(
    Storage* storage,
    const char* old_filename,
    const char* requested_name,
    char* out_filename,
    size_t out_size);

/**
 * @brief Delete one saved basename after path-safety validation.
 * @param storage Open Flipper Storage service.
 * @param filename Basename within AZ_FIGURES_DIR; path separators are rejected.
 * @return True when the validated file was removed.
 */
bool az_saved_delete(Storage* storage, const char* filename);

/**
 * @brief Generate and install fresh standard or type-3 Amiibo rider data into an NFC device.
 * @param device Destination Flipper NFC device.
 * @param figure Figure metadata containing the eight-byte ID.
 * @param keys Valid retail key material.
 * @return True when a valid native NFC device image was installed. Type-3 SRAM is supplied separately at emulation time.
 */
bool az_nfc_generate_device(NfcDevice* device, const AzFigure* figure, const AzKeys* keys);

/**
 * @brief Return whether a validated native device uses the type-3 I2C Plus layout.
 * @param device Native NFC device.
 * @return True for a supported type-3 device.
 */
bool az_nfc_device_is_v3(const NfcDevice* device);

/**
 * @brief Start standard or custom type-3 NFC emulation for app->nfc_device.
 * @param app Application state owning the NFC transport and device.
 * @return True when emulation started.
 */
bool az_nfc_listener_start(AmiiboZeroApp* app);

/**
 * @brief Stop RF and synchronize any listener writes into app->nfc_device.
 * @param app Application state owning the active listener.
 * @return True when an active emulation session stopped cleanly.
 */
bool az_nfc_listener_pause_and_sync(AmiiboZeroApp* app);

/**
 * @brief Randomize the UID of an existing Amiibo device by decrypting and re-signing it.
 * @param device Mutable NFC device; caller must stop RF emulation before invoking.
 * @param keys Valid retail key material.
 * @return True when the old dump authenticated and the new UID image was installed.
 */
bool az_nfc_randomize_uid(NfcDevice* device, const AzKeys* keys);

/**
 * @brief Decode authenticated saved-dump metadata such as nickname and owner Mii name.
 * @param device NFC device containing standard or type-3 Amiibo data.
 * @param keys Valid retail key material.
 * @param out Destination details structure, always initialized by the function.
 * @return True when encrypted Amiibo data authenticated and details were decoded.
 */
bool az_nfc_read_details(const NfcDevice* device, const AzKeys* keys, AzAmiiboDetails* out);

/**
 * @brief Extract the Amiibo figure ID from native NFC device data.
 * @param device NFC device containing MIFARE Ultralight data.
 * @param out_id Destination eight-byte Amiibo figure ID.
 * @return True when the device has a supported Amiibo payload.
 */
bool az_nfc_extract_figure_id(const NfcDevice* device, uint8_t out_id[8]);

/**
 * @brief Validate standard NTAG215 or experimental type-3 NTAG I2C Plus 2K Amiibo data.
 * @param device NFC device to validate.
 * @return True when the device contains supported Amiibo page geometry.
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
 * @brief Load and validate an NFC device from a native `.nfc` file.
 * @param device NFC device receiving the loaded data.
 * @param path Exact source path.
 * @return True when the native load operation succeeds.
 */
bool az_nfc_load_device(NfcDevice* device, const char* path);

/**
 * @brief Extract the standard 532-byte encrypted image from an NTAG215 native device.
 * @param device Valid standard Amiibo NFC device.
 * @param out_dump Destination encrypted dump.
 * @return True only for standard NTAG215 data (type-3 is intentionally rejected).
 */
bool az_nfc_export_v2_dump(const NfcDevice* device, uint8_t out_dump[AZ_DUMP_SIZE]);

/** Begin writing an encrypted v2 Amiibo image to a blank physical NTAG215. */
bool az_tag_write_begin(AmiiboZeroApp* app, const uint8_t encrypted_dump[AZ_DUMP_SIZE]);
/** Begin reading a physical NTAG215 Amiibo and saving it in the app figures directory. */
bool az_tag_read_save_begin(AmiiboZeroApp* app);
/** Begin resetting the writable user-state pages of a physical NTAG215 Amiibo. */
bool az_tag_clear_begin(AmiiboZeroApp* app);
/** Cancel and release any active physical-tag poller/work buffer. */
void az_tag_operation_cancel(AmiiboZeroApp* app);
/** Advance phase transitions after NFC callbacks complete; call from the UI tick. */
void az_tag_operation_tick(AmiiboZeroApp* app);

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
 * @brief Start background index validation/rebuild and show the animated wait screen.
 * @param app Application instance.
 * @param force Force rebuild even if source size/sample identities match.
 * @param return_screen Screen shown when the worker completes.
 * @return True when a worker was started.
 */
bool az_ui_start_database_prepare(AmiiboZeroApp* app, bool force, AzScreen return_screen);

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

#ifdef __cplusplus
}
#endif
