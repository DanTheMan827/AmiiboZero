/**
 * @file amiibo_zero.h
 * @brief Shared declarations for Amiibo Zero.
 * @details Defines application constants, shared data models, runtime state,
 * and cross-module interfaces.
 */

#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>
#include <storage/storage.h>
#include <string.h>

/** @brief Stable application identifier used by the Flipper environment. */
#define AZ_APP_ID "amiibo_zero"

/** @brief Human-readable application name. */
#define AZ_APP_NAME "Amiibo Zero"

/** @brief Application semantic version. */
#define AZ_APP_VERSION "0.1.0"

/** @brief Root directory for Amiibo Zero application data. */
#define AZ_DATA_DIR EXT_PATH("apps_data/amiibo_zero")

/** @brief Directory containing saved NFC figure files. */
#define AZ_FIGURES_DIR AZ_DATA_DIR "/figures"

/** @brief Directory containing reusable Lock-On payload files. */
#define AZ_LOCKON_DIR AZ_DATA_DIR "/lock_on"

/** @brief Path to the Amiibo metadata JSON source. */
#define AZ_AMIIBO_JSON AZ_DATA_DIR "/amiibo.json"

/** @brief Path to the game compatibility JSON source. */
#define AZ_GAMES_JSON AZ_DATA_DIR "/games_info.json"

/** @brief Path to the active binary metadata index. */
#define AZ_INDEX_FILE AZ_DATA_DIR "/amiibo.idx"

/** @brief Path used while constructing a replacement index. */
#define AZ_INDEX_TMP AZ_DATA_DIR "/amiibo.idx.tmp"

/** @brief Path used to preserve the previous index during replacement. */
#define AZ_INDEX_BACKUP AZ_DATA_DIR "/amiibo.idx.bak"

/** @brief Temporary path for unsorted figure index records. */
#define AZ_INDEX_RAW AZ_DATA_DIR "/amiibo.raw.tmp"

/** @brief Temporary path for externally sorted figure runs. */
#define AZ_INDEX_SORT_RUNS AZ_DATA_DIR "/amiibo.sort.tmp"

/** @brief Path to the retail Amiibo key bundle. */
#define AZ_KEYS_FILE AZ_DATA_DIR "/key_retail.bin"

/** @brief Encrypted Amiibo dump size in bytes. */
#define AZ_DUMP_SIZE 532U

/** @brief Total NTAG215 user-memory image size in bytes. */
#define AZ_NTAG215_BYTES 540U

/** @brief Number of logical pages in the version-3 layout. */
#define AZ_V3_PAGES 492U

/** @brief Total byte capacity of the version-3 page layout. */
#define AZ_V3_BYTES (AZ_V3_PAGES * 4U)

/** @brief Size of the in-memory Lock-On SRAM buffer. */
#define AZ_LOCKON_SRAM_SIZE 64U

/** @brief Largest supported Lock-On payload before metadata bytes. */
#define AZ_LOCKON_PAYLOAD_MAX 62U

/** @brief Maximum storage allocated for display names. */
#define AZ_NAME_MAX 64

/** @brief Maximum storage allocated for application paths. */
#define AZ_PATH_MAX 192

/** @brief Maximum storage allocated for game usage text. */
#define AZ_USAGE_MAX 128

/** @brief Maximum storage allocated for search input. */
#define AZ_QUERY_MAX 32

/** @brief Number of list rows visible on the main display. */
#define AZ_LIST_ROWS 4

/** @brief Maximum game compatibility entries loaded at once. */
#define AZ_MAX_GAMES 128

/** @brief Maximum saved-figure entries retained in memory. */
#define AZ_MAX_SAVED 128

/** @brief Maximum Lock-On entries retained in memory. */
#define AZ_MAX_LOCKONS 64

/** @brief Maximum category records supported by the index builder. */
#define AZ_MAX_CATEGORIES 96

/**
 * @brief Copy a string into a fixed-size destination with guaranteed NUL
 * termination.
 * @param dst Destination string buffer.
 * @param dst_size Capacity of the destination string buffer, including the
 * terminator.
 * @param src Source string; NULL is treated as empty.
 */
static inline void az_str_copy(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0)
    return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= dst_size)
    len = dst_size - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

/** @brief Metadata and source location for one Amiibo figure record. */
typedef struct {
  uint8_t id[8];   /**< Eight-byte Amiibo identifier. */
  char id_hex[17]; /**< Canonical hexadecimal representation of the identifier.
                    */
  char name[AZ_NAME_MAX]; /**< Human-readable figure name. */
  char release_na[12];  /**< North American release-date text when available. */
  uint8_t category;     /**< Category identifier used by the local index. */
  uint8_t type;         /**< Amiibo type code. */
  uint32_t json_offset; /**< Byte offset of the source JSON object. */
  uint32_t json_length; /**< Byte length of the source JSON object. */
} AzFigure;

/** @brief Amiibo category metadata and the number of indexed figures it
 * contains. */
typedef struct {
  uint8_t id;             /**< Category identifier. */
  char name[AZ_NAME_MAX]; /**< Human-readable category name. */
  uint16_t count;         /**< Number of figures assigned to the category. */
} AzCategory;

/** @brief One game compatibility entry associated with an Amiibo. */
typedef struct {
  char platform[9];         /**< Short platform label. */
  char name[AZ_NAME_MAX];   /**< Game title. */
  char usage[AZ_USAGE_MAX]; /**< Description of Amiibo behavior in the game. */
  bool writes; /**< Whether the game writes data back to the Amiibo. */
} AzGame;

/** @brief Catalog entry for a saved NFC figure file. */
typedef struct {
  char filename[96];              /**< Filename of the saved NFC record. */
  char display_name[AZ_NAME_MAX]; /**< Display name derived from the saved file.
                                   */
  uint8_t id[8]; /**< Figure identifier extracted from the saved record. */
  bool valid;    /**< Whether the saved record was successfully recognized as an
                    Amiibo. */
} AzSavedEntry;

/** @brief Catalog entry for a Lock-On SRAM payload. */
typedef struct {
  char filename[96];              /**< Filename of the Lock-On payload. */
  char display_name[AZ_NAME_MAX]; /**< Display label derived from the payload
                                     filename. */
  uint8_t
      source_size; /**< Original payload size encoded by the Lock-On file. */
} AzLockOnEntry;

/** @brief Loaded retail key material used for Amiibo cryptographic operations.
 */
typedef struct {
  uint8_t data_key[80];   /**< Retail data-key block. */
  uint8_t static_key[80]; /**< Retail static/tag-key block. */
  bool valid; /**< Whether both required key blocks were loaded successfully. */
} AzKeys;

/** @brief Decoded user and application metadata from an Amiibo dump. */
typedef struct {
  bool available; /**< Whether detailed metadata could be decoded. */
  bool
      initialized; /**< Whether the Amiibo owner/profile area is initialized. */
  bool app_data_initialized; /**< Whether the application-specific data area is
                                initialized. */
  char nickname[48];         /**< Decoded Amiibo nickname. */
  char owner_mii[48];        /**< Decoded owner Mii name. */
  char init_date[12];        /**< Formatted initialization date. */
  char write_date[12];       /**< Formatted last-write date. */
  uint16_t write_counter;    /**< Tag-level write counter. */
  uint64_t application_id;   /**< Application identifier stored by the game data
                                area. */
  uint32_t application_area_id;       /**< Application-area identifier. */
  uint16_t application_write_counter; /**< Application-area write counter. */
} AzAmiiboDetails;

/** @brief Application screens addressable by the UI state machine. */
typedef enum {
  AzScreenHome,          /**< Home menu. */
  AzScreenCategories,    /**< Category browser. */
  AzScreenFigures,       /**< Figure browser for the selected category. */
  AzScreenSearchResults, /**< Figure search results. */
  AzScreenFigure,        /**< Figure details and actions. */
  AzScreenDumpInfo,      /**< Decoded saved-dump details. */
  AzScreenSaved,         /**< Saved figure catalog. */
  AzScreenAdvanced,      /**< Advanced actions menu. */
  AzScreenLockOn,        /**< Lock-On payload selector. */
  AzScreenGames,         /**< Game compatibility details. */
  AzScreenEmulate,       /**< Active NFC emulation view. */
  AzScreenStatus,        /**< Setup and database status. */
  AzScreenAbout,         /**< Application information. */
  AzScreenConfirmDelete, /**< Saved-file deletion confirmation. */
  AzScreenWorking,       /**< Background database progress view. */
  AzScreenCount,         /**< Number of screen states. */
} AzScreen;

/** @brief Purpose assigned to the shared text-input view. */
typedef enum {
  AzTextInputSearch, /**< Text input is collecting a search query. */
  AzTextInputRename, /**< Text input is collecting a replacement saved name. */
} AzTextInputMode;

/** @brief Deferred operation to perform after a Lock-On payload is selected. */
typedef enum {
  AzLockOnActionNone,              /**< No deferred Lock-On operation. */
  AzLockOnActionGenerateTemporary, /**< Generate a temporary emulation using the
                                      selected Lock-On data. */
  AzLockOnActionGeneratePersistent, /**< Generate a persistent saved emulation
                                       using the selected Lock-On data. */
  AzLockOnActionEmulateSaved, /**< Emulate the current saved figure with the
                                 selected Lock-On data. */
  AzLockOnActionReplaceSaved, /**< Replace the current saved figure companion
                                 Lock-On data. */
} AzLockOnAction;

/** @brief High-level phases reported while validating or rebuilding the
 * database index. */
typedef enum {
  AzDbProgressChecking,   /**< Checking source and index state. */
  AzDbProgressAmiibo,     /**< Reading and indexing Amiibo metadata. */
  AzDbProgressSorting,    /**< Sorting figure index records. */
  AzDbProgressGames,      /**< Reading and indexing game references. */
  AzDbProgressFinalizing, /**< Writing and installing the completed index. */
  AzDbProgressDone,       /**< Database preparation is complete. */
} AzDbProgressStage;

/**
 * @brief Callback receiving database preparation stage and percentage updates.
 * @param context Caller-owned callback context.
 * @param stage Current database preparation phase.
 * @param percent Completion percentage from 0 through 100.
 */
typedef void (*AzDbProgressCallback)(void *context, AzDbProgressStage stage,
                                     uint8_t percent);

/** @brief Alias for the application runtime-state structure. */
typedef struct AmiiboZeroApp AmiiboZeroApp;

/** @brief Snapshot of application state consumed by the drawing callback. */
typedef struct {
  AmiiboZeroApp *app; /**< Application instance backing this render snapshot. */
  AzScreen screen;    /**< Screen being rendered. */
  uint16_t selection; /**< Absolute selection ordinal for the active screen. */
  uint16_t count;     /**< Total selectable items for the active screen. */
  uint16_t window_start; /**< Absolute ordinal represented by the first visible
                            list row. */
  AzCategory category_rows[AZ_LIST_ROWS]; /**< Visible category rows. */
  AzFigure
      figure_rows[AZ_LIST_ROWS]; /**< Visible figure or search-result rows. */
  uint8_t row_count;             /**< Number of populated visible rows. */
  char query[AZ_QUERY_MAX];      /**< Current search query. */
  AzCategory category;           /**< Currently selected category metadata. */
  AzFigure figure;               /**< Currently selected figure metadata. */
  bool figure_saved; /**< Whether the selected figure came from a saved NFC
                        file. */
  char saved_filename[96];    /**< Filename associated with the selected saved
                                 figure. */
  AzGame games[AZ_LIST_ROWS]; /**< Visible game compatibility rows. */
  uint16_t games_total;       /**< Total loaded game compatibility entries. */
  char status_line[64];       /**< Transient header/status text. */
  uint8_t animation;   /**< Animation tick used by scrolling UI elements. */
  uint8_t db_progress; /**< Current database preparation percentage. */
  AzDbProgressStage
      db_progress_stage; /**< Current database preparation phase. */
} AzViewModel;

/** @brief Complete runtime state for the Amiibo Zero application. */
struct AmiiboZeroApp {
  Gui *gui;         /**< Opened GUI service record. */
  Storage *storage; /**< Opened storage service record. */
  ViewDispatcher
      *dispatcher;        /**< View dispatcher controlling application views. */
  View *main_view;        /**< Primary custom-rendered application view. */
  TextInput *text_input;  /**< Shared text-input module. */
  ByteInput *byte_input;  /**< Shared byte-input module used for manual IDs. */
  FuriString *ui_scratch; /**< Reusable string buffer for fitted and scrolling
                             UI text. */

  AzScreen screen; /**< Currently active application screen. */
  uint16_t screen_selection[AzScreenCount]; /**< Remembered selection ordinal
                                               for each screen. */
  AzScreen return_screen; /**< Screen restored after leaving a detail or worker
                             view. */
  uint16_t return_selection; /**< Selection restored with the return screen. */
  uint16_t selection;        /**< Current selection ordinal. */
  uint16_t list_count; /**< Number of selectable items on the active screen. */
  uint16_t detail_scroll;   /**< Vertical wrapped-text scroll offset for detail
                               views. */
  char query[AZ_QUERY_MAX]; /**< Current figure search query. */
  char text_buffer[AZ_NAME_MAX]; /**< Shared editable text buffer for search and
                                    rename operations. */
  AzTextInputMode text_input_mode; /**< Operation currently assigned to the
                                      shared text-input view. */
  uint8_t manual_id[8]; /**< Eight-byte figure ID entered through the byte-input
                           view. */

  AzCategory current_category; /**< Category currently open in the browser. */
  AzFigure current_figure;     /**< Figure currently open in the detail view. */
  bool current_is_saved; /**< Whether the current figure is backed by a saved
                            NFC file. */
  char current_saved_filename[96]; /**< Filename backing the current saved
                                      figure. */
  AzAmiiboDetails
      current_details; /**< Decoded details for the current saved figure. */

  AzGame *game_entries; /**< Dynamically allocated compatibility entries for the
                           current figure. */
  uint16_t game_count;  /**< Number of loaded compatibility entries. */
  AzSavedEntry
      *saved_entries;   /**< Dynamically allocated saved-figure catalog. */
  uint16_t saved_count; /**< Number of saved-figure catalog entries. */
  AzLockOnEntry
      *lockon_entries;   /**< Dynamically allocated Lock-On payload catalog. */
  uint16_t lockon_count; /**< Number of Lock-On catalog entries. */
  uint8_t current_lockon_sram[AZ_LOCKON_SRAM_SIZE]; /**< Lock-On SRAM payload
                                                       selected for the current
                                                       figure. */
  bool current_lockon_valid; /**< Whether a current Lock-On SRAM payload is
                                loaded. */
  char current_lockon_filename[96]; /**< Filename of the current Lock-On
                                       payload. */
  AzLockOnAction
      lockon_action; /**< Deferred action awaiting Lock-On selection. */

  AzKeys keys;          /**< Loaded retail key material. */
  bool index_ready;     /**< Whether the database index is ready for queries. */
  uint32_t index_count; /**< Number of figures reported by the current index. */

  FuriThread
      *db_thread; /**< Background thread used for database preparation. */
  volatile bool
      db_thread_done;    /**< Whether the database worker has finished. */
  bool db_thread_result; /**< Success result returned by the database worker. */
  uint32_t
      db_thread_count; /**< Figure count produced by the database worker. */
  bool
      db_thread_force; /**< Whether the worker should force an index rebuild. */
  AzScreen db_thread_return_screen; /**< Screen to restore after database
                                       preparation. */
  volatile uint8_t db_progress; /**< Latest database preparation percentage. */
  volatile AzDbProgressStage
      db_progress_stage; /**< Latest database preparation phase. */

  Nfc *nfc; /**< NFC service instance used for emulation. */
  NfcDevice
      *nfc_device; /**< Mutable NFC device image for the current Amiibo. */
  NfcListener *listener;   /**< Active NFC listener while emulating. */
  BitBuffer *v3_tx_buffer; /**< Reusable transmit buffer for version-3 listener
                              responses. */
  bool v3_i2c_listener;    /**< Whether version-3 listener state is operating in
                              the I2C-style memory mode. */
  bool v3_sector_select_pending; /**< Whether a version-3 sector-select command
                                    awaits its second frame. */
  bool v3_authenticated; /**< Whether the current version-3 interaction has
                            authenticated successfully. */
  bool
      v3_sram_ready; /**< Whether version-3 SRAM content is ready for access. */
  uint8_t v3_sector; /**< Currently selected version-3 logical sector. */
  bool emulating;    /**< Whether NFC emulation is currently active. */
  bool emulation_persistent; /**< Whether mutable emulation state should be
                                saved on stop. */
  char emulation_path[AZ_PATH_MAX]; /**< Backing file path for persistent
                                       emulation. */
  char toast[64];                   /**< Temporary UI notification text. */
  uint8_t toast_ticks; /**< Remaining display ticks for the current toast. */
  uint8_t animation;   /**< Global UI animation tick. */
};

/**
 * @brief Run the Amiibo Zero application.
 * @param p Optional launch context supplied by the Flipper application loader.
 * @return Application exit status.
 */
int32_t amiibo_zero_app(void *p);
