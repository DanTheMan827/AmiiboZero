/**
 * @file lwjson_stream.c
 * @brief lwJSON fixed-memory streaming parser.
 *
 * Minimal integration of the lwJSON v1.9.0 stream parser used by Amiibo Zero.
 * Upstream: MaJerle/lwjson, commit be2b042fae1401957dcc01860532e15b40d3eb66.
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

#include "lwjson.h"

#include <string.h>

/** Number of elements in one compile-time array. */
#define LWJSON_ARRAYSIZE(x) (sizeof(x) / sizeof((x)[0]))

/** Return true when a byte is JSON-compatible whitespace accepted by lwJSON. */
static uint8_t lwjson_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

/** Push a container/key type onto the fixed parser stack. */
static uint8_t lwjson_stack_push(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    if(parser->stack_pos >= LWJSON_ARRAYSIZE(parser->stack)) return 0;
    parser->stack[parser->stack_pos].type = type;
    parser->stack[parser->stack_pos].meta.index = 0;
    parser->stack_pos++;
    return 1;
}

/** Pop and return the top stream-stack type. */
static lwjson_stream_type_t lwjson_stack_pop(lwjson_stream_parser_t* parser) {
    if(parser->stack_pos == 0) return LWJSON_STREAM_TYPE_NONE;
    lwjson_stream_type_t type = parser->stack[--parser->stack_pos].type;
    parser->stack[parser->stack_pos].type = LWJSON_STREAM_TYPE_NONE;
    if(parser->stack_pos > 0 &&
       parser->stack[parser->stack_pos - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
        parser->stack[parser->stack_pos - 1].meta.index++;
    }
    return type;
}

/** Return the current top stream-stack type without changing the stack. */
static lwjson_stream_type_t lwjson_stack_top(const lwjson_stream_parser_t* parser) {
    return parser->stack_pos ? parser->stack[parser->stack_pos - 1].type : LWJSON_STREAM_TYPE_NONE;
}

/** Emit a parser event when an application callback is installed. */
static void lwjson_send_event(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
    if(parser && parser->evt_fn) parser->evt_fn(parser, type);
}

/**
 * @brief Initialize a stream parser and event callback.
 * @param parser Parser instance to initialize.
 * @param event_callback Callback invoked for stream events.
 * @return lwjsonOK on success or an lwjsonr_t error.
 */
lwjsonr_t lwjson_stream_init(
    lwjson_stream_parser_t* parser,
    lwjson_stream_parser_callback_fn event_callback) {
    if(!parser) return lwjsonERRNULL;
    memset(parser, 0, sizeof(*parser));
    parser->parse_state = LWJSON_STREAM_STATE_WAITINGFIRSTCHAR;
    parser->evt_fn = event_callback;
    return lwjsonOK;
}

/**
 * @brief Associate an opaque caller pointer with the parser.
 * @param parser Parser instance.
 * @param user_data Pointer retained without copying.
 * @return lwjsonOK on success or lwjsonERRNULL.
 */
lwjsonr_t lwjson_stream_set_user_data(lwjson_stream_parser_t* parser, void* user_data) {
    if(!parser) return lwjsonERRNULL;
    parser->user_data = user_data;
    return lwjsonOK;
}

/**
 * @brief Retrieve the opaque caller pointer associated with the parser.
 * @param parser Parser instance.
 * @return Caller pointer or NULL.
 */
void* lwjson_stream_get_user_data(lwjson_stream_parser_t* parser) {
    return parser ? parser->user_data : NULL;
}

/**
 * @brief Reset the parser state machine for a new JSON value.
 * @param parser Parser instance.
 * @return lwjsonOK on success or lwjsonERRNULL.
 */
lwjsonr_t lwjson_stream_reset(lwjson_stream_parser_t* parser) {
    if(!parser) return lwjsonERRNULL;
    parser->parse_state = LWJSON_STREAM_STATE_WAITINGFIRSTCHAR;
    parser->stack_pos = 0;
    parser->is_escaped = 0;
    return lwjsonOK;
}

/**
 * @brief Feed one JSON byte into the fixed-memory stream parser.
 * @param parser Parser instance.
 * @param c Next byte from the JSON stream.
 * @return Stream progress/completion status or an lwjsonr_t error.
 */
lwjsonr_t lwjson_stream_parse(lwjson_stream_parser_t* parser, char c) {
    if(!parser) return lwjsonERRNULL;
    if(parser->parse_state == LWJSON_STREAM_STATE_WAITINGFIRSTCHAR && c != '{' && c != '[') {
        return lwjson_is_space(c) ? lwjsonSTREAMWAITFIRSTCHAR : lwjsonERRJSON;
    }

start_over:
    switch(parser->parse_state) {
    case LWJSON_STREAM_STATE_WAITINGFIRSTCHAR:
    case LWJSON_STREAM_STATE_EXPECTING_COMMA_OR_END:
    case LWJSON_STREAM_STATE_PARSING: {
        if(lwjson_is_space(c)) {
            break;
        } else if(c == ',') {
            if(parser->parse_state != LWJSON_STREAM_STATE_EXPECTING_COMMA_OR_END) {
                return lwjsonERRJSON;
            }
            parser->parse_state = LWJSON_STREAM_STATE_PARSING;
        } else if(c == '}' || c == ']') {
            lwjson_stream_type_t top = lwjson_stack_top(parser);
            if(top == LWJSON_STREAM_TYPE_KEY) return lwjsonERRJSON;
            if((c == '}' && top != LWJSON_STREAM_TYPE_OBJECT) ||
               (c == ']' && top != LWJSON_STREAM_TYPE_ARRAY)) {
                return lwjsonERRJSON;
            }
            if(lwjson_stack_pop(parser) == LWJSON_STREAM_TYPE_NONE) return lwjsonERRJSON;
            lwjson_send_event(
                parser,
                c == '}' ? LWJSON_STREAM_TYPE_OBJECT_END : LWJSON_STREAM_TYPE_ARRAY_END);
            if(lwjson_stack_top(parser) == LWJSON_STREAM_TYPE_KEY) lwjson_stack_pop(parser);
            if(parser->stack_pos == 0) {
                lwjson_stream_reset(parser);
                return lwjsonSTREAMDONE;
            }
            parser->parse_state = LWJSON_STREAM_STATE_EXPECTING_COMMA_OR_END;
        } else if(parser->parse_state == LWJSON_STREAM_STATE_EXPECTING_COMMA_OR_END) {
            return lwjsonERRJSON;
        } else if(c == '"') {
            parser->parse_state = LWJSON_STREAM_STATE_PARSING_STRING;
            parser->is_escaped = 0;
            memset(&parser->data.str, 0, sizeof(parser->data.str));
        } else if(lwjson_stack_top(parser) == LWJSON_STREAM_TYPE_OBJECT) {
            return lwjsonERRJSON;
        } else if(c == '{' || c == '[') {
            if(parser->parse_state == LWJSON_STREAM_STATE_WAITINGFIRSTCHAR) parser->stack_pos = 0;
            lwjson_stream_type_t type =
                c == '{' ? LWJSON_STREAM_TYPE_OBJECT : LWJSON_STREAM_TYPE_ARRAY;
            lwjson_send_event(parser, type);
            if(!lwjson_stack_push(parser, type)) return lwjsonERRMEM;
            parser->parse_state = LWJSON_STREAM_STATE_PARSING;
        } else if(c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
            if(lwjson_stack_top(parser) == LWJSON_STREAM_TYPE_OBJECT) return lwjsonERRJSON;
            parser->parse_state = LWJSON_STREAM_STATE_PARSING_PRIMITIVE;
            memset(&parser->data.prim, 0, sizeof(parser->data.prim));
            parser->data.prim.buff[parser->data.prim.buff_pos++] = c;
        } else {
            return lwjsonERRJSON;
        }
        break;
    }

    case LWJSON_STREAM_STATE_EXPECTING_COLON:
        if(lwjson_is_space(c)) {
            break;
        }
        if(c != ':') return lwjsonERRJSON;
        parser->parse_state = LWJSON_STREAM_STATE_PARSING;
        break;

    case LWJSON_STREAM_STATE_PARSING_STRING: {
        lwjson_stream_type_t top = lwjson_stack_top(parser);
        if(parser->is_escaped) {
            parser->is_escaped = 0;
        } else if(c == '"') {
            parser->data.str.is_last = 1;
            parser->parse_state = LWJSON_STREAM_STATE_EXPECTING_COMMA_OR_END;
            if(top == LWJSON_STREAM_TYPE_OBJECT) {
                lwjson_send_event(parser, LWJSON_STREAM_TYPE_KEY);
                if(!lwjson_stack_push(parser, LWJSON_STREAM_TYPE_KEY)) return lwjsonERRMEM;
                size_t len = parser->data.str.buff_pos;
                if(len >= sizeof(parser->stack[0].meta.name)) {
                    len = sizeof(parser->stack[0].meta.name) - 1;
                }
                memcpy(
                    parser->stack[parser->stack_pos - 1].meta.name,
                    parser->data.str.buff,
                    len);
                parser->stack[parser->stack_pos - 1].meta.name[len] = '\0';
                parser->parse_state = LWJSON_STREAM_STATE_EXPECTING_COLON;
            } else if(top == LWJSON_STREAM_TYPE_KEY) {
                lwjson_send_event(parser, LWJSON_STREAM_TYPE_STRING);
                lwjson_stack_pop(parser);
            } else if(top == LWJSON_STREAM_TYPE_ARRAY) {
                lwjson_send_event(parser, LWJSON_STREAM_TYPE_STRING);
                parser->stack[parser->stack_pos - 1].meta.index++;
            } else {
                return lwjsonERRJSON;
            }
            break;
        } else if(c == '\\') {
            parser->is_escaped = 1;
        }

        parser->data.str.buff[parser->data.str.buff_pos++] = c;
        parser->data.str.buff_total_pos++;
        if(parser->data.str.buff_pos >= sizeof(parser->data.str.buff) - 1) {
            parser->data.str.buff[parser->data.str.buff_pos] = '\0';
            lwjson_send_event(
                parser,
                (top == LWJSON_STREAM_TYPE_KEY || top == LWJSON_STREAM_TYPE_ARRAY) ?
                    LWJSON_STREAM_TYPE_STRING :
                    LWJSON_STREAM_TYPE_KEY);
            parser->data.str.buff_pos = 0;
        }
        break;
    }

    case LWJSON_STREAM_STATE_PARSING_PRIMITIVE: {
        if(!lwjson_is_space(c) && c != ',' && c != ']' && c != '}') {
            if(parser->data.prim.buff_pos >= sizeof(parser->data.prim.buff) - 1) {
                return lwjsonERRJSON;
            }
            parser->data.prim.buff[parser->data.prim.buff_pos++] = c;
            break;
        }

        lwjson_stream_type_t top = lwjson_stack_top(parser);
        if(parser->data.prim.buff_pos == 4 &&
           memcmp(parser->data.prim.buff, "true", 4) == 0) {
            lwjson_send_event(parser, LWJSON_STREAM_TYPE_TRUE);
        } else if(parser->data.prim.buff_pos == 4 &&
                  memcmp(parser->data.prim.buff, "null", 4) == 0) {
            lwjson_send_event(parser, LWJSON_STREAM_TYPE_NULL);
        } else if(parser->data.prim.buff_pos == 5 &&
                  memcmp(parser->data.prim.buff, "false", 5) == 0) {
            lwjson_send_event(parser, LWJSON_STREAM_TYPE_FALSE);
        } else if(parser->data.prim.buff_pos > 0 &&
                  (parser->data.prim.buff[0] == '-' ||
                   (parser->data.prim.buff[0] >= '0' && parser->data.prim.buff[0] <= '9'))) {
            lwjson_send_event(parser, LWJSON_STREAM_TYPE_NUMBER);
        } else {
            return lwjsonERRJSON;
        }

        if(top == LWJSON_STREAM_TYPE_KEY) {
            lwjson_stack_pop(parser);
        } else if(top == LWJSON_STREAM_TYPE_ARRAY) {
            parser->stack[parser->stack_pos - 1].meta.index++;
        }
        parser->parse_state = LWJSON_STREAM_STATE_EXPECTING_COMMA_OR_END;
        goto start_over;
    }

    default:
        return lwjsonERRJSON;
    }
    return lwjsonSTREAMINPROG;
}
