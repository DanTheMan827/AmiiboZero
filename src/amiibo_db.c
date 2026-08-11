/**
 * @file amiibo_db.c
 * @brief On-device AmiiboAPI access using lwJSON's fixed-memory stream parser.
 *
 * The default path reads the raw JSON databases directly from SD. A legacy
 * on-device `.idx` cache remains available behind AZ_FEATURE_JSON_INDEX, which
 * defaults to zero. No complete JSON document or JSON object is materialized.
 */

#include "./amiibo_zero.h"
#include "./third_party/lwjson/lwjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AZ_JSON_FILE_BUFFER 512U
#define AZ_INDEX_HEADER "#amiibo-zero-index-v3"

/** @brief Control block shared by all streaming scans. */
typedef struct {
    bool stop;   /**< Stop feeding the current JSON file early. */
    bool failed; /**< Callback-level failure independent of lwJSON syntax status. */
} AzScanControl;

/** @brief Stateful decoder used to copy one streamed JSON string into a bounded field. */
typedef struct {
    bool escaped;          /**< Previous raw byte was a backslash. */
    uint8_t unicode_left;  /**< Remaining hexadecimal digits in a \u escape. */
    uint16_t unicode;      /**< Accumulated basic multilingual plane code point. */
} AzStringDecoder;

/** @brief Buffered line reader used only by the optional index implementation. */
typedef struct {
    File* file;                              /**< Open Storage file. */
    uint8_t buffer[AZ_JSON_FILE_BUFFER];     /**< Small fixed read-ahead buffer. */
    size_t position;                         /**< Current byte within buffer. */
    size_t length;                           /**< Valid bytes in buffer. */
} AzLineReader;

/** @brief Callback invoked once for each complete Amiibo record streamed from amiibo.json. */
typedef bool (*AzFigureVisitor)(const AzFigure* figure, void* context);

/** @brief Parser context that assembles only the currently visited Amiibo record. */
typedef struct {
    AzScanControl control;       /**< Scan cancellation/error state. */
    AzFigureVisitor visitor;     /**< Consumer for completed figure records. */
    void* visitor_context;       /**< Consumer-owned state. */
    AzFigure current;            /**< Current bounded figure record. */
    bool in_figure;              /**< True while inside a top-level amiibos member. */
    AzStringDecoder decoder;     /**< Decoder for the current string field. */
} AzFigureParserContext;

/** @brief Parser context for the small amiibo_series name map. */
typedef struct {
    AzScanControl control;                   /**< Scan cancellation/error state. */
    AzCategory* categories;                  /**< Bounded destination category table. */
    uint16_t capacity;                       /**< Maximum entries in categories. */
    uint16_t count;                          /**< Number of populated entries. */
    AzStringDecoder decoder;                 /**< Decoder for category-name strings. */
} AzCategoryParserContext;

/** @brief Figure-selection direction used for bounded alphabetical window scans. */
typedef enum {
    AzPickLowest,  /**< Keep the alphabetically smallest matching figures. */
    AzPickHighest, /**< Keep the alphabetically largest matching figures. */
    AzPickAfter,   /**< Keep smallest matches strictly after an anchor. */
    AzPickBefore,  /**< Keep largest matches strictly before an anchor. */
} AzPickMode;

/** @brief State used by one bounded figure-window scan. */
typedef struct {
    bool search;                         /**< Apply query filtering instead of category filtering. */
    uint8_t category;                    /**< Required category when search is false. */
    char query[AZ_QUERY_MAX];            /**< Case-insensitive name/ID search term. */
    AzPickMode mode;                     /**< Which side of the sorted set to retain. */
    AzFigure anchor;                     /**< Exclusive comparison anchor for before/after. */
    bool has_anchor;                     /**< Whether anchor participates in filtering. */
    AzFigure rows[AZ_LIST_ROWS];         /**< Bounded sorted result window. */
    uint8_t row_count;                   /**< Number of valid rows. */
    uint16_t total;                      /**< Total figures matching category/query. */
} AzPickContext;

/** @brief Cached direct-JSON window so animation ticks never rescan the database. */
typedef struct {
    bool valid;                          /**< Cache contains a usable window. */
    Storage* storage;                    /**< Storage instance that produced the cache. */
    bool search;                         /**< Cache represents search rather than a category. */
    uint8_t category;                    /**< Category identity for category windows. */
    char query[AZ_QUERY_MAX];            /**< Search identity for search windows. */
    uint16_t start;                      /**< Absolute ordinal of rows[0]. */
    uint16_t total;                      /**< Total matches in the sorted set. */
    AzFigure rows[AZ_LIST_ROWS];         /**< Four visible sorted figures. */
    uint8_t row_count;                   /**< Number of valid rows. */
} AzFigureWindowCache;

/** @brief One compatibility usage record while a game object is being streamed. */
typedef struct {
    char text[AZ_USAGE_MAX]; /**< Human-readable usage description. */
    bool writes;             /**< Whether the game can modify Amiibo data. */
} AzUsage;

/** @brief lwJSON state for direct games_info.json compatibility extraction. */
typedef struct {
    AzScanControl control;             /**< Scan cancellation/error state. */
    const uint8_t* figure_id;          /**< Selected Amiibo ID. */
    AzGame* games;                     /**< Caller-provided bounded result array. */
    uint16_t max_games;                /**< Capacity of games. */
    uint16_t count;                    /**< Number of result entries. */
    bool matching_entry;               /**< Current Amiibo pattern matches figure_id. */
    bool in_game;                      /**< Currently inside one platform game object. */
    bool in_usage;                     /**< Currently inside one amiiboUsage object. */
    char platform[9];                  /**< Label for the current platform array. */
    char game_name[AZ_NAME_MAX];       /**< Current game name. */
    AzUsage usage;                     /**< Current usage item. */
    AzUsage pending[8];                /**< Small order-robust buffer if usage precedes gameName. */
    uint8_t pending_count;             /**< Number of pending usages. */
    bool saw_usage;                    /**< Current game had at least one usage object. */
    AzStringDecoder decoder;           /**< Decoder for the current string field. */
} AzGamesParserContext;

/** @brief Module-local category cache; size is bounded independently of JSON file size. */
static AzCategory az_category_cache[AZ_MAX_CATEGORIES];
/** @brief Number of populated entries in az_category_cache. */
static uint16_t az_category_cache_count;
/** @brief Storage object associated with the category cache. */
static Storage* az_category_cache_storage;
/** @brief True after both the series map and figure counts have been scanned successfully. */
static bool az_category_cache_ready;
#if !AZ_FEATURE_JSON_INDEX
/** @brief Cached figure/search window for direct JSON browsing. */
static AzFigureWindowCache az_figure_window_cache;
#endif

/** @brief Convert one ASCII letter to lowercase without importing locale/ctype helpers. */
static char az_ascii_lower(char c) {
    if(c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/** @brief Compare two NUL-terminated strings case-insensitively using ASCII rules. */
static int az_compare_nocase(const char* left, const char* right) {
    if(!left) left = "";
    if(!right) right = "";
    while(*left && *right) {
        char a = az_ascii_lower(*left++);
        char b = az_ascii_lower(*right++);
        if(a < b) return -1;
        if(a > b) return 1;
    }
    if(*left) return 1;
    if(*right) return -1;
    return 0;
}

/** @brief Return true when needle occurs in haystack using ASCII case folding. */
static bool az_contains_nocase(const char* haystack, const char* needle) {
    if(!needle || !needle[0]) return true;
    if(!haystack) return false;
    size_t nlen = strlen(needle);
    for(const char* h = haystack; *h; ++h) {
        size_t i = 0;
        while(i < nlen && h[i] && az_ascii_lower(h[i]) == az_ascii_lower(needle[i])) i++;
        if(i == nlen) return true;
    }
    return false;
}

/** @brief Convert one hexadecimal ASCII digit to its numeric value, or -1 if invalid. */
static int az_hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** @brief Parse one optional-0x-prefixed hexadecimal byte. */
static bool az_parse_hex_byte(const char* text, uint8_t* out) {
    if(!text || !out) return false;
    if(text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if(strlen(text) != 2) return false;
    int hi = az_hex_nibble(text[0]);
    int lo = az_hex_nibble(text[1]);
    if(hi < 0 || lo < 0) return false;
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}

/** @brief Parse a full 8-byte Amiibo identifier from its 16-digit JSON member name. */
static bool az_parse_id_text(const char* text, uint8_t out[8], char out_hex[17]) {
    if(!text || !out) return false;
    if(text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if(strlen(text) != 16) return false;
    for(size_t i = 0; i < 8; i++) {
        int hi = az_hex_nibble(text[i * 2]);
        int lo = az_hex_nibble(text[i * 2 + 1]);
        if(hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    if(out_hex) {
        for(size_t i = 0; i < 16; i++) out_hex[i] = az_ascii_lower(text[i]);
        out_hex[16] = '\0';
    }
    return true;
}

/** @brief Replace field separators/control newlines with spaces before UI/index use. */
static void az_clean_field(char* text) {
    if(!text) return;
    for(char* p = text; *p; ++p) {
        if(*p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\b') *p = ' ';
    }
}

/** @brief Return the nth nearest KEY name currently retained on the lwJSON stack. */
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

/** @brief Compare the nth nearest lwJSON key with a literal string. */
static bool az_lw_key_is(const lwjson_stream_parser_t* parser, size_t ordinal, const char* key) {
    const char* actual = az_lw_key(parser, ordinal);
    return actual && key && strcmp(actual, key) == 0;
}

/** @brief Return the raw-string offset represented by the current lwJSON string chunk. */
static size_t az_lw_string_piece_offset(const lwjson_stream_parser_t* parser) {
    if(!parser || parser->data.str.buff_total_pos < parser->data.str.buff_pos) return 0;
    return parser->data.str.buff_total_pos - parser->data.str.buff_pos;
}

/** @brief Append one byte to a bounded output string, preserving NUL termination. */
static void az_text_append(char* destination, size_t capacity, size_t* position, char value) {
    if(!destination || !position || capacity == 0) return;
    if(*position + 1 < capacity) destination[(*position)++] = value;
    destination[*position < capacity ? *position : capacity - 1] = '\0';
}

/** @brief Append a BMP code point as UTF-8, substituting '?' for unsupported surrogates. */
static void az_text_append_codepoint(
    char* destination,
    size_t capacity,
    size_t* position,
    uint16_t codepoint) {
    if(codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
        az_text_append(destination, capacity, position, '?');
    } else if(codepoint < 0x80U) {
        az_text_append(destination, capacity, position, (char)codepoint);
    } else if(codepoint < 0x800U) {
        az_text_append(destination, capacity, position, (char)(0xC0U | (codepoint >> 6)));
        az_text_append(destination, capacity, position, (char)(0x80U | (codepoint & 0x3FU)));
    } else {
        az_text_append(destination, capacity, position, (char)(0xE0U | (codepoint >> 12)));
        az_text_append(destination, capacity, position, (char)(0x80U | ((codepoint >> 6) & 0x3FU)));
        az_text_append(destination, capacity, position, (char)(0x80U | (codepoint & 0x3FU)));
    }
}

/** @brief Decode and append the current lwJSON raw string chunk directly into a bounded field. */
static void az_copy_stream_string(
    const lwjson_stream_parser_t* parser,
    char* destination,
    size_t capacity,
    AzStringDecoder* decoder) {
    if(!parser || !destination || capacity == 0 || !decoder) return;
    if(az_lw_string_piece_offset(parser) == 0) {
        destination[0] = '\0';
        memset(decoder, 0, sizeof(*decoder));
    }
    size_t output_pos = strlen(destination);
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
            if(decoder->unicode_left == 0) {
                az_text_append_codepoint(destination, capacity, &output_pos, decoder->unicode);
                decoder->unicode = 0;
            }
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

/** @brief Feed an SD-resident JSON document through one heap-allocated lwJSON parser. */
static bool az_scan_json_file(
    Storage* storage,
    const char* path,
    lwjson_stream_parser_callback_fn callback,
    void* callback_context,
    AzScanControl* control) {
    if(!storage || !path || !callback || !control) return false;
    control->stop = false;
    control->failed = false;

    File* file = storage_file_alloc(storage);
    if(!file) return false;
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        return false;
    }

    lwjson_stream_parser_t* parser = malloc(sizeof(*parser));
    if(!parser) {
        storage_file_close(file);
        storage_file_free(file);
        return false;
    }
    if(lwjson_stream_init(parser, callback) != lwjsonOK ||
       lwjson_stream_set_user_data(parser, callback_context) != lwjsonOK) {
        free(parser);
        storage_file_close(file);
        storage_file_free(file);
        return false;
    }

    uint8_t buffer[AZ_JSON_FILE_BUFFER];
    bool done = false;
    bool ok = true;
    while(!control->stop) {
        size_t read = storage_file_read(file, buffer, sizeof(buffer));
        if(read == 0) break;
        for(size_t i = 0; i < read; i++) {
            lwjsonr_t result = lwjson_stream_parse(parser, (char)buffer[i]);
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
    }

    if(control->failed) ok = false;
    if(!done && !control->stop) ok = false;
    free(parser);
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/** @brief Locate a category by series byte in a caller-provided bounded array. */
static AzCategory* az_find_category(AzCategory* categories, uint16_t count, uint8_t id) {
    for(uint16_t i = 0; i < count; i++) {
        if(categories[i].id == id) return &categories[i];
    }
    return NULL;
}

/** @brief Add a fallback name for a series byte absent from amiibo_series. */
static bool az_add_unknown_category(
    AzCategory* categories,
    uint16_t capacity,
    uint16_t* count,
    uint8_t id) {
    if(!categories || !count || *count >= capacity) return false;
    AzCategory* category = &categories[(*count)++];
    memset(category, 0, sizeof(*category));
    category->id = id;
    snprintf(category->name, sizeof(category->name), "Series %02X", id);
    return true;
}

/** @brief Handle lwJSON events while extracting the amiibo_series object. */
static void az_category_json_event(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    AzCategoryParserContext* context = lwjson_stream_get_user_data(parser);
    if(!context || type != LWJSON_STREAM_TYPE_STRING) return;
    if(!az_lw_key_is(parser, 1, "amiibo_series")) return;

    const char* id_text = az_lw_key(parser, 0);
    uint8_t id = 0;
    if(!id_text || !az_parse_hex_byte(id_text, &id)) return;
    AzCategory* category = az_find_category(context->categories, context->count, id);
    if(!category) {
        if(context->count >= context->capacity) {
            context->control.failed = true;
            context->control.stop = true;
            return;
        }
        category = &context->categories[context->count++];
        memset(category, 0, sizeof(*category));
        category->id = id;
    }
    az_copy_stream_string(parser, category->name, sizeof(category->name), &context->decoder);
    az_clean_field(category->name);
}

/** @brief Compare two categories alphabetically and then by series byte. */
static int az_category_compare(const AzCategory* left, const AzCategory* right) {
    int by_name = az_compare_nocase(left->name, right->name);
    if(by_name) return by_name;
    return left->id < right->id ? -1 : left->id > right->id ? 1 : 0;
}

/** @brief Insertion-sort the bounded category table without importing qsort. */
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

/** @brief Return true when the current lwJSON stack identifies an amiibos member object. */
static bool az_lw_is_amiibo_entry(const lwjson_stream_parser_t* parser, uint8_t id[8], char id_hex[17]) {
    const char* entry = az_lw_key(parser, 0);
    if(!entry || !az_lw_key_is(parser, 1, "amiibos")) return false;
    return az_parse_id_text(entry, id, id_hex);
}

/** @brief Handle lwJSON events and assemble only one bounded AzFigure at a time. */
static void az_figure_json_event(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    AzFigureParserContext* context = lwjson_stream_get_user_data(parser);
    if(!context) return;

    if(type == LWJSON_STREAM_TYPE_OBJECT) {
        uint8_t id[8];
        char id_hex[17];
        if(az_lw_is_amiibo_entry(parser, id, id_hex)) {
            memset(&context->current, 0, sizeof(context->current));
            memcpy(context->current.id, id, sizeof(id));
            az_str_copy(context->current.id_hex, sizeof(context->current.id_hex), id_hex);
            context->current.category = id[6];
            context->in_figure = true;
        }
        return;
    }

    if(type == LWJSON_STREAM_TYPE_STRING && context->in_figure) {
        if(az_lw_key_is(parser, 0, "name") && az_parse_id_text(az_lw_key(parser, 1), (uint8_t[8]){0}, NULL)) {
            az_copy_stream_string(
                parser,
                context->current.name,
                sizeof(context->current.name),
                &context->decoder);
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
        if(context->current.name[0] && context->visitor &&
           !context->visitor(&context->current, context->visitor_context)) {
            context->control.stop = true;
        }
    }
}

/** @brief Stream every Amiibo record from amiibo.json through a caller visitor. */
static bool az_scan_figures(Storage* storage, AzFigureVisitor visitor, void* visitor_context) {
    AzFigureParserContext context = {
        .visitor = visitor,
        .visitor_context = visitor_context,
    };
    return az_scan_json_file(
        storage,
        AZ_AMIIBO_JSON,
        az_figure_json_event,
        &context,
        &context.control);
}

/** @brief Visitor that increments category counts without retaining figure records. */
static bool az_count_category_visitor(const AzFigure* figure, void* context) {
    AzCategoryParserContext* categories = context;
    AzCategory* category =
        az_find_category(categories->categories, categories->count, figure->category);
    if(!category) {
        if(!az_add_unknown_category(
               categories->categories,
               categories->capacity,
               &categories->count,
               figure->category)) {
            categories->control.failed = true;
            return false;
        }
        category = az_find_category(categories->categories, categories->count, figure->category);
    }
    if(category->count < UINT16_MAX) category->count++;
    return true;
}

/** @brief Populate and alphabetize the small module-local category cache from raw JSON. */
static bool az_categories_ensure(Storage* storage, bool force) {
    if(!storage) return false;
    if(!force && az_category_cache_ready && az_category_cache_storage == storage) return true;

    memset(az_category_cache, 0, sizeof(az_category_cache));
    az_category_cache_count = 0;
    az_category_cache_ready = false;
    az_category_cache_storage = storage;
#if !AZ_FEATURE_JSON_INDEX
    memset(&az_figure_window_cache, 0, sizeof(az_figure_window_cache));
#endif

    AzCategoryParserContext context = {
        .categories = az_category_cache,
        .capacity = AZ_MAX_CATEGORIES,
    };
    bool ok = az_scan_json_file(
        storage,
        AZ_AMIIBO_JSON,
        az_category_json_event,
        &context,
        &context.control);
    if(!ok) return false;

    context.control.stop = false;
    context.control.failed = false;
    ok = az_scan_figures(storage, az_count_category_visitor, &context);
    if(!ok || context.control.failed) return false;

    uint16_t write = 0;
    for(uint16_t read = 0; read < context.count; read++) {
        if(context.categories[read].count > 0) context.categories[write++] = context.categories[read];
    }
    context.count = write;
    az_sort_categories(context.categories, context.count);
    az_category_cache_count = context.count;
    az_category_cache_ready = true;
    return true;
}

/** @brief Compare two figures alphabetically and then by full hexadecimal Amiibo ID. */
static int az_figure_compare(const AzFigure* left, const AzFigure* right) {
    int by_name = az_compare_nocase(left->name, right->name);
    if(by_name) return by_name;
    return strcmp(left->id_hex, right->id_hex);
}

#if !AZ_FEATURE_JSON_INDEX

/** @brief Insert a figure into an ascending fixed-size array, keeping the smallest entries. */
static void az_keep_lowest(AzFigure rows[AZ_LIST_ROWS], uint8_t* count, const AzFigure* figure) {
    if(*count < AZ_LIST_ROWS) {
        rows[(*count)++] = *figure;
    } else if(az_figure_compare(figure, &rows[*count - 1]) >= 0) {
        return;
    } else {
        rows[*count - 1] = *figure;
    }
    for(uint8_t i = 1; i < *count; i++) {
        AzFigure current = rows[i];
        uint8_t j = i;
        while(j > 0 && az_figure_compare(&rows[j - 1], &current) > 0) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = current;
    }
}

/** @brief Insert a figure into an ascending fixed-size array, keeping the largest entries. */
static void az_keep_highest(AzFigure rows[AZ_LIST_ROWS], uint8_t* count, const AzFigure* figure) {
    if(*count < AZ_LIST_ROWS) {
        rows[(*count)++] = *figure;
    } else if(az_figure_compare(figure, &rows[0]) <= 0) {
        return;
    } else {
        rows[0] = *figure;
    }
    for(uint8_t i = 1; i < *count; i++) {
        AzFigure current = rows[i];
        uint8_t j = i;
        while(j > 0 && az_figure_compare(&rows[j - 1], &current) > 0) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = current;
    }
}

/** @brief Return true when a figure satisfies the category or search filter in a pick scan. */
static bool az_pick_matches(const AzPickContext* context, const AzFigure* figure) {
    if(context->search) {
        return az_contains_nocase(figure->name, context->query) ||
               az_contains_nocase(figure->id_hex, context->query);
    }
    return figure->category == context->category;
}

/** @brief Visitor that counts matches and retains only one four-row alphabetical edge/window. */
static bool az_pick_visitor(const AzFigure* figure, void* raw_context) {
    AzPickContext* context = raw_context;
    if(!az_pick_matches(context, figure)) return true;
    if(context->total < UINT16_MAX) context->total++;

    if(context->mode == AzPickAfter &&
       (!context->has_anchor || az_figure_compare(figure, &context->anchor) <= 0)) {
        return true;
    }
    if(context->mode == AzPickBefore &&
       (!context->has_anchor || az_figure_compare(figure, &context->anchor) >= 0)) {
        return true;
    }

    if(context->mode == AzPickHighest || context->mode == AzPickBefore) {
        az_keep_highest(context->rows, &context->row_count, figure);
    } else {
        az_keep_lowest(context->rows, &context->row_count, figure);
    }
    return true;
}

/** @brief Run one raw-JSON pass to collect a bounded sorted figure edge relative to an anchor. */
static bool az_pick_scan(Storage* storage, AzPickContext* context) {
    context->row_count = 0;
    context->total = 0;
    memset(context->rows, 0, sizeof(context->rows));
    return az_scan_figures(storage, az_pick_visitor, context);
}

/** @brief Return true when the direct figure cache belongs to the requested category/search identity. */
static bool az_window_identity_matches(
    const AzFigureWindowCache* cache,
    Storage* storage,
    bool search,
    uint8_t category,
    const char* query) {
    if(!cache->valid || cache->storage != storage || cache->search != search) return false;
    if(search) return strcmp(cache->query, query ? query : "") == 0;
    return cache->category == category;
}

/** @brief Fill the direct window cache with the first or last four sorted matches. */
static bool az_window_fill_edge(
    Storage* storage,
    bool search,
    uint8_t category,
    const char* query,
    bool highest) {
    AzPickContext pick = {
        .search = search,
        .category = category,
        .mode = highest ? AzPickHighest : AzPickLowest,
    };
    az_str_copy(pick.query, sizeof(pick.query), query ? query : "");
    if(!az_pick_scan(storage, &pick)) return false;

#if !AZ_FEATURE_JSON_INDEX
    memset(&az_figure_window_cache, 0, sizeof(az_figure_window_cache));
#endif
    az_figure_window_cache.valid = true;
    az_figure_window_cache.storage = storage;
    az_figure_window_cache.search = search;
    az_figure_window_cache.category = category;
    az_str_copy(az_figure_window_cache.query, sizeof(az_figure_window_cache.query), query ? query : "");
    az_figure_window_cache.total = pick.total;
    az_figure_window_cache.row_count = pick.row_count;
    memcpy(az_figure_window_cache.rows, pick.rows, sizeof(pick.rows));
    az_figure_window_cache.start =
        highest && pick.total > pick.row_count ? (uint16_t)(pick.total - pick.row_count) : 0;
    return true;
}

/** @brief Advance the direct cache by up to four records after its current last row. */
static bool az_window_next_page(void) {
    AzFigureWindowCache* cache = &az_figure_window_cache;
    if(!cache->valid || cache->row_count == 0) return false;
    uint16_t old_start = cache->start;
    uint8_t old_count = cache->row_count;
    AzPickContext pick = {
        .search = cache->search,
        .category = cache->category,
        .mode = AzPickAfter,
        .anchor = cache->rows[cache->row_count - 1],
        .has_anchor = true,
    };
    az_str_copy(pick.query, sizeof(pick.query), cache->query);
    if(!az_pick_scan(cache->storage, &pick) || pick.row_count == 0) return false;
    cache->start = (uint16_t)(old_start + old_count);
    cache->total = pick.total;
    cache->row_count = pick.row_count;
    memcpy(cache->rows, pick.rows, sizeof(pick.rows));
    return true;
}

/** @brief Move the direct cache to up to four records immediately before its first row. */
static bool az_window_previous_page(void) {
    AzFigureWindowCache* cache = &az_figure_window_cache;
    if(!cache->valid || cache->row_count == 0 || cache->start == 0) return false;
    AzPickContext pick = {
        .search = cache->search,
        .category = cache->category,
        .mode = AzPickBefore,
        .anchor = cache->rows[0],
        .has_anchor = true,
    };
    az_str_copy(pick.query, sizeof(pick.query), cache->query);
    if(!az_pick_scan(cache->storage, &pick) || pick.row_count == 0) return false;
    cache->start = cache->start > pick.row_count ? (uint16_t)(cache->start - pick.row_count) : 0;
    cache->total = pick.total;
    cache->row_count = pick.row_count;
    memcpy(cache->rows, pick.rows, sizeof(pick.rows));
    return true;
}

/** @brief Ensure the direct cache contains the requested absolute selection ordinal. */
static bool az_window_seek(
    Storage* storage,
    bool search,
    uint8_t category,
    const char* query,
    uint16_t selection) {
    if(!az_window_identity_matches(
           &az_figure_window_cache,
           storage,
           search,
           category,
           query)) {
        if(!az_window_fill_edge(storage, search, category, query, false)) return false;
    }

    AzFigureWindowCache* cache = &az_figure_window_cache;
    if(cache->total == 0) return true;
    if(selection >= cache->total) selection = (uint16_t)(cache->total - 1);

    if(selection == cache->total - 1 &&
       !(selection >= cache->start && selection < cache->start + cache->row_count)) {
        return az_window_fill_edge(storage, search, category, query, true);
    }

    while(selection < cache->start) {
        if(!az_window_previous_page()) return false;
    }
    while(selection >= (uint16_t)(cache->start + cache->row_count)) {
        if(!az_window_next_page()) return false;
    }
    return true;
}

/** @brief Copy the current direct cache into the public four-row window outputs. */
static void az_window_copy_outputs(
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total) {
    const AzFigureWindowCache* cache = &az_figure_window_cache;
    if(out_rows) memcpy(out_rows, cache->rows, sizeof(cache->rows));
    if(out_row_count) *out_row_count = cache->row_count;
    if(out_window_start) *out_window_start = cache->start;
    if(out_total) *out_total = cache->total;
}

#endif /* !AZ_FEATURE_JSON_INDEX */

/** @brief Compute a traditional centered window start for list windows. */
static uint16_t az_window_start(uint16_t selection, uint16_t total) {
    if(total <= AZ_LIST_ROWS) return 0;
    uint16_t start = selection >= 2 ? (uint16_t)(selection - 2) : 0;
    if((uint16_t)(start + AZ_LIST_ROWS) > total) start = (uint16_t)(total - AZ_LIST_ROWS);
    return start;
}

#if AZ_FEATURE_JSON_INDEX

/** @brief Initialize a small buffered line reader around an already-open file. */
static void az_line_reader_init(AzLineReader* reader, File* file) {
    memset(reader, 0, sizeof(*reader));
    reader->file = file;
}

/** @brief Return the next byte from a buffered line reader, or -1 at EOF. */
static int az_line_getc(AzLineReader* reader) {
    if(reader->position >= reader->length) {
        reader->length = storage_file_read(reader->file, reader->buffer, sizeof(reader->buffer));
        reader->position = 0;
        if(reader->length == 0) return -1;
    }
    return reader->buffer[reader->position++];
}

/** @brief Read one CR-stripped text line into a bounded buffer. */
static bool az_read_line(AzLineReader* reader, char* line, size_t size) {
    if(!reader || !line || size == 0) return false;
    size_t position = 0;
    while(true) {
        int c = az_line_getc(reader);
        if(c < 0) {
            if(position == 0) return false;
            break;
        }
        if(c == '\n') break;
        if(c == '\r') continue;
        if(position + 1 < size) line[position++] = (char)c;
    }
    line[position] = '\0';
    return true;
}

/** @brief Write a complete NUL-terminated string to a Storage file. */
static bool az_write_all(File* file, const char* text) {
    size_t length = strlen(text);
    return storage_file_write(file, text, length) == length;
}

/** @brief Parse one optional-index category line in place. */
static bool az_parse_category_line(char* line, AzCategory* category) {
    if(strncmp(line, "C\t", 2) != 0) return false;
    char* id = line + 2;
    char* tab1 = strchr(id, '\t');
    if(!tab1) return false;
    *tab1 = '\0';
    char* name = tab1 + 1;
    char* tab2 = strchr(name, '\t');
    if(!tab2) return false;
    *tab2 = '\0';
    char* count = tab2 + 1;
    memset(category, 0, sizeof(*category));
    if(!az_parse_hex_byte(id, &category->id)) return false;
    az_str_copy(category->name, sizeof(category->name), name);
    category->count = (uint16_t)strtoul(count, NULL, 10);
    return true;
}

/** @brief Parse one optional-index figure line in place. */
static bool az_parse_figure_line(char* line, AzFigure* figure) {
    if(strncmp(line, "F\t", 2) != 0) return false;
    char* category = line + 2;
    char* tab1 = strchr(category, '\t');
    if(!tab1) return false;
    *tab1 = '\0';
    char* id = tab1 + 1;
    char* tab2 = strchr(id, '\t');
    if(!tab2) return false;
    *tab2 = '\0';
    char* name = tab2 + 1;
    char* tab3 = strchr(name, '\t');
    if(!tab3) return false;
    *tab3 = '\0';
    char* release = tab3 + 1;
    memset(figure, 0, sizeof(*figure));
    if(!az_parse_hex_byte(category, &figure->category)) return false;
    if(!az_parse_id_text(id, figure->id, figure->id_hex)) return false;
    az_str_copy(figure->name, sizeof(figure->name), name);
    az_str_copy(figure->release_na, sizeof(figure->release_na), release);
    return true;
}

/** @brief Parse one temporary raw-index line in place. */
static bool az_parse_raw_line(char* line, uint8_t* category, AzFigure* figure) {
    if(strncmp(line, "R\t", 2) != 0) return false;
    line[0] = 'F';
    if(!az_parse_figure_line(line, figure)) return false;
    *category = figure->category;
    return true;
}

/** @brief Open and validate the optional index file and initialize its line reader. */
static bool az_open_index(Storage* storage, File** out_file, AzLineReader* reader) {
    File* file = storage_file_alloc(storage);
    if(!file) return false;
    if(!storage_file_open(file, AZ_INDEX_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        return false;
    }
    az_line_reader_init(reader, file);
    char header[64];
    if(!az_read_line(reader, header, sizeof(header)) || strcmp(header, AZ_INDEX_HEADER) != 0) {
        storage_file_close(file);
        storage_file_free(file);
        return false;
    }
    *out_file = file;
    return true;
}

/** @brief Count figure records in an already-generated optional index. */
static bool az_index_count_figures(Storage* storage, uint32_t* out_count) {
    *out_count = 0;
    File* file = NULL;
    AzLineReader reader;
    if(!az_open_index(storage, &file, &reader)) return false;
    char line[AZ_NAME_MAX + 80];
    while(az_read_line(&reader, line, sizeof(line))) {
        if(strncmp(line, "F\t", 2) == 0) (*out_count)++;
    }
    storage_file_close(file);
    storage_file_free(file);
    return true;
}

/** @brief Context for writing streamed figures into the optional raw temporary index. */
typedef struct {
    File* file;          /**< Temporary raw index destination. */
    uint32_t count;      /**< Number of figure rows written. */
    bool failed;         /**< True after the first write failure. */
} AzRawIndexWriter;

/** @brief Visitor that writes one already-decoded figure to the optional raw index. */
static bool az_raw_index_visitor(const AzFigure* figure, void* raw_context) {
    AzRawIndexWriter* context = raw_context;
    char line[AZ_NAME_MAX + 80];
    snprintf(
        line,
        sizeof(line),
        "R\t%02X\t%s\t%s\t%s\n",
        figure->category,
        figure->id_hex,
        figure->name,
        figure->release_na);
    if(!az_write_all(context->file, line)) {
        context->failed = true;
        return false;
    }
    context->count++;
    return true;
}

/** @brief Emit all figures in one category in alphabetical order using repeated bounded scans. */
static bool az_write_sorted_category(Storage* storage, File* output, const AzCategory* category) {
    AzFigure last = {0};
    bool have_last = false;
    for(uint16_t ordinal = 0; ordinal < category->count; ordinal++) {
        File* raw = storage_file_alloc(storage);
        if(!raw || !storage_file_open(raw, AZ_INDEX_RAW, FSAM_READ, FSOM_OPEN_EXISTING)) {
            if(raw) storage_file_free(raw);
            return false;
        }
        AzLineReader reader;
        az_line_reader_init(&reader, raw);
        char line[AZ_NAME_MAX + 80];
        AzFigure best = {0};
        bool found = false;
        while(az_read_line(&reader, line, sizeof(line))) {
            uint8_t row_category = 0;
            AzFigure candidate;
            if(!az_parse_raw_line(line, &row_category, &candidate) || row_category != category->id) continue;
            if(have_last && az_figure_compare(&candidate, &last) <= 0) continue;
            if(!found || az_figure_compare(&candidate, &best) < 0) {
                best = candidate;
                found = true;
            }
        }
        storage_file_close(raw);
        storage_file_free(raw);
        if(!found) return false;
        char out_line[AZ_NAME_MAX + 80];
        snprintf(
            out_line,
            sizeof(out_line),
            "F\t%02X\t%s\t%s\t%s\n",
            category->id,
            best.id_hex,
            best.name,
            best.release_na);
        if(!az_write_all(output, out_line)) return false;
        last = best;
        have_last = true;
    }
    return true;
}

/** @brief Rebuild the optional index from raw JSON, using lwJSON for all JSON parsing. */
static bool az_index_build(Storage* storage, uint32_t* out_count) {
    if(!az_categories_ensure(storage, true)) return false;
    File* raw = storage_file_alloc(storage);
    if(!raw || !storage_file_open(raw, AZ_INDEX_RAW, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        if(raw) storage_file_free(raw);
        return false;
    }
    AzRawIndexWriter writer = {.file = raw};
    bool ok = az_scan_figures(storage, az_raw_index_visitor, &writer) && !writer.failed;
    storage_file_sync(raw);
    storage_file_close(raw);
    storage_file_free(raw);

    File* index = storage_file_alloc(storage);
    if(ok && (!index || !storage_file_open(index, AZ_INDEX_TMP, FSAM_WRITE, FSOM_CREATE_ALWAYS))) ok = false;
    if(ok) ok = az_write_all(index, AZ_INDEX_HEADER "\n");
    for(uint16_t i = 0; ok && i < az_category_cache_count; i++) {
        char line[AZ_NAME_MAX + 32];
        snprintf(
            line,
            sizeof(line),
            "C\t%02X\t%s\t%u\n",
            az_category_cache[i].id,
            az_category_cache[i].name,
            az_category_cache[i].count);
        ok = az_write_all(index, line);
    }
    for(uint16_t i = 0; ok && i < az_category_cache_count; i++) {
        ok = az_write_sorted_category(storage, index, &az_category_cache[i]);
    }
    if(index) {
        storage_file_sync(index);
        storage_file_close(index);
        storage_file_free(index);
    }
    if(ok) ok = storage_common_rename(storage, AZ_INDEX_TMP, AZ_INDEX_FILE) == FSE_OK;
    if(!ok) storage_common_remove(storage, AZ_INDEX_TMP);
    storage_common_remove(storage, AZ_INDEX_RAW);
    if(ok && out_count) *out_count = writer.count;
    return ok;
}

/** @brief Read an optional-index category window. */
static bool az_index_get_category_window(
    Storage* storage,
    uint16_t selection,
    AzCategory out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total) {
    File* file = NULL;
    AzLineReader reader;
    if(!az_open_index(storage, &file, &reader)) return false;
    uint16_t total = 0;
    char line[AZ_NAME_MAX + 80];
    while(az_read_line(&reader, line, sizeof(line))) {
        AzCategory category;
        if(az_parse_category_line(line, &category)) total++;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(total && selection >= total) selection = (uint16_t)(total - 1);
    uint16_t start = az_window_start(selection, total);
    uint8_t rows = 0;
    if(!az_open_index(storage, &file, &reader)) return false;
    uint16_t index = 0;
    while(az_read_line(&reader, line, sizeof(line))) {
        AzCategory category;
        if(!az_parse_category_line(line, &category)) continue;
        if(index >= start && rows < AZ_LIST_ROWS) out_rows[rows++] = category;
        index++;
        if(rows == AZ_LIST_ROWS) break;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(out_row_count) *out_row_count = rows;
    if(out_window_start) *out_window_start = start;
    if(out_total) *out_total = total;
    return true;
}

/** @brief Read one optional-index category by sorted ordinal. */
static bool az_index_get_category(Storage* storage, uint16_t category_index, AzCategory* out) {
    File* file = NULL;
    AzLineReader reader;
    if(!out || !az_open_index(storage, &file, &reader)) return false;
    uint16_t index = 0;
    bool found = false;
    char line[AZ_NAME_MAX + 80];
    while(az_read_line(&reader, line, sizeof(line))) {
        AzCategory category;
        if(!az_parse_category_line(line, &category)) continue;
        if(index++ == category_index) {
            *out = category;
            found = true;
            break;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return found;
}

/** @brief Read an optional-index figure/search window. */
static bool az_index_get_figure_window(
    Storage* storage,
    bool search,
    uint8_t category,
    const char* query,
    uint16_t selection,
    AzFigure out_rows[AZ_LIST_ROWS],
    uint8_t* out_row_count,
    uint16_t* out_window_start,
    uint16_t* out_total) {
    File* file = NULL;
    AzLineReader reader;
    if(!az_open_index(storage, &file, &reader)) return false;
    uint16_t total = 0;
    char line[AZ_NAME_MAX + 80];
    while(az_read_line(&reader, line, sizeof(line))) {
        AzFigure figure;
        if(!az_parse_figure_line(line, &figure)) continue;
        bool matches = search ?
                           (az_contains_nocase(figure.name, query) || az_contains_nocase(figure.id_hex, query)) :
                           figure.category == category;
        if(matches) total++;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(total && selection >= total) selection = (uint16_t)(total - 1);
    uint16_t start = az_window_start(selection, total);
    uint8_t rows = 0;
    if(!az_open_index(storage, &file, &reader)) return false;
    uint16_t index = 0;
    while(az_read_line(&reader, line, sizeof(line))) {
        AzFigure figure;
        if(!az_parse_figure_line(line, &figure)) continue;
        bool matches = search ?
                           (az_contains_nocase(figure.name, query) || az_contains_nocase(figure.id_hex, query)) :
                           figure.category == category;
        if(!matches) continue;
        if(index >= start && rows < AZ_LIST_ROWS) out_rows[rows++] = figure;
        index++;
        if(rows == AZ_LIST_ROWS) break;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(out_row_count) *out_row_count = rows;
    if(out_window_start) *out_window_start = start;
    if(out_total) *out_total = total;
    return true;
}

/** @brief Read one optional-index figure/search record by filtered ordinal. */
static bool az_index_get_figure(
    Storage* storage,
    bool search,
    uint8_t category,
    const char* query,
    uint16_t wanted,
    AzFigure* out) {
    File* file = NULL;
    AzLineReader reader;
    if(!out || !az_open_index(storage, &file, &reader)) return false;
    uint16_t index = 0;
    bool found = false;
    char line[AZ_NAME_MAX + 80];
    while(az_read_line(&reader, line, sizeof(line))) {
        AzFigure figure;
        if(!az_parse_figure_line(line, &figure)) continue;
        bool matches = search ?
                           (az_contains_nocase(figure.name, query) || az_contains_nocase(figure.id_hex, query)) :
                           figure.category == category;
        if(!matches) continue;
        if(index++ == wanted) {
            *out = figure;
            found = true;
            break;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return found;
}

/** @brief Find one optional-index figure by exact 8-byte ID. */
static bool az_index_find_by_id(Storage* storage, const uint8_t id[8], AzFigure* out) {
    File* file = NULL;
    AzLineReader reader;
    if(!out || !az_open_index(storage, &file, &reader)) return false;
    bool found = false;
    char line[AZ_NAME_MAX + 80];
    while(az_read_line(&reader, line, sizeof(line))) {
        AzFigure figure;
        if(az_parse_figure_line(line, &figure) && memcmp(figure.id, id, 8) == 0) {
            *out = figure;
            found = true;
            break;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return found;
}

#endif /* AZ_FEATURE_JSON_INDEX */

#if !AZ_FEATURE_JSON_INDEX

/** @brief Find-by-ID visitor state. */
typedef struct {
    const uint8_t* id; /**< Target 8-byte ID. */
    AzFigure* out;     /**< Destination figure. */
    bool found;        /**< True after exact match. */
} AzFindContext;

/** @brief Visitor that copies and stops at one exact figure ID. */
static bool az_find_visitor(const AzFigure* figure, void* raw_context) {
    AzFindContext* context = raw_context;
    if(memcmp(figure->id, context->id, 8) != 0) return true;
    *context->out = *figure;
    context->found = true;
    return false;
}

#endif /* !AZ_FEATURE_JSON_INDEX */

/**
 * @brief Prepare database access; build the optional index only when its feature flag is enabled.
 */
bool az_db_ensure_index(Storage* storage, bool force, uint32_t* out_count) {
    if(out_count) *out_count = 0;
    if(!storage || !storage_file_exists(storage, AZ_AMIIBO_JSON)) return false;
#if AZ_FEATURE_JSON_INDEX
    if(!force && storage_file_exists(storage, AZ_INDEX_FILE)) {
        uint32_t count = 0;
        if(az_index_count_figures(storage, &count)) {
            if(out_count) *out_count = count;
            return true;
        }
    }
    return az_index_build(storage, out_count);
#else
    if(!az_categories_ensure(storage, force)) return false;
    uint32_t count = 0;
    for(uint16_t i = 0; i < az_category_cache_count; i++) count += az_category_cache[i].count;
    if(out_count) *out_count = count;
    return true;
#endif
}

/**
 * @brief Return the visible alphabetical category window and total category count.
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
#if AZ_FEATURE_JSON_INDEX
    return az_index_get_category_window(
        storage,
        selection,
        out_rows,
        out_row_count,
        out_window_start,
        out_total);
#else
    if(!az_categories_ensure(storage, false)) return false;
    uint16_t total = az_category_cache_count;
    if(total && selection >= total) selection = (uint16_t)(total - 1);
    uint16_t start = az_window_start(selection, total);
    uint8_t rows = 0;
    while(rows < AZ_LIST_ROWS && start + rows < total) {
        out_rows[rows] = az_category_cache[start + rows];
        rows++;
    }
    if(out_row_count) *out_row_count = rows;
    if(out_window_start) *out_window_start = start;
    if(out_total) *out_total = total;
    return true;
#endif
}

/**
 * @brief Return one alphabetical category by absolute ordinal.
 */
bool az_db_get_category(Storage* storage, uint16_t category_index, AzCategory* out) {
    if(!out) return false;
#if AZ_FEATURE_JSON_INDEX
    return az_index_get_category(storage, category_index, out);
#else
    if(!az_categories_ensure(storage, false) || category_index >= az_category_cache_count) return false;
    *out = az_category_cache[category_index];
    return true;
#endif
}

/**
 * @brief Return a bounded alphabetical figure window for one category.
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
#if AZ_FEATURE_JSON_INDEX
    return az_index_get_figure_window(
        storage,
        false,
        category,
        "",
        selection,
        out_rows,
        out_row_count,
        out_window_start,
        out_total);
#else
    if(!az_window_seek(storage, false, category, "", selection)) return false;
    az_window_copy_outputs(out_rows, out_row_count, out_window_start, out_total);
    return true;
#endif
}

/**
 * @brief Return one figure by alphabetical category ordinal.
 */
bool az_db_get_figure(Storage* storage, uint8_t category, uint16_t figure_index, AzFigure* out) {
    if(!out) return false;
#if AZ_FEATURE_JSON_INDEX
    return az_index_get_figure(storage, false, category, "", figure_index, out);
#else
    if(!az_window_seek(storage, false, category, "", figure_index)) return false;
    const AzFigureWindowCache* cache = &az_figure_window_cache;
    if(figure_index < cache->start || figure_index >= cache->start + cache->row_count) return false;
    *out = cache->rows[figure_index - cache->start];
    return true;
#endif
}

/**
 * @brief Return a bounded alphabetical search-result window.
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
#if AZ_FEATURE_JSON_INDEX
    return az_index_get_figure_window(
        storage,
        true,
        0,
        query,
        selection,
        out_rows,
        out_row_count,
        out_window_start,
        out_total);
#else
    if(!az_window_seek(storage, true, 0, query ? query : "", selection)) return false;
    az_window_copy_outputs(out_rows, out_row_count, out_window_start, out_total);
    return true;
#endif
}

/**
 * @brief Return one search result by absolute filtered ordinal.
 */
bool az_db_search_get(Storage* storage, const char* query, uint16_t match_index, AzFigure* out) {
    if(!out) return false;
#if AZ_FEATURE_JSON_INDEX
    return az_index_get_figure(storage, true, 0, query, match_index, out);
#else
    if(!az_window_seek(storage, true, 0, query ? query : "", match_index)) return false;
    const AzFigureWindowCache* cache = &az_figure_window_cache;
    if(match_index < cache->start || match_index >= cache->start + cache->row_count) return false;
    *out = cache->rows[match_index - cache->start];
    return true;
#endif
}

/**
 * @brief Resolve one exact Amiibo ID without materializing the JSON database.
 */
bool az_db_find_by_id(Storage* storage, const uint8_t id[8], AzFigure* out) {
    if(!storage || !id || !out) return false;
#if AZ_FEATURE_JSON_INDEX
    return az_index_find_by_id(storage, id, out);
#else
    AzFindContext context = {.id = id, .out = out};
    bool ok = az_scan_figures(storage, az_find_visitor, &context);
    return (ok || context.found) && context.found;
#endif
}

/** @brief Return a short UI label for one games_info platform-array key. */
static const char* az_platform_label(const char* key) {
    if(!key) return NULL;
    if(strcmp(key, "games3DS") == 0) return "3DS";
    if(strcmp(key, "gamesWiiU") == 0) return "Wii U";
    if(strcmp(key, "gamesSwitch") == 0) return "Switch";
    if(strcmp(key, "gamesSwitch2") == 0) return "Switch 2";
    return NULL;
}

/** @brief Return true when an AmiiboAPI wildcard ID pattern matches a concrete figure ID. */
static bool az_pattern_matches(const uint8_t pattern[8], const uint8_t id[8]) {
    for(size_t i = 0; i < 8; i++) {
        if(pattern[i] != 0 && pattern[i] != id[i]) return false;
    }
    return true;
}

/** @brief Return true when a compatibility result already contains the same logical usage. */
static bool az_game_duplicate(
    const AzGame* games,
    uint16_t count,
    const char* platform,
    const char* name,
    const char* usage) {
    for(uint16_t i = 0; i < count; i++) {
        if(strcmp(games[i].platform, platform) == 0 && strcmp(games[i].name, name) == 0 &&
           strcmp(games[i].usage, usage) == 0) {
            return true;
        }
    }
    return false;
}

/** @brief Append one deduplicated compatibility entry to the caller's bounded array. */
static void az_game_add(
    AzGamesParserContext* context,
    const char* usage_text,
    bool writes) {
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

/** @brief Flush usages buffered only because amiiboUsage appeared before gameName. */
static void az_game_flush_pending(AzGamesParserContext* context) {
    if(!context || !context->game_name[0]) return;
    for(uint8_t i = 0; i < context->pending_count; i++) {
        az_game_add(context, context->pending[i].text, context->pending[i].writes);
    }
    context->pending_count = 0;
}

/** @brief Handle lwJSON events while extracting only matching compatibility records. */
static void az_games_json_event(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    AzGamesParserContext* context = lwjson_stream_get_user_data(parser);
    if(!context) return;

    if(type == LWJSON_STREAM_TYPE_OBJECT) {
        const char* nearest = az_lw_key(parser, 0);
        const char* previous = az_lw_key(parser, 1);
        uint8_t pattern[8];
        if(nearest && previous && strcmp(previous, "amiibos") == 0 &&
           az_parse_id_text(nearest, pattern, NULL)) {
            context->matching_entry = az_pattern_matches(pattern, context->figure_id);
            return;
        }
        if(!context->matching_entry) return;
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
           parser->stack_pos > 0 &&
           parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
            context->in_usage = true;
            context->saw_usage = true;
            memset(&context->usage, 0, sizeof(context->usage));
        }
        return;
    }

    if(!context->matching_entry) return;

    if(type == LWJSON_STREAM_TYPE_STRING) {
        if(context->in_game && az_lw_key_is(parser, 0, "gameName")) {
            az_copy_stream_string(
                parser,
                context->game_name,
                sizeof(context->game_name),
                &context->decoder);
            az_clean_field(context->game_name);
            if(parser->data.str.is_last) az_game_flush_pending(context);
        } else if(context->in_usage && az_lw_key_is(parser, 0, "Usage")) {
            az_copy_stream_string(
                parser,
                context->usage.text,
                sizeof(context->usage.text),
                &context->decoder);
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
        const char* previous = az_lw_key(parser, 1);
        if(context->in_usage && nearest && strcmp(nearest, "amiiboUsage") == 0 &&
           parser->stack_pos > 0 &&
           parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
            context->in_usage = false;
            if(context->game_name[0]) {
                az_game_add(context, context->usage.text, context->usage.writes);
            } else if(context->pending_count < COUNT_OF(context->pending)) {
                context->pending[context->pending_count++] = context->usage;
            }
            return;
        }

        const char* platform = az_platform_label(nearest);
        if(context->in_game && platform && parser->stack_pos > 0 &&
           parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
            az_game_flush_pending(context);
            if(!context->saw_usage) az_game_add(context, "Compatible", false);
            context->in_game = false;
            context->platform[0] = '\0';
            return;
        }

        uint8_t pattern[8];
        if(nearest && previous && strcmp(previous, "amiibos") == 0 &&
           az_parse_id_text(nearest, pattern, NULL)) {
            context->matching_entry = false;
            context->in_game = false;
            context->in_usage = false;
        }
    }
}

/**
 * @brief Stream games_info.json and collect bounded compatibility results for one Amiibo.
 */
bool az_db_load_games(
    Storage* storage,
    const uint8_t id[8],
    AzGame* out_games,
    uint16_t max_games,
    uint16_t* out_count) {
    if(out_count) *out_count = 0;
    if(!storage || !id || !out_games || !out_count || max_games == 0) return false;
    if(!storage_file_exists(storage, AZ_GAMES_JSON)) return false;

    AzGamesParserContext* context = calloc(1, sizeof(*context));
    if(!context) return false;
    context->figure_id = id;
    context->games = out_games;
    context->max_games = max_games;
    bool ok = az_scan_json_file(
        storage,
        AZ_GAMES_JSON,
        az_games_json_event,
        context,
        &context->control);
    *out_count = context->count;
    free(context);
    return ok;
}
