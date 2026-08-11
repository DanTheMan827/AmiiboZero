/**
 * @file lwjson.h
 * @brief Minimal lwJSON streaming-parser interface vendored for Amiibo Zero.
 *
 * This file contains only the stream API and types used by Amiibo Zero. It is
 * derived from MaJerle/lwjson v1.9.0 (commit
 * be2b042fae1401957dcc01860532e15b40d3eb66), MIT licensed.
 */

/*
 * Copyright (c) 2024 Tilen MAJERLE
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum retained object-key length in bytes, excluding NUL. */
#ifndef LWJSON_CFG_STREAM_KEY_MAX_LEN
#define LWJSON_CFG_STREAM_KEY_MAX_LEN 32
#endif

/** Maximum JSON nesting stack depth retained by the stream parser. */
#ifndef LWJSON_CFG_STREAM_STACK_SIZE
#define LWJSON_CFG_STREAM_STACK_SIZE 16
#endif

/** Maximum raw string chunk retained at once, excluding NUL. */
#ifndef LWJSON_CFG_STREAM_STRING_MAX_LEN
#define LWJSON_CFG_STREAM_STRING_MAX_LEN 256
#endif

/** Maximum primitive token length retained at once, excluding NUL. */
#ifndef LWJSON_CFG_STREAM_PRIMITIVE_MAX_LEN
#define LWJSON_CFG_STREAM_PRIMITIVE_MAX_LEN 32
#endif

/** Return and parser-state codes used by lwJSON. */
typedef enum {
    lwjsonOK = 0x00,              /**< Operation completed successfully. */
    lwjsonERR,                    /**< Generic parser error. */
    lwjsonERRJSON,                /**< Input is not valid JSON for the stream state. */
    lwjsonERRMEM,                 /**< Fixed parser stack has insufficient capacity. */
    lwjsonERRPAR,                 /**< Invalid parameter. */
    lwjsonSTREAMWAITFIRSTCHAR,    /**< Parser is waiting for the first object/array byte. */
    lwjsonSTREAMDONE,             /**< One complete top-level JSON value was parsed. */
    lwjsonSTREAMINPROG,           /**< Parsing is in progress. */
    lwjsonERRNULL,                /**< Required pointer argument was NULL. */
    lwjsonERRINVAL,               /**< Invalid non-NULL input value. */
    lwjsonERRBUF,                 /**< Caller-provided output buffer is too small. */
    lwjsonERRESC,                 /**< Invalid JSON escape sequence. */
} lwjsonr_t;

/** Event types emitted by the streaming parser. */
typedef enum {
    LWJSON_STREAM_TYPE_NONE,       /**< No active stream item. */
    LWJSON_STREAM_TYPE_OBJECT,     /**< Start of an object. */
    LWJSON_STREAM_TYPE_OBJECT_END, /**< End of an object. */
    LWJSON_STREAM_TYPE_ARRAY,      /**< Start of an array. */
    LWJSON_STREAM_TYPE_ARRAY_END,  /**< End of an array. */
    LWJSON_STREAM_TYPE_KEY,        /**< Object key or key chunk. */
    LWJSON_STREAM_TYPE_STRING,     /**< String value or value chunk. */
    LWJSON_STREAM_TYPE_TRUE,       /**< Boolean true primitive. */
    LWJSON_STREAM_TYPE_FALSE,      /**< Boolean false primitive. */
    LWJSON_STREAM_TYPE_NULL,       /**< Null primitive. */
    LWJSON_STREAM_TYPE_NUMBER,     /**< Numeric primitive. */
} lwjson_stream_type_t;

/** One element in lwJSON's fixed-depth stream stack. */
typedef struct {
    lwjson_stream_type_t type; /**< Structural type represented by this stack entry. */
    union {
        char name[LWJSON_CFG_STREAM_KEY_MAX_LEN + 1]; /**< Retained object-key name. */
        uint16_t index; /**< Current array index when this entry represents an array. */
    } meta; /**< Key-name or array-index metadata for this stack entry. */
} lwjson_stream_stack_t;

/** Internal state of the lwJSON stream state machine. */
typedef enum {
    LWJSON_STREAM_STATE_WAITINGFIRSTCHAR = 0x00, /**< Waiting for top-level '{' or '['. */
    LWJSON_STREAM_STATE_PARSING,                 /**< Parsing the next JSON value/member. */
    LWJSON_STREAM_STATE_PARSING_STRING,          /**< Parsing a quoted string. */
    LWJSON_STREAM_STATE_PARSING_PRIMITIVE,       /**< Parsing number/true/false/null. */
    LWJSON_STREAM_STATE_EXPECTING_COMMA_OR_END,  /**< Waiting for separator/container end. */
    LWJSON_STREAM_STATE_EXPECTING_COLON,         /**< Waiting for ':' after an object key. */
} lwjson_stream_state_t;

/** Forward declaration of the fixed-memory stream parser. */
struct lwjson_stream_parser;

/**
 * @brief Callback invoked whenever the stream parser completes a structural/value event.
 * @param parser Parser emitting the event.
 * @param type Event type being emitted.
 */
typedef void (*lwjson_stream_parser_callback_fn)(
    struct lwjson_stream_parser* parser,
    lwjson_stream_type_t type);

/** Fixed-memory lwJSON streaming parser instance. */
typedef struct lwjson_stream_parser {
    lwjson_stream_stack_t stack[LWJSON_CFG_STREAM_STACK_SIZE]; /**< Fixed structural/key stack. */
    size_t stack_pos; /**< Number of active entries in stack. */
    lwjson_stream_state_t parse_state; /**< Current parser state-machine state. */
    lwjson_stream_parser_callback_fn evt_fn; /**< Application event callback. */
    void* user_data; /**< Opaque application context returned to callbacks. */
    union {
        struct {
            char buff[LWJSON_CFG_STREAM_STRING_MAX_LEN + 1]; /**< Current raw string chunk. */
            size_t buff_pos; /**< Bytes currently stored in buff. */
            size_t buff_total_pos; /**< Total raw bytes consumed for this string. */
            uint8_t is_last; /**< Nonzero when this chunk terminates the string. */
        } str; /**< Temporary state for object keys and string values. */
        struct {
            char buff[LWJSON_CFG_STREAM_PRIMITIVE_MAX_LEN + 1]; /**< Primitive bytes. */
            size_t buff_pos; /**< Bytes currently stored in primitive buff. */
        } prim; /**< Temporary state for numbers/booleans/null. */
    } data; /**< Temporary storage shared by string and primitive parser states. */
    uint8_t is_escaped; /**< Nonzero when the previous string byte was a backslash. */
} lwjson_stream_parser_t;

/**
 * @brief Initialize a stream parser and set its event callback.
 * @param parser Parser instance to initialize.
 * @param event_callback Callback invoked for parsed stream events; may be NULL.
 * @return lwjsonOK on success, otherwise an lwjsonr_t error code.
 */
lwjsonr_t lwjson_stream_init(
    lwjson_stream_parser_t* parser,
    lwjson_stream_parser_callback_fn event_callback);

/**
 * @brief Associate application data with a stream parser.
 * @param parser Parser instance.
 * @param user_data Opaque application pointer retained without copying.
 * @return lwjsonOK on success, or lwjsonERRNULL when parser is NULL.
 */
lwjsonr_t lwjson_stream_set_user_data(lwjson_stream_parser_t* parser, void* user_data);

/**
 * @brief Return application data associated with a stream parser.
 * @param parser Parser instance.
 * @return Previously associated pointer, or NULL for a NULL parser.
 */
void* lwjson_stream_get_user_data(lwjson_stream_parser_t* parser);

/**
 * @brief Reset stream state so the parser can consume a new JSON document.
 * @param parser Parser instance to reset.
 * @return lwjsonOK on success, or lwjsonERRNULL when parser is NULL.
 */
lwjsonr_t lwjson_stream_reset(lwjson_stream_parser_t* parser);

/**
 * @brief Feed one byte of JSON to the streaming parser.
 * @param parser Parser instance.
 * @param c Next input byte.
 * @return Stream progress/completion status or an lwjsonr_t error code.
 */
lwjsonr_t lwjson_stream_parse(lwjson_stream_parser_t* parser, char c);

#ifdef __cplusplus
}
#endif
