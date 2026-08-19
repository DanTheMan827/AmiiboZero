/**
 * @file amiibo_db.c
 * @brief Indexed Amiibo metadata database access.
 * @details Builds and validates the compact on-device index, streams JSON sources, and serves category, figure, search, and game queries.
 */

#include "amiibo_db.h"
#include "./third_party/lwjson/lwjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <toolbox/crc32_calc.h>
#include <furi/core/memmgr.h>

/** @brief Constant used for JSON file buffer. */
#define AZ_JSON_FILE_BUFFER 2048U

/** @brief Constant used for JSON range buffer. */
#define AZ_JSON_RANGE_BUFFER 512U

/** @brief Constant used for sort run records. */
#define AZ_SORT_RUN_RECORDS 128U

/** @brief Constant used for sort batch target records. */
#define AZ_SORT_BATCH_TARGET_RECORDS 512U

/** @brief Number of figure records stored in each fast-sort heap slab. */
#define AZ_SORT_BATCH_CHUNK_RECORDS 8U

/** @brief Minimum heap headroom retained while growing enough slabs for the largest category. */
#define AZ_SORT_HEAP_MIN_RESERVE 2048U

/** @brief Preferred heap headroom retained once the largest category fits in RAM. */
#define AZ_SORT_HEAP_PREFERRED_RESERVE 6144U

/** @brief Constant used for sort batch scan records. */
#define AZ_SORT_BATCH_SCAN_RECORDS 8U

/** @brief Constant used for sort batch write records. */
#define AZ_SORT_BATCH_WRITE_RECORDS 32U

/** @brief Constant used for copy buffer. */
#define AZ_COPY_BUFFER 4096U

/** @brief Constant used for source sample bytes. */
#define AZ_SOURCE_SAMPLE_BYTES 256U
/** @brief Constant used for index magic. */
#define AZ_INDEX_MAGIC "AZIDX34"
/** @brief Constant used for index version. */
#define AZ_INDEX_VERSION 10U

/** @brief Remove the active index and all index-build artifacts. */
void az_db_remove_index_files(Storage* storage) {
    if(!storage) return;
    storage_common_remove(storage, AZ_INDEX_FILE);
    storage_common_remove(storage, AZ_INDEX_TMP);
    storage_common_remove(storage, AZ_INDEX_BACKUP);
    storage_common_remove(storage, AZ_INDEX_RAW);
    storage_common_remove(storage, AZ_INDEX_SORT_RUNS);
}

/** @brief Compact fingerprint used to detect changes in a JSON source file. */
typedef struct {
    uint64_t size; /**< Source file size in bytes. */
    uint32_t sample_crc32; /**< CRC-32 of sampled source bytes. */
    uint32_t reserved; /**< Reserved field retained for binary-format stability. */
} AzSourceStamp;

/** @brief Binary index header describing format, source fingerprints, counts, and section offsets. */
typedef struct {
    char magic[8]; /**< Index format signature. */
    uint16_t version; /**< Binary index format version. */
    uint16_t header_size; /**< Serialized header size in bytes. */
    uint16_t category_record_size; /**< Serialized category-record size in bytes. */
    uint16_t figure_record_size; /**< Serialized figure-record size in bytes. */
    uint16_t game_record_size; /**< Serialized game-reference-record size in bytes. */
    uint16_t reserved0; /**< Reserved header field. */
    AzSourceStamp amiibo_source; /**< Fingerprint of the Amiibo JSON source. */
    AzSourceStamp games_source; /**< Fingerprint of the games JSON source. */
    uint32_t figure_count; /**< Number of figure records in the index. */
    uint32_t game_ref_count; /**< Number of indexed game-reference records. */
    uint16_t category_count; /**< Number of category records in the index. */
    uint16_t reserved1; /**< Reserved header field. */
    uint32_t categories_offset; /**< Byte offset of the category section. */
    uint32_t figures_offset; /**< Byte offset of the figure section. */
    uint32_t games_offset; /**< Byte offset of the game-reference section. */
} AzIndexHeader;

/** @brief Indexed category record with the ordinal of its first figure. */
typedef struct {
    AzCategory category; /**< Serialized category metadata. */
    uint32_t first_figure; /**< Ordinal of the first figure in this category. */
} AzIndexCategoryRecord;

/** @brief Serialized figure record stored in the database index. */
typedef struct {
    AzFigure figure; /**< Serialized figure metadata. */
} AzIndexFigureRecord;

/** @brief Reference to a game JSON object keyed by an Amiibo identifier pattern. */
typedef struct {
    uint8_t pattern[8]; /**< Eight-byte identifier pattern used to match Amiibo figures. */
    uint32_t json_offset; /**< Byte offset of the referenced game JSON object. */
    uint32_t json_length; /**< Byte length of the referenced game JSON object. */
} AzIndexGameRef;

/** @brief Shared stop and failure state for streaming JSON scans. */
typedef struct {
    bool stop; /**< Whether the streaming scan should stop early. */
    bool failed; /**< Whether a parser or file operation failed. */
    uint32_t current_offset; /**< Current absolute source offset reported by the scanner. */
} AzScanControl;

/** @brief State used to map byte progress into a database progress range. */
typedef struct {
    AzDbProgressCallback callback; /**< Progress callback supplied by the caller. */
    void* context; /**< Context passed to the progress callback. */
    AzDbProgressStage stage; /**< Database phase represented by this reporter. */
    uint8_t start_percent; /**< Percentage at the start of the mapped phase. */
    uint8_t end_percent; /**< Percentage at the end of the mapped phase. */
    uint32_t total_bytes; /**< Source byte count used for percentage scaling. */
    uint8_t last_percent; /**< Last percentage emitted to suppress duplicates. */
} AzProgressReporter;

/** @brief Incremental decoder state for escaped JSON string fragments. */
typedef struct {
    bool escaped; /**< Whether the next character follows a JSON escape marker. */
    uint8_t unicode_left; /**< Number of hexadecimal digits still expected for a Unicode escape. */
    uint16_t unicode; /**< Unicode code point currently being assembled. */
} AzStringDecoder;

/** @brief Streaming-parser state used while constructing figure index records. */
typedef struct {
    AzScanControl control; /**< Shared streaming-scan control state. */
    File* raw_file; /**< Temporary destination for unsorted figure records. */
    AzCategory* categories; /**< In-memory category table. */
    uint16_t category_capacity; /**< Capacity of the category table. */
    uint16_t* category_count; /**< Pointer to the current category count. */
    AzFigure current; /**< Figure record currently being assembled. */
    bool in_figure; /**< Whether parser state is inside an Amiibo figure object. */
    uint32_t object_start; /**< Source offset where the current figure object begins. */
    uint32_t figure_count; /**< Number of figure records emitted so far. */
    AzStringDecoder decoder; /**< Incremental JSON string decoder state. */
} AzFigureBuildContext;

/** @brief Streaming-parser state used while constructing game-reference records. */
typedef struct {
    AzScanControl control; /**< Shared streaming-scan control state. */
    File* output_file; /**< Destination file for game-reference records. */
    bool in_entry; /**< Whether parser state is inside a game-reference entry. */
    uint8_t pattern[8]; /**< Identifier pattern being assembled. */
    uint32_t object_start; /**< Source offset where the current game object begins. */
    uint32_t count; /**< Number of game references emitted so far. */
} AzGameIndexBuildContext;

/** @brief Game usage text paired with whether the interaction writes tag data. */
typedef struct {
    char text[AZ_USAGE_MAX]; /**< Compatibility usage text. */
    bool writes; /**< Whether this usage writes data back to the Amiibo. */
} AzUsage;

/** @brief Streaming-parser state used to materialize game compatibility records from a JSON range. */
typedef struct {
    AzScanControl control; /**< Shared streaming-scan control state. */
    AzGame* games; /**< Destination array for parsed compatibility records. */
    uint16_t max_games; /**< Capacity of the destination game array. */
    uint16_t count; /**< Number of destination records populated. */
    bool in_game; /**< Whether parser state is inside a game object. */
    bool in_usage; /**< Whether parser state is inside a usage object. */
    char platform[9]; /**< Current platform label. */
    char game_name[AZ_NAME_MAX]; /**< Current game title. */
    AzUsage usage; /**< Usage record currently being assembled. */
    AzUsage pending[8]; /**< Usage records held until game context is complete. */
    uint8_t pending_count; /**< Number of pending usage records. */
    bool saw_usage; /**< Whether the current game contained an explicit usage section. */
    AzStringDecoder decoder; /**< Incremental JSON string decoder state. */
} AzGamesRangeContext;

/**
 * @brief Convert an ASCII uppercase letter to lowercase.
 * @param c ASCII character or byte to inspect.
 * @return The computed result value.
 */
static char az_ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/**
 * @brief Compare two ASCII strings without case sensitivity.
 * @param left Left-side input value.
 * @param right Right-side input value.
 * @return The computed result value.
 */
static int az_compare_nocase(const char* left, const char* right) {
    if(!left) left = "";
    if(!right) right = "";
    while(*left && *right) {
        char a = az_ascii_lower(*left++);
        char b = az_ascii_lower(*right++);
        if(a < b) return -1;
        if(a > b) return 1;
    }
    return *left ? 1 : *right ? -1 : 0;
}

/**
 * @brief Test whether one ASCII string contains another without case sensitivity.
 * @param haystack Text to search within.
 * @param needle Text to search for.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_contains_nocase(const char* haystack, const char* needle) {
    if(!needle || !needle[0]) return true;
    if(!haystack) return false;
    const size_t nlen = strlen(needle);
    for(const char* h = haystack; *h; ++h) {
        size_t i = 0;
        while(i < nlen && h[i] && az_ascii_lower(h[i]) == az_ascii_lower(needle[i])) i++;
        if(i == nlen) return true;
    }
    return false;
}

/**
 * @brief Convert one hexadecimal character to its numeric nibble value.
 * @param c ASCII character or byte to inspect.
 * @return Nibble value from 0 through 15, or -1 for a non-hexadecimal character.
 */
static int az_hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief Parse two hexadecimal characters into one byte.
 * @param text Text to process or display.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_parse_hex_byte(const char* text, uint8_t* out) {
    if(!text || !out) return false;
    if(text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    int hi = az_hex_nibble(text[0]);
    int lo = az_hex_nibble(text[1]);
    if(hi < 0 || lo < 0 || text[2] != '\0') return false;
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}

/**
 * @brief Parse a textual Amiibo identifier into binary and canonical hexadecimal forms.
 * @param text Text to process or display.
 * @param out Destination for the computed result.
 * @param out_hex Destination for the resulting hex.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_parse_id_text(const char* text, uint8_t out[8], char out_hex[17]) {
    if(!text) return false;
    if(text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if(strlen(text) != 16) return false;
    static const char hex[] = "0123456789abcdef";
    for(size_t i = 0; i < 8; i++) {
        int hi = az_hex_nibble(text[i * 2]);
        int lo = az_hex_nibble(text[i * 2 + 1]);
        if(hi < 0 || lo < 0) return false;
        uint8_t value = (uint8_t)((hi << 4) | lo);
        if(out) out[i] = value;
        if(out_hex) {
            out_hex[i * 2] = hex[value >> 4];
            out_hex[i * 2 + 1] = hex[value & 0x0F];
        }
    }
    if(out_hex) out_hex[16] = '\0';
    return true;
}

/**
 * @brief Normalize a parsed text field for display and indexing.
 * @param text Text to process or display.
 */
static void az_clean_field(char* text) {
    if(!text) return;
    for(char* p = text; *p; p++) {
        if(*p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
    }
}

/**
 * @brief Return a JSON object-key fragment from the streaming parser stack.
 * @param parser Streaming JSON parser state.
 * @param ordinal Zero-based record or parser-stack ordinal.
 * @return Pointer to the selected text, or NULL when no text is available.
 */
static const char* az_lw_key(const lwjson_stream_parser_t* parser, size_t ordinal) {
    if(!parser) return NULL;
    for(size_t i = parser->stack_pos; i > 0; i--) {
        const lwjson_stream_stack_t* item = &parser->stack[i - 1];
        if(item->type != LWJSON_STREAM_TYPE_KEY) continue;
        if(ordinal == 0) return item->meta.name;
        ordinal--;
    }
    return NULL;
}

/**
 * @brief Test a streaming-parser object key against an expected key name.
 * @param parser Streaming JSON parser state.
 * @param ordinal Zero-based record or parser-stack ordinal.
 * @param key Cryptographic key bytes.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_lw_key_is(const lwjson_stream_parser_t* parser, size_t ordinal, const char* key) {
    const char* actual = az_lw_key(parser, ordinal);
    return actual && key && strcmp(actual, key) == 0;
}

/**
 * @brief Determine the source offset of the current streamed JSON string fragment.
 * @param parser Streaming JSON parser state.
 * @return The computed result value.
 */
static size_t az_lw_string_piece_offset(const lwjson_stream_parser_t* parser) {
    return parser->data.str.buff_total_pos >= parser->data.str.buff_pos ?
               parser->data.str.buff_total_pos - parser->data.str.buff_pos :
               0;
}

/**
 * @brief Append one character to a bounded text buffer.
 * @param destination Destination object or buffer.
 * @param capacity Maximum number of elements or bytes available.
 * @param position Current write position in the destination buffer.
 * @param value Value to process or transmit.
 */
static void az_text_append(char* destination, size_t capacity, size_t* position, char value) {
    if(!destination || !position || capacity == 0 || *position + 1 >= capacity) return;
    destination[(*position)++] = value;
    destination[*position] = '\0';
}

/**
 * @brief Encode and append one Unicode code point as UTF-8.
 * @param destination Destination object or buffer.
 * @param capacity Maximum number of elements or bytes available.
 * @param position Current write position in the destination buffer.
 * @param codepoint Unicode code point to encode.
 */
static void az_text_append_codepoint(
    char* destination,
    size_t capacity,
    size_t* position,
    uint16_t codepoint) {
    if(codepoint < 0x80) {
        az_text_append(destination, capacity, position, (char)codepoint);
    } else if(codepoint < 0x800) {
        az_text_append(destination, capacity, position, (char)(0xC0 | (codepoint >> 6)));
        az_text_append(destination, capacity, position, (char)(0x80 | (codepoint & 0x3F)));
    } else if(codepoint < 0xD800 || codepoint > 0xDFFF) {
        az_text_append(destination, capacity, position, (char)(0xE0 | (codepoint >> 12)));
        az_text_append(destination, capacity, position, (char)(0x80 | ((codepoint >> 6) & 0x3F)));
        az_text_append(destination, capacity, position, (char)(0x80 | (codepoint & 0x3F)));
    }
}

/**
 * @brief Decode and append the current streamed JSON string fragment.
 * @param parser Streaming JSON parser state.
 * @param destination Destination object or buffer.
 * @param capacity Maximum number of elements or bytes available.
 * @param decoder Incremental JSON string-decoder state.
 */
static void az_copy_stream_string(
    const lwjson_stream_parser_t* parser,
    char* destination,
    size_t capacity,
    AzStringDecoder* decoder) {
    if(!parser || !destination || !decoder || capacity == 0) return;
    size_t piece_offset = az_lw_string_piece_offset(parser);
    size_t output_pos = piece_offset == 0 ? 0 : strlen(destination);
    if(piece_offset == 0) {
        destination[0] = '\0';
        memset(decoder, 0, sizeof(*decoder));
    }
    for(size_t i = 0; i < parser->data.str.buff_pos; i++) {
        char c = parser->data.str.buff[i];
        if(decoder->unicode_left) {
            int nibble = az_hex_nibble(c);
            if(nibble < 0) {
                decoder->unicode_left = 0;
                decoder->unicode = 0;
                az_text_append(destination, capacity, &output_pos, '?');
                continue;
            }
            decoder->unicode = (uint16_t)((decoder->unicode << 4) | (uint16_t)nibble);
            decoder->unicode_left--;
            if(decoder->unicode_left == 0) az_text_append_codepoint(destination, capacity, &output_pos, decoder->unicode);
            continue;
        }
        if(decoder->escaped) {
            decoder->escaped = false;
            if(c == 'u') {
                decoder->unicode_left = 4;
                decoder->unicode = 0;
                continue;
            }
            switch(c) {
            case '"':
            case '\\':
            case '/': az_text_append(destination, capacity, &output_pos, c); break;
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't': az_text_append(destination, capacity, &output_pos, ' '); break;
            default: az_text_append(destination, capacity, &output_pos, '?'); break;
            }
            continue;
        }
        if(c == '\\') decoder->escaped = true;
        else az_text_append(destination, capacity, &output_pos, c);
    }
}

/**
 * @brief Write an entire buffer to a file.
 * @param file Open storage file.
 * @param data Data buffer or tag state used by the operation.
 * @param size Number of bytes in the supplied buffer or file.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_write_exact(File* file, const void* data, size_t size) {
    return file && data && storage_file_write(file, data, size) == size;
}

/**
 * @brief Read an exact number of bytes from a file.
 * @param file Open storage file.
 * @param data Data buffer or tag state used by the operation.
 * @param size Number of bytes in the supplied buffer or file.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_read_exact(File* file, void* data, size_t size) {
    return file && data && storage_file_read(file, data, size) == size;
}

/**
 * @brief Report a bounded database progress value to a callback.
 * @param callback Callback invoked while processing data.
 * @param context Caller-owned callback context.
 * @param stage Database progress phase.
 * @param percent Progress percentage in the range 0 through 100.
 */
static void az_progress_emit(
    AzDbProgressCallback callback,
    void* context,
    AzDbProgressStage stage,
    uint8_t percent) {
    if(percent > 100U) percent = 100U;
    if(callback) callback(context, stage, percent);
}

/**
 * @brief Scale a bounded 32-bit value without promoting the division to 64 bits.
 * @details Progress spans are at most 100. Values are shifted together until value * scale
 *          cannot overflow uint32_t, preserving the ratio closely enough for UI percentages.
 */
static uint32_t az_progress_scale_u32(uint32_t value, uint32_t total, uint32_t scale) {
    if(total == 0U || scale == 0U) return 0U;
    if(value >= total) return scale;
    while(total > UINT32_MAX / scale) {
        total = (total + 1U) >> 1U;
        value = (value + 1U) >> 1U;
    }
    return (value * scale) / total;
}

/**
 * @brief Initialize byte-based progress mapping for one database stage.
 * @param reporter Byte-progress reporter state.
 * @param callback Callback invoked while processing data.
 * @param context Caller-owned callback context.
 * @param stage Database progress phase.
 * @param start_percent Progress percentage assigned to the start of the operation.
 * @param end_percent Progress percentage assigned to the end of the operation.
 * @param total_bytes Total byte count used for progress scaling.
 */
static void az_progress_reporter_init(
    AzProgressReporter* reporter,
    AzDbProgressCallback callback,
    void* context,
    AzDbProgressStage stage,
    uint8_t start_percent,
    uint8_t end_percent,
    uint32_t total_bytes) {
    if(!reporter) return;
    reporter->callback = callback;
    reporter->context = context;
    reporter->stage = stage;
    reporter->start_percent = start_percent;
    reporter->end_percent = end_percent >= start_percent ? end_percent : start_percent;
    reporter->total_bytes = total_bytes;
    reporter->last_percent = 0xFFU;
    az_progress_emit(callback, context, stage, start_percent);
    reporter->last_percent = start_percent;
}

/**
 * @brief Update byte-based database progress after input is consumed.
 * @param reporter Byte-progress reporter state.
 * @param consumed Number of source bytes consumed so far.
 */
static void az_progress_report_bytes(AzProgressReporter* reporter, uint32_t consumed) {
    if(!reporter || !reporter->callback) return;
    uint8_t percent = reporter->end_percent;
    if(reporter->total_bytes > 0U && consumed < reporter->total_bytes) {
        uint32_t span = (uint32_t)(reporter->end_percent - reporter->start_percent);
        uint32_t scaled = az_progress_scale_u32(consumed, reporter->total_bytes, span);
        percent = (uint8_t)(reporter->start_percent + scaled);
    }
    if(percent != reporter->last_percent) {
        reporter->last_percent = percent;
        az_progress_emit(reporter->callback, reporter->context, reporter->stage, percent);
    }
}

/**
 * @brief Stream an entire JSON file through an lwjson callback.
 * @param storage Storage service used for file operations.
 * @param path Filesystem path.
 * @param callback Callback invoked while processing data.
 * @param callback_context Context passed to the streaming parser callback.
 * @param control Mutable scan control state.
 * @param progress Optional byte-progress reporter.
 * @param source_size Destination for the source file size.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_scan_json_file(
    Storage* storage,
    const char* path,
    lwjson_stream_parser_callback_fn callback,
    void* callback_context,
    AzScanControl* control,
    AzProgressReporter* progress,
    uint64_t* source_size) {
    if(!storage || !path || !callback || !control) return false;
    control->stop = false;
    control->failed = false;
    control->current_offset = 0;
    if(source_size) *source_size = 0;

    File* file = storage_file_alloc(storage);
    if(!file || !storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(file) storage_file_free(file);
        return false;
    }
    uint64_t expected_size = storage_file_size(file);
    if(expected_size > UINT32_MAX) {
        storage_file_close(file);
        storage_file_free(file);
        return false;
    }

    lwjson_stream_parser_t* parser = malloc(sizeof(lwjson_stream_parser_t));
    uint8_t* buffer = malloc(AZ_JSON_FILE_BUFFER);
    if(!parser || !buffer) {
        free(buffer);
        free(parser);
        storage_file_close(file);
        storage_file_free(file);
        return false;
    }

    bool ok = true;
    if(ok) {
        ok = lwjson_stream_init(parser, callback) == lwjsonOK &&
             lwjson_stream_set_user_data(parser, callback_context) == lwjsonOK;
    }

    bool done = false;
    uint32_t absolute = 0;
    uint64_t total_read = 0;
    while(ok && !control->stop) {
        size_t read = storage_file_read(file, buffer, AZ_JSON_FILE_BUFFER);
        if(read == 0) break;
        total_read += read;
        if(total_read > expected_size) {
            ok = false;
            break;
        }
        for(size_t i = 0; i < read; i++) {
            control->current_offset = absolute;
            lwjsonr_t result = lwjson_stream_parse(parser, (char)buffer[i]);
            absolute++;
            if(result == lwjsonSTREAMDONE) {
                done = true;
                control->stop = true;
                break;
            }
            if(result != lwjsonSTREAMINPROG && result != lwjsonSTREAMWAITFIRSTCHAR) {
                ok = false;
                control->stop = true;
                break;
            }
            if(control->stop) break;
        }
        az_progress_report_bytes(progress, (uint32_t)total_read);
    }

    if(control->failed) ok = false;
    if(!done) ok = false;
    if(ok && source_size) *source_size = expected_size;
    if(ok && done) az_progress_report_bytes(progress, progress ? progress->total_bytes : absolute);

    free(buffer);
    free(parser);
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Stream a bounded JSON byte range through an lwjson callback.
 * @param storage Storage service used for file operations.
 * @param path Filesystem path.
 * @param offset Byte offset where the range begins.
 * @param length Number of bytes to process.
 * @param callback Callback invoked while processing data.
 * @param callback_context Context passed to the streaming parser callback.
 * @param control Mutable scan control state.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_scan_json_range(
    Storage* storage,
    const char* path,
    uint32_t offset,
    uint32_t length,
    lwjson_stream_parser_callback_fn callback,
    void* callback_context,
    AzScanControl* control) {
    if(!storage || !path || !callback || !control || length == 0) return false;
    control->stop = false;
    control->failed = false;
    control->current_offset = offset;
    File* file = storage_file_alloc(storage);
    if(!file || !storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(file) storage_file_free(file);
        return false;
    }
    bool ok = storage_file_seek(file, offset, true);
    lwjson_stream_parser_t* parser = ok ? malloc(sizeof(lwjson_stream_parser_t)) : NULL;
    if(!parser) ok = false;
    if(ok) {
        ok = lwjson_stream_init(parser, callback) == lwjsonOK &&
             lwjson_stream_set_user_data(parser, callback_context) == lwjsonOK;
    }
    bool done = false;
    uint8_t buffer[AZ_JSON_RANGE_BUFFER];
    uint32_t consumed = 0;
    while(ok && consumed < length && !control->stop) {
        size_t wanted = length - consumed;
        if(wanted > sizeof(buffer)) wanted = sizeof(buffer);
        size_t read = storage_file_read(file, buffer, wanted);
        if(read == 0) break;
        for(size_t i = 0; i < read; i++) {
            control->current_offset = offset + consumed;
            lwjsonr_t result = lwjson_stream_parse(parser, (char)buffer[i]);
            consumed++;
            if(result == lwjsonSTREAMDONE) {
                done = true;
                control->stop = true;
                break;
            }
            if(result != lwjsonSTREAMINPROG && result != lwjsonSTREAMWAITFIRSTCHAR) {
                ok = false;
                control->stop = true;
                break;
            }
        }
    }
    if(control->failed) ok = false;
    if(!done) ok = false;
    if(parser) free(parser);
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Find a category record by identifier in an in-memory array.
 * @param categories Category record array.
 * @param count Number of records or elements.
 * @param id Eight-byte Amiibo identifier.
 * @return Pointer to the requested object, or NULL when it is not available.
 */
static AzCategory* az_find_category(AzCategory* categories, uint16_t count, uint8_t id) {
    for(uint16_t i = 0; i < count; i++) {
        if(categories[i].id == id) return &categories[i];
    }
    return NULL;
}

/**
 * @brief Add a placeholder category when input references an unknown category ID.
 * @param categories Category record array.
 * @param capacity Maximum number of elements or bytes available.
 * @param count Number of records or elements.
 * @param id Eight-byte Amiibo identifier.
 * @return Pointer to the requested object, or NULL when it is not available.
 */
static AzCategory* az_add_unknown_category(
    AzCategory* categories,
    uint16_t capacity,
    uint16_t* count,
    uint8_t id) {
    if(!categories || !count || *count >= capacity) return NULL;
    AzCategory* category = &categories[(*count)++];
    memset(category, 0, sizeof(*category));
    category->id = id;
    snprintf(category->name, sizeof(category->name), "Series %02X", id);
    return category;
}

/**
 * @brief Compare category records for display ordering.
 * @param left Left comparison operand.
 * @param right Right comparison operand.
 * @return A negative, zero, or positive value when the left operand sorts before, equal to, or after the right operand.
 */
static int az_category_compare(const AzCategory* left, const AzCategory* right) {
    int by_name = az_compare_nocase(left->name, right->name);
    if(by_name) return by_name;
    return left->id < right->id ? -1 : left->id > right->id ? 1 : 0;
}

/**
 * @brief Sort category records in display order.
 * @param categories Category record array.
 * @param count Number of records or elements.
 */
static void az_sort_categories(AzCategory* categories, uint16_t count) {
    for(uint16_t i = 1; i < count; i++) {
        AzCategory current = categories[i];
        uint16_t j = i;
        while(j > 0 && az_category_compare(&categories[j - 1], &current) > 0) {
            categories[j] = categories[j - 1];
            j--;
        }
        if(j != i) categories[j] = current;
    }
}

/**
 * @brief Compare figure records for index ordering.
 * @param left Left comparison operand.
 * @param right Right comparison operand.
 * @return A negative, zero, or positive value when the left operand sorts before, equal to, or after the right operand.
 */
static int az_figure_compare(const AzFigure* left, const AzFigure* right) {
    int by_name = az_compare_nocase(left->name, right->name);
    if(by_name) return by_name;
    int by_id = memcmp(left->id, right->id, 8);
    return by_id < 0 ? -1 : by_id > 0 ? 1 : 0;
}

/**
 * @brief Recognize an Amiibo object entry and parse its identifier key.
 * @param parser Streaming JSON parser state.
 * @param id Eight-byte Amiibo identifier.
 * @param id_hex Canonical hexadecimal Amiibo identifier.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_lw_is_amiibo_entry(
    const lwjson_stream_parser_t* parser,
    uint8_t id[8],
    char id_hex[17]) {
    const char* entry = az_lw_key(parser, 0);
    if(!entry || !az_lw_key_is(parser, 1, "amiibos")) return false;
    return az_parse_id_text(entry, id, id_hex);
}

/**
 * @brief Remove a trailing version-3 variant suffix that begins with "(&".
 * @details The name is scanned in place without a temporary buffer. If "(&" occurs and the
 *          figure name ends with ')', every byte from the opening '(' through the end of the suffix is
 *          replaced with NUL. Examples include "(& Tank Star)" and "(& Warp Star)".
 * @param figure Figure whose decoded name may need normalization.
 */
static void az_strip_v3_name_suffix(AzFigure* figure) {
    if(!figure || figure->id[7] != 0x03U) return;

    size_t suffix_start = sizeof(figure->name);
    size_t name_end = 0U;

    for(size_t i = 0; i < sizeof(figure->name); ++i) {
        if(figure->name[i] == '\0') {
            name_end = i;
            break;
        }

        if(suffix_start == sizeof(figure->name) && i + 1U < sizeof(figure->name) &&
           figure->name[i] == '(' && figure->name[i + 1U] == '&') {
            suffix_start = i;
        }
    }

    if(suffix_start == sizeof(figure->name) || name_end == 0U ||
       figure->name[name_end - 1U] != ')') {
        return;
    }

    for(size_t i = suffix_start; i < name_end; ++i) {
        figure->name[i] = '\0';
    }
}

/**
 * @brief Consume one streaming JSON event while building figure records.
 * @param parser Streaming JSON parser state.
 * @param type Type or event code to interpret.
 */
static void az_figure_build_event(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    AzFigureBuildContext* context = lwjson_stream_get_user_data(parser);
    if(!context) return;

    if(type == LWJSON_STREAM_TYPE_STRING && az_lw_key_is(parser, 1, "amiibo_series")) {
        const char* id_text = az_lw_key(parser, 0);
        uint8_t id = 0;
        if(id_text && az_parse_hex_byte(id_text, &id)) {
            AzCategory* category =
                az_find_category(context->categories, *context->category_count, id);
            if(!category) {
                category = az_add_unknown_category(
                    context->categories,
                    context->category_capacity,
                    context->category_count,
                    id);
            }
            if(!category) {
                context->control.failed = true;
                context->control.stop = true;
                return;
            }
            az_copy_stream_string(
                parser, category->name, sizeof(category->name), &context->decoder);
            az_clean_field(category->name);
        }
        return;
    }

    if(type == LWJSON_STREAM_TYPE_OBJECT) {
        uint8_t id[8];
        char hex[17];
        if(az_lw_is_amiibo_entry(parser, id, hex)) {
            memset(&context->current, 0, sizeof(context->current));
            memcpy(context->current.id, id, 8);
            az_str_copy(context->current.id_hex, sizeof(context->current.id_hex), hex);
            context->current.category = id[6];
            context->current.type = id[3];
            context->current.json_offset = context->control.current_offset;
            context->object_start = context->control.current_offset;
            context->in_figure = true;
        }
        return;
    }
    if(type == LWJSON_STREAM_TYPE_STRING && context->in_figure) {
        if(az_lw_key_is(parser, 0, "name") &&
           az_parse_id_text(az_lw_key(parser, 1), NULL, NULL)) {
            az_copy_stream_string(
                parser, context->current.name, sizeof(context->current.name), &context->decoder);
            az_clean_field(context->current.name);
        } else if(az_lw_key_is(parser, 0, "na") && az_lw_key_is(parser, 1, "release")) {
            az_copy_stream_string(
                parser,
                context->current.release_na,
                sizeof(context->current.release_na),
                &context->decoder);
            az_clean_field(context->current.release_na);
        }
        return;
    }
    if(type == LWJSON_STREAM_TYPE_OBJECT_END && context->in_figure) {
        uint8_t id[8];
        if(!az_lw_is_amiibo_entry(parser, id, NULL)) return;
        context->in_figure = false;
        context->current.json_length = context->control.current_offset - context->object_start + 1U;
        if(!context->current.name[0])
            az_str_copy(
                context->current.name,
                sizeof(context->current.name),
                context->current.id_hex);
        az_strip_v3_name_suffix(&context->current);
        AzCategory* category = az_find_category(
            context->categories, *context->category_count, context->current.category);
        if(!category) {
            category = az_add_unknown_category(
                context->categories,
                context->category_capacity,
                context->category_count,
                context->current.category);
        }
        if(!category) {
            context->control.failed = true;
            context->control.stop = true;
            return;
        }
        if(category->count < UINT16_MAX) category->count++;
        AzIndexFigureRecord record = {0};
        record.figure = context->current;
        if(!az_write_exact(context->raw_file, &record, sizeof(record))) {
            context->control.failed = true;
            context->control.stop = true;
            return;
        }
        context->figure_count++;
    }
}

/**
 * @brief Consume one streaming JSON event while building game references.
 * @param parser Streaming JSON parser state.
 * @param type Type or event code to interpret.
 */
static void az_game_index_event(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    AzGameIndexBuildContext* context = lwjson_stream_get_user_data(parser);
    if(!context) return;
    if(type == LWJSON_STREAM_TYPE_OBJECT) {
        const char* entry = az_lw_key(parser, 0);
        if(entry && az_lw_key_is(parser, 1, "amiibos") && az_parse_id_text(entry, context->pattern, NULL)) {
            context->in_entry = true;
            context->object_start = context->control.current_offset;
        }
        return;
    }
    if(type == LWJSON_STREAM_TYPE_OBJECT_END && context->in_entry) {
        uint8_t pattern[8];
        const char* entry = az_lw_key(parser, 0);
        if(!entry || !az_lw_key_is(parser, 1, "amiibos") || !az_parse_id_text(entry, pattern, NULL)) return;
        context->in_entry = false;
        AzIndexGameRef ref;
        memcpy(ref.pattern, context->pattern, sizeof(ref.pattern));
        ref.json_offset = context->object_start;
        ref.json_length = context->control.current_offset - context->object_start + 1U;
        if(!az_write_exact(context->output_file, &ref, sizeof(ref))) {
            context->control.failed = true;
            context->control.stop = true;
            return;
        }
        context->count++;
    }
}

/**
 * @brief Read the byte size of a source file.
 * @param storage Storage service used for file operations.
 * @param path Filesystem path.
 * @param out_size Destination for the resulting size.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_source_size(Storage* storage, const char* path, uint64_t* out_size) {
    if(out_size) *out_size = 0;
    if(!storage || !path || !out_size) return false;
    FileInfo info;
    memset(&info, 0, sizeof(info));
    if(storage_common_stat(storage, path, &info) != FSE_OK || (info.flags & FSF_DIRECTORY) ||
       info.size > UINT32_MAX) {
        return false;
    }
    *out_size = info.size;
    return true;
}

/**
 * @brief Build a compact fingerprint for a source file.
 * @param storage Storage service used for file operations.
 * @param path Filesystem path.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_source_stamp(Storage* storage, const char* path, AzSourceStamp* out) {
    if(!out) return false;
    memset(out, 0, sizeof(*out));
    if(!az_source_size(storage, path, &out->size)) return false;
    if(out->size == 0U) return true;

    File* file = storage_file_alloc(storage);
    if(!file || !storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(file) storage_file_free(file);
        return false;
    }

    uint8_t buffer[AZ_SOURCE_SAMPLE_BYTES];
    uint32_t crc32 = 0U;
    uint32_t offsets[3];
    offsets[0] = 0U;
    offsets[1] = out->size > AZ_SOURCE_SAMPLE_BYTES ?
                     (uint32_t)((out->size - AZ_SOURCE_SAMPLE_BYTES) / 2U) :
                     0U;
    offsets[2] = out->size > AZ_SOURCE_SAMPLE_BYTES ?
                     (uint32_t)(out->size - AZ_SOURCE_SAMPLE_BYTES) :
                     0U;

    bool ok = true;
    uint32_t previous_offset = UINT32_MAX;
    for(size_t i = 0; i < COUNT_OF(offsets); i++) {
        uint32_t offset = offsets[i];
        if(offset == previous_offset) continue;
        previous_offset = offset;
        uint64_t remaining = out->size - offset;
        size_t length = remaining > AZ_SOURCE_SAMPLE_BYTES ? AZ_SOURCE_SAMPLE_BYTES : (size_t)remaining;
        if(!storage_file_seek(file, offset, true) || storage_file_read(file, buffer, length) != length) {
            ok = false;
            break;
        }
        crc32 = crc32_calc_buffer(crc32, &offset, sizeof(offset));
        crc32 = crc32_calc_buffer(crc32, buffer, length);
    }

    uint64_t size_after = 0;
    if(ok) ok = az_source_size(storage, path, &size_after) && size_after == out->size;
    if(ok) out->sample_crc32 = crc32;

    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Compare two source fingerprints.
 * @param left Left-side input value.
 * @param right Right-side input value.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_stamp_equal(const AzSourceStamp* left, const AzSourceStamp* right) {
    return left && right && left->size == right->size &&
           left->sample_crc32 == right->sample_crc32;
}

/**
 * @brief Validate fixed format identifiers and record sizes in an index header.
 * @param header Validated database index header.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_header_shape_valid(const AzIndexHeader* header) {
    return header && memcmp(header->magic, AZ_INDEX_MAGIC, sizeof(header->magic)) == 0 &&
           header->version == AZ_INDEX_VERSION && header->header_size == sizeof(AzIndexHeader) &&
           header->category_record_size == sizeof(AzIndexCategoryRecord) &&
           header->figure_record_size == sizeof(AzIndexFigureRecord) &&
           header->game_record_size == sizeof(AzIndexGameRef);
}

/**
 * @brief Validate index section offsets and extents against the file size.
 * @param header Validated database index header.
 * @param file_size Size associated with file.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_header_layout_valid(const AzIndexHeader* header, uint64_t file_size) {
    if(!az_header_shape_valid(header) || header->category_count > AZ_MAX_CATEGORIES) return false;
    uint64_t categories_offset = sizeof(AzIndexHeader);
    uint64_t figures_offset = categories_offset +
                              (uint64_t)header->category_count * sizeof(AzIndexCategoryRecord);
    uint64_t games_offset = figures_offset +
                            (uint64_t)header->figure_count * sizeof(AzIndexFigureRecord);
    uint64_t end_offset = games_offset +
                          (uint64_t)header->game_ref_count * sizeof(AzIndexGameRef);
    return categories_offset == header->categories_offset &&
           figures_offset == header->figures_offset && games_offset == header->games_offset &&
           end_offset <= UINT32_MAX && end_offset <= file_size;
}

/**
 * @brief Check that a JSON byte range lies within its source file.
 * @param source_size Destination for the source file size.
 * @param offset Byte offset where the range begins.
 * @param length Number of bytes to process.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_json_range_valid(uint64_t source_size, uint32_t offset, uint32_t length) {
    return length > 0 && (uint64_t)offset < source_size &&
           (uint64_t)offset + (uint64_t)length <= source_size;
}

/**
 * @brief Read the binary database index header from storage.
 * @param storage Storage service used for file operations.
 * @param header Validated database index header.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_read_index_header(Storage* storage, AzIndexHeader* header) {
    if(!storage || !header || !storage_file_exists(storage, AZ_INDEX_FILE)) return false;
    File* file = storage_file_alloc(storage);
    if(!file || !storage_file_open(file, AZ_INDEX_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(file) storage_file_free(file);
        return false;
    }
    uint64_t file_size = storage_file_size(file);
    bool ok = az_read_exact(file, header, sizeof(*header)) &&
              az_header_layout_valid(header, file_size);
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Verify that an existing index is structurally valid and matches its sources.
 * @param storage Storage service used for file operations.
 * @param out_header Destination for the resulting header.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_index_current(
    Storage* storage,
    AzIndexHeader* out_header,
    AzDbProgressCallback progress_callback,
    void* progress_context) {
    AzIndexHeader header;
    if(!az_read_index_header(storage, &header)) return false;

    uint64_t amiibo_size = 0;
    if(!az_source_size(storage, AZ_AMIIBO_JSON, &amiibo_size)) return false;
    uint64_t games_size = 0;
    bool has_games = az_source_size(storage, AZ_GAMES_JSON, &games_size);
    if(!has_games) games_size = 0;
    if(header.amiibo_source.size != amiibo_size || header.games_source.size != games_size) {
        az_progress_emit(progress_callback, progress_context, AzDbProgressChecking, 5U);
        return false;
    }

    AzSourceStamp amiibo;
    if(!az_source_stamp(storage, AZ_AMIIBO_JSON, &amiibo)) return false;
    AzSourceStamp games;
    memset(&games, 0, sizeof(games));
    if(has_games && !az_source_stamp(storage, AZ_GAMES_JSON, &games)) return false;
    az_progress_emit(progress_callback, progress_context, AzDbProgressChecking, 5U);

    if(!az_stamp_equal(&header.amiibo_source, &amiibo) ||
       !az_stamp_equal(&header.games_source, &games)) {
        return false;
    }
    if(out_header) *out_header = header;
    return true;
}

/**
 * @brief Build a category-ID to sort-rank lookup table.
 * @param categories Category record array.
 * @param count Number of records or elements.
 * @param ranks Category sort-rank lookup table.
 */
static void az_build_category_ranks(
    const AzCategory* categories,
    uint16_t count,
    uint8_t ranks[256]) {
    memset(ranks, 0xFF, 256U);
    for(uint16_t i = 0; i < count; i++) ranks[categories[i].id] = (uint8_t)i;
}

/** @brief Chunked record storage used by the fast figure sorter. */
typedef struct {
    AzIndexFigureRecord** chunks; /**< Dynamically sized table of independently allocated record slabs. */
    uint16_t chunk_count; /**< Number of allocated slabs. */
    uint16_t chunk_slots; /**< Number of pointer-table entries available. */
    uint32_t capacity; /**< Total number of records that fit in allocated slabs. */
} AzFigureBatch;

/**
 * @brief Return a record in a chunked figure batch.
 * @param batch Batch containing the record.
 * @param index Logical record index.
 * @return Pointer to the requested record, or NULL when the index is outside the batch capacity.
 */
static AzIndexFigureRecord* az_figure_batch_at(AzFigureBatch* batch, uint32_t index) {
    if(!batch || index >= batch->capacity) return NULL;
    uint32_t chunk = index / AZ_SORT_BATCH_CHUNK_RECORDS;
    uint32_t offset = index % AZ_SORT_BATCH_CHUNK_RECORDS;
    if(chunk >= batch->chunk_count || !batch->chunks[chunk]) return NULL;
    return &batch->chunks[chunk][offset];
}

/**
 * @brief Return a const record in a chunked figure batch.
 * @param batch Batch containing the record.
 * @param index Logical record index.
 * @return Pointer to the requested record, or NULL when the index is outside the batch capacity.
 */
static const AzIndexFigureRecord* az_figure_batch_at_const(
    const AzFigureBatch* batch,
    uint32_t index) {
    if(!batch || index >= batch->capacity) return NULL;
    uint32_t chunk = index / AZ_SORT_BATCH_CHUNK_RECORDS;
    uint32_t offset = index % AZ_SORT_BATCH_CHUNK_RECORDS;
    if(chunk >= batch->chunk_count || !batch->chunks[chunk]) return NULL;
    return &batch->chunks[chunk][offset];
}

/**
 * @brief Release all slabs owned by a chunked figure batch.
 * @param batch Batch to clear.
 */
static void az_figure_batch_free(AzFigureBatch* batch) {
    if(!batch) return;
    for(uint16_t i = 0U; i < batch->chunk_count; i++) {
        free(batch->chunks[i]);
        batch->chunks[i] = NULL;
    }
    free(batch->chunks);
    batch->chunks = NULL;
    batch->chunk_count = 0U;
    batch->chunk_slots = 0U;
    batch->capacity = 0U;
}

/**
 * @brief Grow a chunked figure batch while preserving heap headroom.
 * @param batch Batch to grow.
 * @param target_capacity Desired record capacity.
 * @return Number of records available after growth.
 */
static uint32_t az_figure_batch_allocate(
    AzFigureBatch* batch,
    uint32_t required_capacity,
    uint32_t target_capacity) {
    if(!batch) return 0U;
    memset(batch, 0, sizeof(*batch));

    if(target_capacity < required_capacity) target_capacity = required_capacity;
    if(target_capacity == 0U) return 0U;

    const uint32_t chunk_slots32 =
        (target_capacity + AZ_SORT_BATCH_CHUNK_RECORDS - 1U) / AZ_SORT_BATCH_CHUNK_RECORDS;
    if(chunk_slots32 == 0U || chunk_slots32 > UINT16_MAX) return 0U;

    batch->chunks = calloc((size_t)chunk_slots32, sizeof(*batch->chunks));
    if(!batch->chunks) return 0U;
    batch->chunk_slots = (uint16_t)chunk_slots32;

    const size_t chunk_bytes =
        (size_t)AZ_SORT_BATCH_CHUNK_RECORDS * sizeof(AzIndexFigureRecord);
    while(batch->capacity < target_capacity && batch->chunk_count < batch->chunk_slots) {
        const size_t reserve = batch->capacity < required_capacity ?
                                   AZ_SORT_HEAP_MIN_RESERVE :
                                   AZ_SORT_HEAP_PREFERRED_RESERVE;
        const size_t free_heap = memmgr_get_free_heap();
        if(free_heap <= reserve || free_heap - reserve < chunk_bytes) break;

        AzIndexFigureRecord* chunk = malloc(chunk_bytes);
        if(!chunk) break;

        batch->chunks[batch->chunk_count++] = chunk;
        batch->capacity += AZ_SORT_BATCH_CHUNK_RECORDS;
    }

    if(batch->capacity > target_capacity) batch->capacity = target_capacity;
    return batch->capacity;
}

/**
 * @brief Swap two logical records in a chunked figure batch.
 * @param batch Batch containing the records.
 * @param left_index First logical index.
 * @param right_index Second logical index.
 */
static void az_figure_batch_swap(
    AzFigureBatch* batch,
    uint32_t left_index,
    uint32_t right_index) {
    if(left_index == right_index) return;
    AzIndexFigureRecord* left = az_figure_batch_at(batch, left_index);
    AzIndexFigureRecord* right = az_figure_batch_at(batch, right_index);
    if(!left || !right) return;
    AzIndexFigureRecord temporary = *left;
    *left = *right;
    *right = temporary;
}

/**
 * @brief Restore heap ordering within one logical category range in a chunked batch.
 * @param batch Batch containing the category.
 * @param base Logical index of the category's first record.
 * @param count Number of records in the category.
 * @param root Heap node index relative to the category start.
 */
static void az_figure_batch_heap_sift_down(
    AzFigureBatch* batch,
    uint32_t base,
    uint32_t count,
    uint32_t root) {
    while(count > 1U && root <= (count - 2U) / 2U) {
        uint32_t child = root * 2U + 1U;
        const AzIndexFigureRecord* child_record = az_figure_batch_at_const(batch, base + child);
        if(child + 1U < count) {
            const AzIndexFigureRecord* right_record =
                az_figure_batch_at_const(batch, base + child + 1U);
            if(child_record && right_record &&
               az_figure_compare(&child_record->figure, &right_record->figure) < 0) {
                child++;
                child_record = right_record;
            }
        }

        const AzIndexFigureRecord* root_record = az_figure_batch_at_const(batch, base + root);
        if(!root_record || !child_record ||
           az_figure_compare(&root_record->figure, &child_record->figure) >= 0) {
            break;
        }
        az_figure_batch_swap(batch, base + root, base + child);
        root = child;
    }
}

/**
 * @brief Sort one category held in a chunked figure batch.
 * @param batch Batch containing the category.
 * @param base Logical index of the category's first record.
 * @param count Number of records in the category.
 */
static void az_sort_category_batch(AzFigureBatch* batch, uint32_t base, uint32_t count) {
    if(!batch || count < 2U) return;
    for(uint32_t start = count / 2U; start > 0U; start--) {
        az_figure_batch_heap_sift_down(batch, base, count, start - 1U);
    }
    for(uint32_t end = count; end > 1U; end--) {
        az_figure_batch_swap(batch, base, base + end - 1U);
        az_figure_batch_heap_sift_down(batch, base, end - 1U, 0U);
    }
}

/**
 * @brief Write logical records from a chunked figure batch to the destination index.
 * @param destination Destination index file.
 * @param batch Batch containing records to write.
 * @param count Number of logical records to write.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_write_figure_batch(File* destination, const AzFigureBatch* batch, uint32_t count) {
    if(!destination || !batch || count > batch->capacity) return false;

    uint32_t written = 0U;
    while(written < count) {
        uint32_t chunk_index = written / AZ_SORT_BATCH_CHUNK_RECORDS;
        uint32_t chunk_offset = written % AZ_SORT_BATCH_CHUNK_RECORDS;
        if(chunk_index >= batch->chunk_count || !batch->chunks[chunk_index]) return false;

        uint32_t available = AZ_SORT_BATCH_CHUNK_RECORDS - chunk_offset;
        uint32_t remaining = count - written;
        uint32_t record_count = remaining < available ? remaining : available;
        size_t bytes = (size_t)record_count * sizeof(AzIndexFigureRecord);
        if(storage_file_write(
               destination, batch->chunks[chunk_index] + chunk_offset, bytes) != bytes) {
            return false;
        }
        written += record_count;
    }
    return true;
}

/**
 * @brief Sort figure records in bounded batches and write them to the destination index.
 * @param storage Storage service used for file operations.
 * @param destination Destination object or buffer.
 * @param categories Category record array.
 * @param category_count Number of category entries.
 * @param figure_count Number of figure entries.
 * @param ranks Category sort-rank lookup table.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @param out_allocation_failed Destination for the resulting allocation failed.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_write_sorted_figure_batches(
    Storage* storage,
    File* destination,
    const AzCategory* categories,
    uint16_t category_count,
    uint32_t figure_count,
    const uint8_t ranks[256],
    AzDbProgressCallback progress_callback,
    void* progress_context,
    bool* out_allocation_failed) {
    if(out_allocation_failed) *out_allocation_failed = false;
    if(!storage || !destination || !categories || !ranks) return false;
    az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, 45U);
    if(figure_count == 0U) {
        az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, 68U);
        return true;
    }

    uint32_t largest_category = 0U;
    for(uint16_t i = 0; i < category_count; i++) {
        if(categories[i].count > largest_category) largest_category = categories[i].count;
    }
    if(largest_category == 0U || largest_category > figure_count) return false;

    /* Reserve the storage object's heap before consuming the rest with record slabs. */
    File* source = storage_file_alloc(storage);
    if(!source || !storage_file_open(source, AZ_INDEX_RAW, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(source) storage_file_free(source);
        return false;
    }

    AzIndexFigureRecord* scan =
        malloc((size_t)AZ_SORT_BATCH_SCAN_RECORDS * sizeof(AzIndexFigureRecord));
    if(!scan) {
        storage_file_close(source);
        storage_file_free(source);
        if(out_allocation_failed) *out_allocation_failed = true;
        return false;
    }

    /* Prime the storage read path before the slabs consume the remaining heap. */
    uint32_t prime_count = figure_count;
    if(prime_count > AZ_SORT_BATCH_SCAN_RECORDS) prime_count = AZ_SORT_BATCH_SCAN_RECORDS;
    size_t prime_bytes = (size_t)prime_count * sizeof(AzIndexFigureRecord);
    if(storage_file_read(source, scan, prime_bytes) != prime_bytes ||
       !storage_file_seek(source, 0U, true)) {
        free(scan);
        storage_file_close(source);
        storage_file_free(source);
        return false;
    }

    uint32_t target_capacity = AZ_SORT_BATCH_TARGET_RECORDS;
    if(target_capacity < largest_category) target_capacity = largest_category;
    if(target_capacity > figure_count) target_capacity = figure_count;

    AzFigureBatch batch;
    uint32_t capacity =
        az_figure_batch_allocate(&batch, largest_category, target_capacity);
    if(capacity < largest_category) {
        az_figure_batch_free(&batch);
        free(scan);
        storage_file_close(source);
        storage_file_free(source);
        if(out_allocation_failed) *out_allocation_failed = true;
        return false;
    }

    bool ok = true;
    uint32_t category_base[AZ_MAX_CATEGORIES];
    uint16_t category_filled[AZ_MAX_CATEGORIES];
    uint16_t batch_start = 0U;
    uint32_t emitted = 0U;

    while(ok && batch_start < category_count) {
        memset(category_base, 0, sizeof(category_base));
        memset(category_filled, 0, sizeof(category_filled));

        uint16_t batch_end = batch_start;
        uint32_t batch_count = 0U;
        while(batch_end < category_count) {
            uint32_t category_records = categories[batch_end].count;
            if(batch_count > 0U && batch_count + category_records > capacity) break;
            if(category_records > capacity) {
                ok = false;
                break;
            }
            category_base[batch_end] = batch_count;
            batch_count += category_records;
            batch_end++;
        }
        if(!ok || batch_end == batch_start || batch_count == 0U) {
            ok = false;
            break;
        }

        if(!storage_file_seek(source, 0U, true)) {
            ok = false;
            break;
        }

        uint32_t scanned = 0U;
        while(ok && scanned < figure_count) {
            uint32_t chunk = figure_count - scanned;
            if(chunk > AZ_SORT_BATCH_SCAN_RECORDS) chunk = AZ_SORT_BATCH_SCAN_RECORDS;
            size_t bytes = (size_t)chunk * sizeof(AzIndexFigureRecord);
            if(storage_file_read(source, scan, bytes) != bytes) {
                ok = false;
                break;
            }
            for(uint32_t i = 0; i < chunk; i++) {
                uint8_t rank = ranks[scan[i].figure.category];
                if(rank < batch_start || rank >= batch_end) continue;
                uint16_t filled = category_filled[rank];
                if(filled >= categories[rank].count) {
                    ok = false;
                    break;
                }
                AzIndexFigureRecord* destination_record =
                    az_figure_batch_at(&batch, category_base[rank] + filled);
                if(!destination_record) {
                    ok = false;
                    break;
                }
                *destination_record = scan[i];
                category_filled[rank] = (uint16_t)(filled + 1U);
            }
            scanned += chunk;
        }

        for(uint16_t rank = batch_start; ok && rank < batch_end; rank++) {
            if(category_filled[rank] != categories[rank].count) {
                ok = false;
                break;
            }
            az_sort_category_batch(&batch, category_base[rank], categories[rank].count);
        }
        if(ok) ok = az_write_figure_batch(destination, &batch, batch_count);
        if(!ok) break;

        emitted += batch_count;
        uint8_t percent = (uint8_t)(45U + az_progress_scale_u32(emitted, figure_count, 23U));
        if(percent > 68U) percent = 68U;
        az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, percent);
        batch_start = batch_end;
    }

    az_figure_batch_free(&batch);
    free(scan);
    storage_file_close(source);
    storage_file_free(source);
    return ok && emitted == figure_count;
}

/**
 * @brief Compare indexed figure records using category ranks and figure ordering.
 * @param left Left comparison operand.
 * @param right Right comparison operand.
 * @param ranks Category sort-rank lookup table.
 * @return A negative, zero, or positive value when the left operand sorts before, equal to, or after the right operand.
 */
static int az_index_figure_compare(
    const AzIndexFigureRecord* left,
    const AzIndexFigureRecord* right,
    const uint8_t ranks[256]) {
    uint8_t left_rank = ranks[left->figure.category];
    uint8_t right_rank = ranks[right->figure.category];
    if(left_rank != right_rank) return left_rank < right_rank ? -1 : 1;
    if(left_rank == 0xFFU && left->figure.category != right->figure.category) {
        return left->figure.category < right->figure.category ? -1 : 1;
    }
    return az_figure_compare(&left->figure, &right->figure);
}

/**
 * @brief Sort one in-memory run of figure records.
 * @param records Figure record array.
 * @param count Number of records or elements.
 * @param ranks Category sort-rank lookup table.
 */
static void az_sort_figure_run(
    AzIndexFigureRecord* records,
    uint16_t count,
    const uint8_t ranks[256]) {
    for(uint16_t i = 1; i < count; i++) {
        AzIndexFigureRecord current = records[i];
        uint16_t j = i;
        while(j > 0 && az_index_figure_compare(&records[j - 1], &current, ranks) > 0) {
            records[j] = records[j - 1];
            j--;
        }
        if(j != i) records[j] = current;
    }
}

/**
 * @brief Create sorted temporary runs for an external figure-record merge.
 * @param storage Storage service used for file operations.
 * @param figure_count Number of figure entries.
 * @param ranks Category sort-rank lookup table.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_create_sorted_runs(
    Storage* storage,
    uint32_t figure_count,
    const uint8_t ranks[256],
    AzDbProgressCallback progress_callback,
    void* progress_context) {
    File* source = storage_file_alloc(storage);
    File* destination = storage_file_alloc(storage);
    bool ok = source && destination &&
              storage_file_open(source, AZ_INDEX_RAW, FSAM_READ, FSOM_OPEN_EXISTING) &&
              storage_file_open(destination, AZ_INDEX_SORT_RUNS, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    AzIndexFigureRecord* records =
        ok ? malloc(AZ_SORT_RUN_RECORDS * sizeof(AzIndexFigureRecord)) : NULL;
    if(!records) ok = false;

    uint32_t processed = 0;
    while(ok && processed < figure_count) {
        uint32_t remaining = figure_count - processed;
        uint16_t chunk = (uint16_t)(remaining > AZ_SORT_RUN_RECORDS ? AZ_SORT_RUN_RECORDS : remaining);
        size_t bytes = (size_t)chunk * sizeof(AzIndexFigureRecord);
        if(storage_file_read(source, records, bytes) != bytes) {
            ok = false;
            break;
        }
        az_sort_figure_run(records, chunk, ranks);
        if(storage_file_write(destination, records, bytes) != bytes) {
            ok = false;
            break;
        }
        processed += chunk;
        uint8_t percent = figure_count ? (uint8_t)(45U + az_progress_scale_u32(processed, figure_count, 8U)) : 53U;
        az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, percent);
    }
    free(records);
    if(source) {
        storage_file_close(source);
        storage_file_free(source);
    }
    if(destination) {
        storage_file_close(destination);
        storage_file_free(destination);
    }
    return ok;
}

/**
 * @brief Write buffered output figure records and reset the buffer count.
 * @param destination Destination object or buffer.
 * @param output Output buffer.
 * @param output_count Number of output entries.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_flush_figure_records(
    File* destination,
    AzIndexFigureRecord* output,
    uint8_t* output_count) {
    if(!destination || !output || !output_count) return false;
    if(*output_count == 0U) return true;
    size_t bytes = (size_t)(*output_count) * sizeof(AzIndexFigureRecord);
    bool ok = storage_file_write(destination, output, bytes) == bytes;
    *output_count = 0;
    return ok;
}

/**
 * @brief Merge adjacent sorted figure runs into larger runs.
 * @param storage Storage service used for file operations.
 * @param source_path Path of the source file.
 * @param destination_path Path of the destination file.
 * @param figure_count Number of figure entries.
 * @param run_size Size associated with run.
 * @param ranks Category sort-rank lookup table.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_merge_figure_pass(
    Storage* storage,
    const char* source_path,
    const char* destination_path,
    uint32_t figure_count,
    uint32_t run_size,
    const uint8_t ranks[256]) {
    File* left = storage_file_alloc(storage);
    File* right = storage_file_alloc(storage);
    File* destination = storage_file_alloc(storage);
    bool ok = left && right && destination &&
              storage_file_open(left, source_path, FSAM_READ, FSOM_OPEN_EXISTING) &&
              storage_file_open(right, source_path, FSAM_READ, FSOM_OPEN_EXISTING) &&
              storage_file_open(destination, destination_path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    AzIndexFigureRecord output[16];
    uint8_t output_count = 0;

    for(uint32_t base = 0; ok && base < figure_count; base += run_size * 2U) {
        uint32_t left_count = figure_count - base;
        if(left_count > run_size) left_count = run_size;
        uint32_t right_count = figure_count - base - left_count;
        if(right_count > run_size) right_count = run_size;

        uint64_t left_offset = (uint64_t)base * sizeof(AzIndexFigureRecord);
        uint64_t right_offset = (uint64_t)(base + left_count) * sizeof(AzIndexFigureRecord);
        if(left_offset > UINT32_MAX || right_offset > UINT32_MAX ||
           !storage_file_seek(left, (uint32_t)left_offset, true) ||
           !storage_file_seek(right, (uint32_t)right_offset, true)) {
            ok = false;
            break;
        }

        uint32_t left_read = 0;
        uint32_t right_read = 0;
        AzIndexFigureRecord left_record;
        AzIndexFigureRecord right_record;
        bool have_left = left_count > 0U && az_read_exact(left, &left_record, sizeof(left_record));
        bool have_right = right_count > 0U && az_read_exact(right, &right_record, sizeof(right_record));
        if(left_count > 0U && !have_left) ok = false;
        if(right_count > 0U && !have_right) ok = false;
        if(!ok) break;
        if(have_left) left_read = 1U;
        if(have_right) right_read = 1U;

        while(ok && (have_left || have_right)) {
            bool take_left = have_left &&
                             (!have_right ||
                              az_index_figure_compare(&left_record, &right_record, ranks) <= 0);
            output[output_count++] = take_left ? left_record : right_record;
            if(output_count == COUNT_OF(output)) {
                ok = az_flush_figure_records(destination, output, &output_count);
                if(!ok) break;
            }

            if(take_left) {
                if(left_read < left_count) {
                    have_left = az_read_exact(left, &left_record, sizeof(left_record));
                    if(!have_left) ok = false;
                    left_read++;
                } else {
                    have_left = false;
                }
            } else {
                if(right_read < right_count) {
                    have_right = az_read_exact(right, &right_record, sizeof(right_record));
                    if(!have_right) ok = false;
                    right_read++;
                } else {
                    have_right = false;
                }
            }
        }
    }
    if(ok) ok = az_flush_figure_records(destination, output, &output_count);
    if(left) {
        storage_file_close(left);
        storage_file_free(left);
    }
    if(right) {
        storage_file_close(right);
        storage_file_free(right);
    }
    if(destination) {
        storage_file_close(destination);
        storage_file_free(destination);
    }
    return ok;
}

/**
 * @brief Produce a fully sorted temporary figure stream using in-memory or external sorting.
 * @param storage Storage service used for file operations.
 * @param figure_count Number of figure entries.
 * @param ranks Category sort-rank lookup table.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @param out_sorted_path Destination for the resulting sorted path.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_prepare_sorted_figures(
    Storage* storage,
    uint32_t figure_count,
    const uint8_t ranks[256],
    AzDbProgressCallback progress_callback,
    void* progress_context,
    const char** out_sorted_path) {
    if(!storage || !ranks || !out_sorted_path) return false;
    *out_sorted_path = AZ_INDEX_SORT_RUNS;
    az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, 45U);
    if(!az_create_sorted_runs(storage, figure_count, ranks, progress_callback, progress_context)) {
        return false;
    }

    uint32_t pass_count = 0;
    for(uint32_t size = AZ_SORT_RUN_RECORDS; size < figure_count && size <= UINT32_MAX / 2U;
        size *= 2U) {
        pass_count++;
    }
    const char* source_path = AZ_INDEX_SORT_RUNS;
    const char* destination_path = AZ_INDEX_RAW;
    uint32_t run_size = AZ_SORT_RUN_RECORDS;
    for(uint32_t pass = 0; pass < pass_count; pass++) {
        if(!az_merge_figure_pass(
               storage, source_path, destination_path, figure_count, run_size, ranks)) {
            return false;
        }
        const char* swap = source_path;
        source_path = destination_path;
        destination_path = swap;
        if(run_size <= UINT32_MAX / 2U) run_size *= 2U;
        uint8_t percent = (uint8_t)(53U + az_progress_scale_u32(pass + 1U, pass_count, 15U));
        az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, percent);
    }
    if(pass_count == 0U) {
        az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, 68U);
    }
    *out_sorted_path = source_path;
    return true;
}

/**
 * @brief Copy a fixed byte count between storage files while reporting progress.
 * @param storage Storage service used for file operations.
 * @param source_path Path of the source file.
 * @param destination Destination object or buffer.
 * @param byte_count Number of bytes to copy.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @param start_percent Progress percentage assigned to the start of the operation.
 * @param end_percent Progress percentage assigned to the end of the operation.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_copy_file_bytes(
    Storage* storage,
    const char* source_path,
    File* destination,
    uint32_t byte_count,
    AzDbProgressCallback progress_callback,
    void* progress_context,
    uint8_t start_percent,
    uint8_t end_percent) {
    if(!storage || !source_path || !destination) return false;
    File* source = storage_file_alloc(storage);
    if(!source || !storage_file_open(source, source_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(source) storage_file_free(source);
        return false;
    }
    uint8_t* buffer = malloc(AZ_COPY_BUFFER);
    bool ok = buffer != NULL;
    uint32_t copied = 0;
    while(ok && copied < byte_count) {
        uint32_t remaining = byte_count - copied;
        size_t chunk = remaining > AZ_COPY_BUFFER ? AZ_COPY_BUFFER : remaining;
        size_t read = storage_file_read(source, buffer, chunk);
        if(read != chunk || storage_file_write(destination, buffer, read) != read) {
            ok = false;
            break;
        }
        copied += (uint32_t)read;
        uint32_t span = (uint32_t)(end_percent - start_percent);
        uint8_t percent = byte_count ?
                              (uint8_t)(start_percent + az_progress_scale_u32(copied, byte_count, span)) :
                              end_percent;
        az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, percent);
    }
    free(buffer);
    storage_file_close(source);
    storage_file_free(source);
    return ok;
}

/**
 * @brief Build a fresh binary index from the Amiibo and game JSON sources.
 * @param storage Storage service used for file operations.
 * @param out_count Destination for a resulting record count.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_index_build(
    Storage* storage,
    uint32_t* out_count,
    AzDbProgressCallback progress_callback,
    void* progress_context) {
    if(out_count) *out_count = 0;
    az_progress_emit(progress_callback, progress_context, AzDbProgressChecking, 5U);

    AzSourceStamp amiibo_before;
    if(!az_source_stamp(storage, AZ_AMIIBO_JSON, &amiibo_before)) return false;
    AzSourceStamp games_before;
    memset(&games_before, 0, sizeof(games_before));
    bool has_games = az_source_stamp(storage, AZ_GAMES_JSON, &games_before);

    AzCategory* categories = calloc(AZ_MAX_CATEGORIES, sizeof(AzCategory));
    if(!categories) return false;
    uint16_t category_count = 0;
    uint32_t figure_count = 0;
    uint32_t game_ref_count = 0;
    bool ok = true;

    File* raw_figures = storage_file_alloc(storage);
    if(!raw_figures ||
       !storage_file_open(raw_figures, AZ_INDEX_RAW, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = false;
    }
    if(ok) {
        AzFigureBuildContext figure_context = {0};
        figure_context.raw_file = raw_figures;
        figure_context.categories = categories;
        figure_context.category_capacity = AZ_MAX_CATEGORIES;
        figure_context.category_count = &category_count;
        AzProgressReporter amiibo_progress;
        az_progress_reporter_init(
            &amiibo_progress,
            progress_callback,
            progress_context,
            AzDbProgressAmiibo,
            5U,
            45U,
            (uint32_t)amiibo_before.size);
        ok = az_scan_json_file(
            storage,
            AZ_AMIIBO_JSON,
            az_figure_build_event,
            &figure_context,
            &figure_context.control,
            &amiibo_progress,
            NULL);
        figure_count = figure_context.figure_count;
    }
    if(raw_figures) {
        storage_file_close(raw_figures);
        storage_file_free(raw_figures);
        raw_figures = NULL;
    }

    az_sort_categories(categories, category_count);
    uint8_t category_ranks[256];
    az_build_category_ranks(categories, category_count, category_ranks);

    File* index = NULL;
    AzIndexHeader header;
    memset(&header, 0, sizeof(header));
    if(ok) {
        index = storage_file_alloc(storage);
        ok = index && storage_file_open(index, AZ_INDEX_TMP, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS);
    }
    if(ok) {
        az_str_copy(header.magic, sizeof(header.magic), AZ_INDEX_MAGIC);
        header.version = AZ_INDEX_VERSION;
        header.header_size = sizeof(header);
        header.category_record_size = sizeof(AzIndexCategoryRecord);
        header.figure_record_size = sizeof(AzIndexFigureRecord);
        header.game_record_size = sizeof(AzIndexGameRef);
        header.amiibo_source = amiibo_before;
        header.games_source = games_before;
        header.figure_count = figure_count;
        header.category_count = category_count;
        uint64_t categories_offset = sizeof(header);
        uint64_t figures_offset = categories_offset +
                                  (uint64_t)header.category_count * sizeof(AzIndexCategoryRecord);
        uint64_t games_offset = figures_offset +
                                (uint64_t)header.figure_count * sizeof(AzIndexFigureRecord);
        if(games_offset > UINT32_MAX) {
            ok = false;
        } else {
            header.categories_offset = (uint32_t)categories_offset;
            header.figures_offset = (uint32_t)figures_offset;
            header.games_offset = (uint32_t)games_offset;
            ok = az_write_exact(index, &header, sizeof(header));
        }
    }

    uint32_t first_figure = 0;
    for(uint16_t i = 0; ok && i < category_count; i++) {
        AzIndexCategoryRecord record = {0};
        record.category = categories[i];
        record.first_figure = first_figure;
        ok = az_write_exact(index, &record, sizeof(record));
        first_figure += categories[i].count;
    }
    if(ok) {
        bool batch_allocation_failed = false;
        ok = az_write_sorted_figure_batches(
            storage,
            index,
            categories,
            category_count,
            figure_count,
            category_ranks,
            progress_callback,
            progress_context,
            &batch_allocation_failed);
        const bool allow_external_sort_fallback = false;
        if(!ok && batch_allocation_failed && allow_external_sort_fallback) {
            const char* sorted_figure_path = AZ_INDEX_SORT_RUNS;
            ok = az_prepare_sorted_figures(
                storage,
                figure_count,
                category_ranks,
                progress_callback,
                progress_context,
                &sorted_figure_path);
            if(ok) {
                uint64_t figure_bytes64 =
                    (uint64_t)figure_count * sizeof(AzIndexFigureRecord);
                ok = figure_bytes64 <= UINT32_MAX &&
                     az_copy_file_bytes(
                         storage,
                         sorted_figure_path,
                         index,
                         (uint32_t)figure_bytes64,
                         progress_callback,
                         progress_context,
                         68U,
                         75U);
            }
        } else if(ok) {
            az_progress_emit(progress_callback, progress_context, AzDbProgressSorting, 75U);
        }
    }
    if(ok && storage_file_tell(index) != header.games_offset) ok = false;

    if(ok && has_games) {
        AzGameIndexBuildContext game_context = {0};
        game_context.output_file = index;
        AzProgressReporter games_progress;
        az_progress_reporter_init(
            &games_progress,
            progress_callback,
            progress_context,
            AzDbProgressGames,
            75U,
            95U,
            (uint32_t)games_before.size);
        ok = az_scan_json_file(
            storage,
            AZ_GAMES_JSON,
            az_game_index_event,
            &game_context,
            &game_context.control,
            &games_progress,
            NULL);
        game_ref_count = game_context.count;
    } else if(ok) {
        az_progress_emit(progress_callback, progress_context, AzDbProgressGames, 95U);
    }

    if(ok) {
        uint64_t expected_end = (uint64_t)header.games_offset +
                                (uint64_t)game_ref_count * sizeof(AzIndexGameRef);
        if(expected_end > UINT32_MAX || storage_file_tell(index) != expected_end) {
            ok = false;
        } else {
            header.game_ref_count = game_ref_count;
            header.games_source = games_before;
            ok = storage_file_seek(index, 0, true) && az_write_exact(index, &header, sizeof(header));
        }
    }
    if(index) {
        if(ok) storage_file_sync(index);
        storage_file_close(index);
        storage_file_free(index);
        index = NULL;
    }

    az_progress_emit(progress_callback, progress_context, AzDbProgressFinalizing, 96U);
    if(ok) {

        AzSourceStamp amiibo_after;
        ok = az_source_stamp(storage, AZ_AMIIBO_JSON, &amiibo_after) &&
             az_stamp_equal(&amiibo_before, &amiibo_after);
        AzSourceStamp games_after;
        memset(&games_after, 0, sizeof(games_after));
        bool games_after_present = az_source_stamp(storage, AZ_GAMES_JSON, &games_after);
        ok = ok && games_after_present == has_games &&
             (!has_games || az_stamp_equal(&games_before, &games_after));
    }
    az_progress_emit(progress_callback, progress_context, AzDbProgressFinalizing, 99U);
    if(ok) {

        storage_common_remove(storage, AZ_INDEX_BACKUP);
        bool had_old = storage_file_exists(storage, AZ_INDEX_FILE);
        if(had_old && storage_common_rename(storage, AZ_INDEX_FILE, AZ_INDEX_BACKUP) != FSE_OK) {
            ok = false;
        }
        if(ok && storage_common_rename(storage, AZ_INDEX_TMP, AZ_INDEX_FILE) != FSE_OK) {
            ok = false;
            if(had_old) storage_common_rename(storage, AZ_INDEX_BACKUP, AZ_INDEX_FILE);
        } else if(ok && had_old) {
            storage_common_remove(storage, AZ_INDEX_BACKUP);
        }
    }
    if(!ok) storage_common_remove(storage, AZ_INDEX_TMP);
    storage_common_remove(storage, AZ_INDEX_RAW);
    storage_common_remove(storage, AZ_INDEX_SORT_RUNS);
    if(ok && out_count) *out_count = figure_count;
    free(categories);
    return ok;
}

/**
 * @brief Open the current index and validate its header before use.
 * @param storage Storage service used for file operations.
 * @param out_file Destination for the resulting file.
 * @param out_header Destination for the resulting header.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_open_index(Storage* storage, File** out_file, AzIndexHeader* out_header) {
    if(!storage || !out_file || !out_header) return false;
    *out_file = NULL;
    File* file = storage_file_alloc(storage);
    if(!file || !storage_file_open(file, AZ_INDEX_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(file) storage_file_free(file);
        return false;
    }
    AzIndexHeader header;
    uint64_t file_size = storage_file_size(file);
    if(!az_read_exact(file, &header, sizeof(header)) || !az_header_layout_valid(&header, file_size)) {
        storage_file_close(file);
        storage_file_free(file);
        return false;
    }
    *out_file = file;
    *out_header = header;
    return true;
}

/**
 * @brief Read one category record from the index by ordinal.
 * @param file Open storage file.
 * @param header Validated database index header.
 * @param ordinal Zero-based record or parser-stack ordinal.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_read_category_record(
    File* file,
    const AzIndexHeader* header,
    uint16_t ordinal,
    AzIndexCategoryRecord* out) {
    if(!file || !header || !out || ordinal >= header->category_count) return false;
    uint32_t offset = header->categories_offset + (uint32_t)ordinal * sizeof(*out);
    return storage_file_seek(file, offset, true) && az_read_exact(file, out, sizeof(*out));
}

/**
 * @brief Read one figure record from the index by ordinal.
 * @param file Open storage file.
 * @param header Validated database index header.
 * @param ordinal Zero-based record or parser-stack ordinal.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_read_figure_record(
    File* file,
    const AzIndexHeader* header,
    uint32_t ordinal,
    AzIndexFigureRecord* out) {
    if(!file || !header || !out || ordinal >= header->figure_count) return false;
    uint32_t offset = header->figures_offset + ordinal * sizeof(*out);
    if(!storage_file_seek(file, offset, true) || !az_read_exact(file, out, sizeof(*out))) return false;
    return az_json_range_valid(
        header->amiibo_source.size,
        out->figure.json_offset,
        out->figure.json_length);
}

/**
 * @brief Find an indexed category record by category ID.
 * @param file Open storage file.
 * @param header Validated database index header.
 * @param category Category identifier.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_find_category_record(
    File* file,
    const AzIndexHeader* header,
    uint8_t category,
    AzIndexCategoryRecord* out) {
    for(uint16_t i = 0; i < header->category_count; i++) {
        AzIndexCategoryRecord record;
        if(!az_read_category_record(file, header, i, &record)) return false;
        if(record.category.id == category) {
            *out = record;
            return true;
        }
    }
    return false;
}

/**
 * @brief Calculate the first row of a fixed-height list window around a selection.
 * @param selection Selected result ordinal.
 * @param total Total number of records or items.
 * @return The absolute ordinal of the first row in the visible window.
 */
static uint16_t az_window_start(uint16_t selection, uint16_t total) {
    if(total <= AZ_LIST_ROWS) return 0;
    uint16_t start = selection >= 2 ? (uint16_t)(selection - 2) : 0;
    if(start + AZ_LIST_ROWS > total) start = (uint16_t)(total - AZ_LIST_ROWS);
    return start;
}

/**
 * @brief Ensure the binary metadata index exists and matches the current JSON sources.
 * @param storage Storage service used for file operations.
 * @param force Whether to rebuild even when the existing index is current.
 * @param out_count Destination for a resulting record count.
 * @param progress_callback Optional callback that receives database progress updates.
 * @param progress_context Caller-owned context passed to the progress callback.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_ensure_index(
    Storage* storage,
    bool force,
    uint32_t* out_count,
    AzDbProgressCallback progress_callback,
    void* progress_context) {
    if(out_count) *out_count = 0;
    az_progress_emit(progress_callback, progress_context, AzDbProgressChecking, 0U);
    if(!storage || !storage_file_exists(storage, AZ_AMIIBO_JSON)) return false;
    AzIndexHeader header;
    if(!force && az_index_current(storage, &header, progress_callback, progress_context)) {
        if(out_count) *out_count = header.figure_count;
        az_progress_emit(progress_callback, progress_context, AzDbProgressDone, 100U);
        return true;
    }
    bool ok = az_index_build(storage, out_count, progress_callback, progress_context);
    if(ok) az_progress_emit(progress_callback, progress_context, AzDbProgressDone, 100U);
    return ok;
}

/**
 * @brief Read the visible category window surrounding a selection.
 * @param storage Storage service used for file operations.
 * @param selection Selected result ordinal.
 * @param out_rows Destination array for visible rows.
 * @param out_row_count Destination for the number of populated output rows.
 * @param out_window_start Destination for the absolute ordinal of the first returned row.
 * @param out_total Destination for the total number of matching records.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_category_window(
    Storage* storage,
    uint16_t selection,
    AzCategory out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total) {
    if(out_row_count) *out_row_count = 0;
    if(out_window_start) *out_window_start = 0;
    if(out_total) *out_total = 0;
    File* file;
    AzIndexHeader header;
    if(!out_rows || !az_open_index(storage, &file, &header)) return false;
    uint16_t total = header.category_count;
    if(total && selection >= total) selection = (uint16_t)(total - 1);
    uint16_t start = az_window_start(selection, total);
    uint8_t rows = 0;
    for(uint16_t i = start; i < total && rows < AZ_LIST_ROWS; i++) {
        AzIndexCategoryRecord record;
        if(!az_read_category_record(file, &header, i, &record)) break;
        out_rows[rows++] = record.category;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(out_row_count) *out_row_count = rows;
    if(out_window_start) *out_window_start = start;
    if(out_total) *out_total = total;
    return true;
}

/**
 * @brief Read one category by index ordinal.
 * @param storage Storage service used for file operations.
 * @param category_index Category ordinal in the index.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_category(Storage* storage, uint16_t category_index, AzCategory* out) {
    File* file;
    AzIndexHeader header;
    if(!out || !az_open_index(storage, &file, &header)) return false;
    AzIndexCategoryRecord record;
    bool ok = az_read_category_record(file, &header, category_index, &record);
    if(ok) *out = record.category;
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Read the visible figure window for a category and selection.
 * @param storage Storage service used for file operations.
 * @param category Category identifier.
 * @param selection Selected result ordinal.
 * @param out_rows Destination array for visible rows.
 * @param out_row_count Destination for the number of populated output rows.
 * @param out_window_start Destination for the absolute ordinal of the first returned row.
 * @param out_total Destination for the total number of matching records.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_figure_window(
    Storage* storage,
    uint8_t category,
    uint16_t selection,
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total) {
    if(out_row_count) *out_row_count = 0;
    if(out_window_start) *out_window_start = 0;
    if(out_total) *out_total = 0;
    File* file;
    AzIndexHeader header;
    if(!out_rows || !az_open_index(storage, &file, &header)) return false;
    AzIndexCategoryRecord category_record;
    bool ok = az_find_category_record(file, &header, category, &category_record);
    uint16_t total = ok ? category_record.category.count : 0;
    if(total && selection >= total) selection = (uint16_t)(total - 1);
    uint16_t start = az_window_start(selection, total);
    uint8_t rows = 0;
    for(uint16_t i = start; ok && i < total && rows < AZ_LIST_ROWS; i++) {
        AzIndexFigureRecord record;
        ok = az_read_figure_record(file, &header, category_record.first_figure + i, &record);
        if(ok) out_rows[rows++] = record.figure;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(out_row_count) *out_row_count = rows;
    if(out_window_start) *out_window_start = start;
    if(out_total) *out_total = total;
    return ok;
}

/**
 * @brief Read one figure by category-relative ordinal.
 * @param storage Storage service used for file operations.
 * @param category Category identifier.
 * @param figure_index Figure ordinal within the category.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_get_figure(Storage* storage, uint8_t category, uint16_t figure_index, AzFigure* out) {
    File* file;
    AzIndexHeader header;
    if(!out || !az_open_index(storage, &file, &header)) return false;
    AzIndexCategoryRecord category_record;
    bool ok = az_find_category_record(file, &header, category, &category_record) &&
              figure_index < category_record.category.count;
    AzIndexFigureRecord record;
    if(ok) ok = az_read_figure_record(file, &header, category_record.first_figure + figure_index, &record);
    if(ok) *out = record.figure;
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/**
 * @brief Test whether figure metadata matches a case-insensitive search query.
 * @param figure Figure metadata.
 * @param query Case-insensitive search query.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_figure_matches_query(const AzFigure* figure, const char* query) {
    return figure && (az_contains_nocase(figure->name, query) || az_contains_nocase(figure->id_hex, query));
}

/**
 * @brief Count indexed figures matching a search query.
 * @param file Open storage file.
 * @param header Validated database index header.
 * @param query Case-insensitive search query.
 * @param out_total Destination for the total number of matching records.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_search_count(File* file, const AzIndexHeader* header, const char* query, uint16_t* out_total) {
    uint32_t count = 0;
    for(uint32_t i = 0; i < header->figure_count; i++) {
        AzIndexFigureRecord record;
        if(!az_read_figure_record(file, header, i, &record)) return false;
        if(az_figure_matches_query(&record.figure, query)) count++;
    }
    *out_total = count > UINT16_MAX ? UINT16_MAX : (uint16_t)count;
    return true;
}

/**
 * @brief Read the visible window of figures matching a search query.
 * @param storage Storage service used for file operations.
 * @param query Case-insensitive search query.
 * @param selection Selected result ordinal.
 * @param out_rows Destination array for visible rows.
 * @param out_row_count Destination for the number of populated output rows.
 * @param out_window_start Destination for the absolute ordinal of the first returned row.
 * @param out_total Destination for the total number of matching records.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_search_window(
    Storage* storage,
    const char* query,
    uint16_t selection,
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total) {
    if(out_row_count) *out_row_count = 0;
    if(out_window_start) *out_window_start = 0;
    if(out_total) *out_total = 0;
    File* file;
    AzIndexHeader header;
    if(!out_rows || !az_open_index(storage, &file, &header)) return false;
    uint16_t total = 0;
    bool ok = az_search_count(file, &header, query ? query : "", &total);
    if(total && selection >= total) selection = (uint16_t)(total - 1);
    uint16_t start = az_window_start(selection, total);
    uint16_t match = 0;
    uint8_t rows = 0;
    for(uint32_t i = 0; ok && i < header.figure_count && rows < AZ_LIST_ROWS; i++) {
        AzIndexFigureRecord record;
        ok = az_read_figure_record(file, &header, i, &record);
        if(!ok || !az_figure_matches_query(&record.figure, query ? query : "")) continue;
        if(match >= start) out_rows[rows++] = record.figure;
        match++;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(out_row_count) *out_row_count = rows;
    if(out_window_start) *out_window_start = start;
    if(out_total) *out_total = total;
    return ok;
}

/**
 * @brief Read one search result by match ordinal.
 * @param storage Storage service used for file operations.
 * @param query Case-insensitive search query.
 * @param match_index Matching-result ordinal.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_search_get(Storage* storage, const char* query, uint16_t match_index, AzFigure* out) {
    File* file;
    AzIndexHeader header;
    if(!out || !az_open_index(storage, &file, &header)) return false;
    uint16_t match = 0;
    bool found = false;
    for(uint32_t i = 0; i < header.figure_count; i++) {
        AzIndexFigureRecord record;
        if(!az_read_figure_record(file, &header, i, &record)) break;
        if(!az_figure_matches_query(&record.figure, query ? query : "")) continue;
        if(match++ == match_index) {
            *out = record.figure;
            found = true;
            break;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return found;
}

/**
 * @brief Find a figure whose identifier exactly matches the requested ID.
 * @param storage Storage service used for file operations.
 * @param id Eight-byte Amiibo identifier.
 * @param out Destination for the computed result.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_find_by_id(Storage* storage, const uint8_t id[8], AzFigure* out) {
    File* file;
    AzIndexHeader header;
    if(!id || !out || !az_open_index(storage, &file, &header)) return false;
    bool found = false;
    for(uint32_t i = 0; i < header.figure_count; i++) {
        AzIndexFigureRecord record;
        if(!az_read_figure_record(file, &header, i, &record)) break;
        if(memcmp(record.figure.id, id, 8) == 0) {
            *out = record.figure;
            found = true;
            break;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return found;
}

/**
 * @brief Map a game-platform key to a short display label.
 * @param key Cryptographic key bytes.
 * @return Pointer to the selected text, or NULL when no text is available.
 */
static const char* az_platform_label(const char* key) {
    if(!key) return NULL;
    if(strcmp(key, "games3DS") == 0) return "3DS";
    if(strcmp(key, "gamesWiiU") == 0) return "Wii U";
    if(strcmp(key, "gamesSwitch") == 0) return "Switch";
    if(strcmp(key, "gamesSwitch2") == 0) return "Switch 2";
    return NULL;
}

/**
 * @brief Test an Amiibo identifier against a game-reference byte pattern.
 * @param pattern Eight-byte Amiibo match pattern.
 * @param id Eight-byte Amiibo identifier.
 * @return true when the tested condition is satisfied; false otherwise.
 */
static bool az_pattern_matches(const uint8_t pattern[8], const uint8_t id[8]) {
    for(size_t i = 0; i < 8; i++) {
        if(pattern[i] != 0 && pattern[i] != id[i]) return false;
    }
    return true;
}

/**
 * @brief Detect whether a game compatibility entry is already present.
 * @param games Game compatibility array.
 * @param count Number of records or elements.
 * @param platform Platform label.
 * @param name Display name.
 * @param usage Usage description.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_game_duplicate(
    const AzGame* games,
    uint16_t count,
    const char* platform,
    const char* name,
    const char* usage) {
    for(uint16_t i = 0; i < count; i++) {
        if(strcmp(games[i].platform, platform) == 0 && strcmp(games[i].name, name) == 0 &&
           strcmp(games[i].usage, usage) == 0) return true;
    }
    return false;
}

/**
 * @brief Append a game compatibility entry to the current range result.
 * @param context Caller-owned callback context.
 * @param usage_text Game usage text to append.
 * @param writes Whether the game interaction writes data to the Amiibo.
 */
static void az_game_add(AzGamesRangeContext* context, const char* usage_text, bool writes) {
    if(!context || !context->game_name[0] || context->count >= context->max_games) return;
    const char* text = usage_text && usage_text[0] ? usage_text : "Compatible";
    if(az_game_duplicate(context->games, context->count, context->platform, context->game_name, text)) return;
    AzGame* game = &context->games[context->count++];
    memset(game, 0, sizeof(*game));
    az_str_copy(game->platform, sizeof(game->platform), context->platform);
    az_str_copy(game->name, sizeof(game->name), context->game_name);
    az_str_copy(game->usage, sizeof(game->usage), text);
    game->writes = writes;
}

/**
 * @brief Commit pending usage records once enough game context is available.
 * @param context Caller-owned callback context.
 */
static void az_game_flush_pending(AzGamesRangeContext* context) {
    if(!context || !context->game_name[0]) return;
    for(uint8_t i = 0; i < context->pending_count; i++) {
        az_game_add(context, context->pending[i].text, context->pending[i].writes);
    }
    context->pending_count = 0;
}

/**
 * @brief Consume one streaming JSON event while materializing game compatibility data.
 * @param parser Streaming JSON parser state.
 * @param type Type or event code to interpret.
 */
static void az_games_range_event(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    AzGamesRangeContext* context = lwjson_stream_get_user_data(parser);
    if(!context) return;
    if(type == LWJSON_STREAM_TYPE_OBJECT) {
        const char* nearest = az_lw_key(parser, 0);
        const char* platform = az_platform_label(nearest);
        if(platform && parser->stack_pos > 0 &&
           parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
            context->in_game = true;
            context->in_usage = false;
            context->pending_count = 0;
            context->saw_usage = false;
            context->game_name[0] = '\0';
            az_str_copy(context->platform, sizeof(context->platform), platform);
            return;
        }
        if(context->in_game && nearest && strcmp(nearest, "amiiboUsage") == 0 &&
           parser->stack_pos > 0 && parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
            context->in_usage = true;
            context->saw_usage = true;
            memset(&context->usage, 0, sizeof(context->usage));
        }
        return;
    }
    if(type == LWJSON_STREAM_TYPE_STRING) {
        if(context->in_game && az_lw_key_is(parser, 0, "gameName")) {
            az_copy_stream_string(parser, context->game_name, sizeof(context->game_name), &context->decoder);
            az_clean_field(context->game_name);
            if(parser->data.str.is_last) az_game_flush_pending(context);
        } else if(context->in_usage && az_lw_key_is(parser, 0, "Usage")) {
            az_copy_stream_string(parser, context->usage.text, sizeof(context->usage.text), &context->decoder);
            az_clean_field(context->usage.text);
        }
        return;
    }
    if(type == LWJSON_STREAM_TYPE_TRUE && context->in_usage && az_lw_key_is(parser, 0, "write")) {
        context->usage.writes = true;
        return;
    }
    if(type == LWJSON_STREAM_TYPE_FALSE && context->in_usage && az_lw_key_is(parser, 0, "write")) {
        context->usage.writes = false;
        return;
    }
    if(type == LWJSON_STREAM_TYPE_OBJECT_END) {
        const char* nearest = az_lw_key(parser, 0);
        if(context->in_usage && nearest && strcmp(nearest, "amiiboUsage") == 0 &&
           parser->stack_pos > 0 && parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
            context->in_usage = false;
            if(context->game_name[0]) az_game_add(context, context->usage.text, context->usage.writes);
            else if(context->pending_count < COUNT_OF(context->pending)) context->pending[context->pending_count++] = context->usage;
            return;
        }
        const char* platform = az_platform_label(nearest);
        if(context->in_game && platform && parser->stack_pos > 0 &&
           parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
            az_game_flush_pending(context);
            if(!context->saw_usage) az_game_add(context, "Compatible", false);
            context->in_game = false;
            context->platform[0] = '\0';
        }
    }
}

/**
 * @brief Parse one indexed game JSON range and append matching compatibility entries.
 * @param storage Storage service used for file operations.
 * @param ref Indexed game-reference record to parse.
 * @param games Game compatibility array.
 * @param max_games Capacity of the output game array.
 * @param in_out_count Input/output value for count.
 * @return true on success; false if the operation cannot be completed.
 */
static bool az_parse_game_ref(
    Storage* storage,
    const AzIndexGameRef* ref,
    AzGame* games,
    uint16_t max_games,
    uint16_t* in_out_count) {
    AzGamesRangeContext* context = calloc(1, sizeof(AzGamesRangeContext));
    if(!context) return false;
    context->games = games;
    context->max_games = max_games;
    context->count = *in_out_count;
    bool ok = az_scan_json_range(
        storage,
        AZ_GAMES_JSON,
        ref->json_offset,
        ref->json_length,
        az_games_range_event,
        context,
        &context->control);
    *in_out_count = context->count;
    free(context);
    return ok;
}

/**
 * @brief Load game compatibility records matching an Amiibo identifier.
 * @param storage Storage service used for file operations.
 * @param id Eight-byte Amiibo identifier.
 * @param out_games Destination array for game compatibility records.
 * @param max_games Capacity of the output game array.
 * @param out_count Destination for a resulting record count.
 * @return true on success; false if the operation cannot be completed.
 */
bool az_db_load_games(
    Storage* storage,
    const uint8_t id[8],
    AzGame* out_games,
    uint16_t max_games,
    uint16_t* out_count) {
    if(out_count) *out_count = 0;
    if(!storage || !id || !out_games || !out_count || max_games == 0) return false;
    File* file;
    AzIndexHeader header;
    if(!az_open_index(storage, &file, &header)) return false;
    if(header.game_ref_count == 0) {
        storage_file_close(file);
        storage_file_free(file);
        return storage_file_exists(storage, AZ_GAMES_JSON) ? true : false;
    }
    bool ok = storage_file_seek(file, header.games_offset, true);
    uint16_t count = 0;
    for(uint32_t i = 0; ok && i < header.game_ref_count && count < max_games; i++) {
        AzIndexGameRef ref;
        ok = az_read_exact(file, &ref, sizeof(ref));
        if(!ok) break;
        if(!az_json_range_valid(header.games_source.size, ref.json_offset, ref.json_length)) {
            ok = false;
            break;
        }
        if(az_pattern_matches(ref.pattern, id)) {
            ok = az_parse_game_ref(storage, &ref, out_games, max_games, &count);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    *out_count = count;
    return ok;
}
