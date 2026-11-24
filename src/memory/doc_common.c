#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Shared helpers for the binary document extractors: bounded UTF-8 text assembly and
   bounded zlib inflation (decompression-bomb defence). */

int asgend_memory_text_init(AsgendMemoryTextBuffer *buffer, size_t limit) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->limit = limit;
    buffer->capacity = limit < 4096U ? limit + 1U : 4096U;
    buffer->data = malloc(buffer->capacity);
    if (!buffer->data) { buffer->oom = true; return -1; }
    buffer->data[0] = '\0';
    return 0;
}

void asgend_memory_text_free(AsgendMemoryTextBuffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static bool text_reserve(AsgendMemoryTextBuffer *buffer, size_t extra) {
    if (buffer->oom || !buffer->data) return false;
    if (extra > buffer->limit - buffer->length) { buffer->truncated = true; return false; }
    size_t needed = buffer->length + extra + 1U;
    if (needed <= buffer->capacity) return true;
    size_t capacity = buffer->capacity;
    while (capacity < needed) {
        if (capacity > (buffer->limit + 1U) / 2U) { capacity = buffer->limit + 1U; break; }
        capacity *= 2U;
    }
    if (capacity < needed) capacity = needed;
    char *grown = realloc(buffer->data, capacity);
    if (!grown) { buffer->oom = true; return false; }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static void text_raw(AsgendMemoryTextBuffer *buffer, const char *bytes, size_t length) {
    if (buffer->truncated || !text_reserve(buffer, length)) return;
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static char text_last(const AsgendMemoryTextBuffer *buffer) {
    return buffer->length ? buffer->data[buffer->length - 1U] : '\0';
}

static void text_flush_pending(AsgendMemoryTextBuffer *buffer) {
    unsigned int pending = buffer->pending;
    buffer->pending = 0;
    if (!pending || buffer->length == 0) return;
    char last = text_last(buffer);
    if (pending >= 3U) {
        if (last != '\n') text_raw(buffer, "\n", 1U);
        if (buffer->length < 2U || buffer->data[buffer->length - 2U] != '\n') text_raw(buffer, "\n", 1U);
    } else if (pending == 2U) {
        if (last != '\n') text_raw(buffer, "\n", 1U);
    } else if (last != ' ' && last != '\n' && last != '\t') {
        text_raw(buffer, " ", 1U);
    }
}

void asgend_memory_text_separator(AsgendMemoryTextBuffer *buffer, unsigned int kind) {
    if (kind > buffer->pending) buffer->pending = kind;
}

void asgend_memory_text_codepoint(AsgendMemoryTextBuffer *buffer, uint32_t codepoint) {
    if (buffer->truncated || buffer->oom) return;
    if (codepoint == '\r') codepoint = '\n';
    if (codepoint == 0xa0U) codepoint = ' ';
    if (codepoint == '\n') { asgend_memory_text_separator(buffer, 2U); return; }
    if (codepoint == ' ' && buffer->pending == 0U) {
        char last = text_last(buffer);
        if (buffer->length == 0 || last == ' ' || last == '\n' || last == '\t') return;
        text_raw(buffer, " ", 1U);
        return;
    }
    if (codepoint == ' ') return; /* A pending separator already covers whitespace. */
    if (codepoint != '\t' && (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU))) return;
    if ((codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU ||
        codepoint == 0xfffeU || codepoint == 0xffffU || codepoint == 0xfeffU) return;
    text_flush_pending(buffer);
    char encoded[4];
    size_t length;
    if (codepoint < 0x80U) { encoded[0] = (char)codepoint; length = 1U; }
    else if (codepoint < 0x800U) {
        encoded[0] = (char)(0xc0U | (codepoint >> 6U));
        encoded[1] = (char)(0x80U | (codepoint & 0x3fU));
        length = 2U;
    } else if (codepoint < 0x10000U) {
        encoded[0] = (char)(0xe0U | (codepoint >> 12U));
        encoded[1] = (char)(0x80U | ((codepoint >> 6U) & 0x3fU));
        encoded[2] = (char)(0x80U | (codepoint & 0x3fU));
        length = 3U;
    } else {
        encoded[0] = (char)(0xf0U | (codepoint >> 18U));
        encoded[1] = (char)(0x80U | ((codepoint >> 12U) & 0x3fU));
        encoded[2] = (char)(0x80U | ((codepoint >> 6U) & 0x3fU));
        encoded[3] = (char)(0x80U | (codepoint & 0x3fU));
        length = 4U;
    }
    text_raw(buffer, encoded, length);
}

void asgend_memory_text_utf8(AsgendMemoryTextBuffer *buffer, const char *bytes, size_t length) {
    const unsigned char *in = (const unsigned char *)bytes;
    size_t i = 0;
    while (i < length && !buffer->truncated && !buffer->oom) {
        unsigned char first = in[i];
        uint32_t codepoint, minimum;
        size_t need;
        if (first < 0x80U) { asgend_memory_text_codepoint(buffer, first); ++i; continue; }
        if (first >= 0xc2U && first <= 0xdfU) { need = 2U; codepoint = first & 0x1fU; minimum = 0x80U; }
        else if (first >= 0xe0U && first <= 0xefU) { need = 3U; codepoint = first & 0x0fU; minimum = 0x800U; }
        else if (first >= 0xf0U && first <= 0xf4U) { need = 4U; codepoint = first & 0x07U; minimum = 0x10000U; }
        else { ++i; continue; }
        if (need > length - i) break;
        bool valid = true;
        for (size_t k = 1; k < need; ++k) {
            if ((in[i + k] & 0xc0U) != 0x80U) { valid = false; break; }
            codepoint = (codepoint << 6U) | (uint32_t)(in[i + k] & 0x3fU);
        }
        if (!valid || codepoint < minimum) { ++i; continue; }
        asgend_memory_text_codepoint(buffer, codepoint);
        i += need;
    }
}

int asgend_memory_text_finish(AsgendMemoryTextBuffer *buffer, AsgendMemoryDocText *out) {
    if (buffer->oom || !buffer->data) { asgend_memory_text_free(buffer); return -1; }
    /* Trailing whitespace carries no information. */
    while (buffer->length && (buffer->data[buffer->length - 1U] == ' ' || buffer->data[buffer->length - 1U] == '\n' ||
                              buffer->data[buffer->length - 1U] == '\t')) buffer->data[--buffer->length] = '\0';
    out->text = buffer->data;
    out->text_len = buffer->length;
    if (buffer->truncated) out->truncated = true;
    buffer->data = NULL;
    buffer->length = buffer->capacity = 0;
    return 0;
}

void asgend_memory_doc_reason(AsgendMemoryDocText *out, const char *format, ...) {
    if (!out || out->reason[0]) return; /* Keep the first reason; it is usually the root cause. */
    va_list args;
    va_start(args, format);
    (void)vsnprintf(out->reason, sizeof(out->reason), format, args);
    va_end(args);
}

int asgend_memory_inflate_bounded(const unsigned char *input, size_t input_len, bool raw,
                                  size_t max_output, unsigned char **output, size_t *output_len,
                                  bool *limit_hit, bool *corrupt) {
    *output = NULL; *output_len = 0;
    bool over_limit = false, bad = false;
    if (limit_hit) *limit_hit = false;
    if (corrupt) *corrupt = false;
    if (input_len > (size_t)UINT_MAX) { if (corrupt) *corrupt = true; return -1; }
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, raw ? -MAX_WBITS : MAX_WBITS + 32) != Z_OK) return -2;
    size_t capacity = input_len < 16384U ? 65536U : (input_len > max_output / 4U ? max_output : input_len * 4U);
    if (capacity > max_output) capacity = max_output;
    if (capacity == 0) capacity = 1U;
    unsigned char *buffer = malloc(capacity + 1U);
    if (!buffer) { inflateEnd(&stream); return -2; }
    stream.next_in = (Bytef *)(uintptr_t)input;
    stream.avail_in = (uInt)input_len;
    size_t produced = 0;
    int rc = Z_OK;
    for (;;) {
        if (produced == capacity) {
            if (capacity >= max_output) {
                /* Probe for one more byte to tell "exactly at the limit" from "over the limit". */
                unsigned char probe;
                stream.next_out = &probe; stream.avail_out = 1U;
                rc = inflate(&stream, Z_NO_FLUSH);
                if (stream.avail_out == 0U) over_limit = true;
                break;
            }
            size_t next = capacity > max_output / 2U ? max_output : capacity * 2U;
            unsigned char *grown = realloc(buffer, next + 1U);
            if (!grown) { free(buffer); inflateEnd(&stream); return -2; }
            buffer = grown; capacity = next;
        }
        size_t room = capacity - produced;
        if (room > (size_t)UINT_MAX) room = (size_t)UINT_MAX;
        stream.next_out = buffer + produced;
        stream.avail_out = (uInt)room;
        rc = inflate(&stream, Z_NO_FLUSH);
        produced = (size_t)(stream.next_out - buffer);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) {
            if (rc == Z_BUF_ERROR && produced == capacity && stream.avail_in != 0U) continue; /* grow */
            bad = true;
            break;
        }
        /* Input exhausted before the end-of-stream marker: truncated stream, keep the prefix. */
        if (stream.avail_in == 0U && stream.avail_out != 0U) { bad = true; break; }
    }
    inflateEnd(&stream);
    if (rc == Z_MEM_ERROR) { free(buffer); return -2; }
    if (limit_hit) *limit_hit = over_limit;
    if (corrupt) *corrupt = bad;
    if (produced == 0 && bad) { free(buffer); return -1; }
    buffer[produced] = 0U;
    *output = buffer;
    *output_len = produced;
    return 0;
}
