#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Dependency-free (zlib only) PDF text extractor.
 *
 * The parser never trusts the cross-reference table: it scans the file for "N G obj" headers,
 * skips stream bodies using /Length (falling back to "endstream"), decodes object streams and
 * then walks the page tree from the trailer /Root. Text is taken from Tj/TJ/'/" operators and
 * mapped through ToUnicode CMaps or simple-font encodings (WinAnsi/Standard + /Differences).
 * Every structure is bounded: object count, nesting depth, value nodes, per-stream and total
 * decompressed bytes, page count and Form XObject recursion. Malformed input yields partial or
 * unsupported results, never a crash. Encrypted documents are reported as unsupported.
 */

#define PDF_MAX_OBJECTS 200000U
#define PDF_MAX_STREAM_BYTES (16U * 1024U * 1024U)
#define PDF_MAX_TOTAL_DECODED (128U * 1024U * 1024U)
#define PDF_MAX_DEPTH 32U
#define PDF_MAX_VALUE_NODES 262144U
#define PDF_MAX_PAGES 50000U
#define PDF_MAX_TREE_DEPTH 64U
#define PDF_MAX_FORM_DEPTH 6U
#define PDF_MAX_FONTS 1024U
#define PDF_MAX_CMAP_CHARS 100000U
#define PDF_MAX_CMAP_RANGES 8192U
#define PDF_MAX_CODESPACES 16U
#define PDF_MAX_OPERANDS 64U
#define PDF_MAX_CMAP_STACK 1024U
#define PDF_MAX_OBJSTM_OBJECTS 100000U

enum { PR_VALUE = 0, PR_KEYWORD = 1, PR_EOF = 2, PR_ERROR = -1, PR_OOM = -2 };

typedef enum { PV_NULL = 0, PV_BOOL, PV_NUM, PV_STR, PV_NAME, PV_ARRAY, PV_DICT, PV_REF } PdfType;

typedef struct PdfValue PdfValue;
struct PdfValue {
    PdfType type;
    bool integer;
    bool boolean;
    double num;
    long ref_num;
    unsigned char *bytes;   /* STR/NAME payload, NUL-terminated. */
    size_t len;
    PdfValue *items;        /* ARRAY items; DICT stores key,value,key,value... */
    size_t count;
    size_t capacity;
};

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
    size_t budget;          /* Remaining value nodes before the parser refuses more input. */
    const unsigned char *kw;
    size_t kw_len;
} PdfLexer;

typedef struct {
    long num;
    size_t order;           /* File offset of the header; later definitions win. */
    int container;          /* -1 for the file itself, else index into PdfDoc.bufs. */
    size_t start, end;      /* Value region inside the container. */
    bool has_stream;
    size_t stream_start, stream_len;
} PdfObj;

typedef struct { unsigned char *data; size_t len; } PdfBuf;

typedef struct { uint32_t lo, hi; unsigned char nbytes; } PdfCodespace;
typedef struct { uint32_t code; uint16_t dst[8]; unsigned char dst_len; } PdfCmapChar;
typedef struct { uint32_t lo, hi; uint16_t dst[8]; unsigned char dst_len; } PdfCmapRange;

typedef struct {
    long objnum;
    bool composite;
    bool has_cmap;
    PdfCodespace cs[PDF_MAX_CODESPACES];
    size_t cs_count;
    PdfCmapChar *chars;
    size_t char_count, char_cap;
    PdfCmapRange *ranges;
    size_t range_count, range_cap;
    uint32_t simple_map[256];
} PdfFont;

typedef struct {
    const unsigned char *data;
    size_t len;
    PdfObj *objs;
    size_t obj_count, obj_cap;
    PdfBuf *bufs;
    size_t buf_count, buf_cap;
    size_t decoded_total;
    PdfFont *fonts;
    size_t font_count, font_cap;
    PdfFont scratch_font;
    PdfFont default_font;
    unsigned char *visited;
    size_t pages;
    long root_ref, info_ref;
    size_t trailer_order;
    bool trailer_found;
    bool encrypted;
    bool oom;
    uint64_t unmapped_codes;
    double line_y;
    bool has_line_y;
    AsgendMemoryTextBuffer text;
    AsgendMemoryDocText *out;
} PdfDoc;

/* ------------------------------------------------------------------ values & lexer */

static void pv_free(PdfValue *value) {
    if (!value) return;
    free(value->bytes);
    for (size_t i = 0; i < value->count; ++i) pv_free(&value->items[i]);
    free(value->items);
    memset(value, 0, sizeof(*value));
}

static bool pdf_ws(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static bool pdf_delim(unsigned char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}
static bool pdf_regular(unsigned char c) { return !pdf_ws(c) && !pdf_delim(c); }
static bool pdf_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void lx_skip_ws(PdfLexer *lx) {
    while (lx->pos < lx->len) {
        unsigned char c = lx->data[lx->pos];
        if (pdf_ws(c)) { ++lx->pos; continue; }
        if (c == '%') {
            while (lx->pos < lx->len && lx->data[lx->pos] != '\n' && lx->data[lx->pos] != '\r') ++lx->pos;
            continue;
        }
        break;
    }
}

static const unsigned char *find_bytes(const unsigned char *hay, size_t hay_len, const char *needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return NULL;
    const unsigned char *end = hay + hay_len - needle_len + 1U;
    const unsigned char *p = hay;
    while (p < end) {
        const unsigned char *hit = memchr(p, (unsigned char)needle[0], (size_t)(end - p));
        if (!hit) return NULL;
        if (memcmp(hit, needle, needle_len) == 0) return hit;
        p = hit + 1;
    }
    return NULL;
}

static bool byte_push(unsigned char **buffer, size_t *length, size_t *capacity, unsigned char byte) {
    if (*length + 1U >= *capacity) {
        size_t next = *capacity ? *capacity * 2U : 64U;
        if (next < *capacity) return false;
        unsigned char *grown = realloc(*buffer, next);
        if (!grown) return false;
        *buffer = grown; *capacity = next;
    }
    (*buffer)[(*length)++] = byte;
    (*buffer)[*length] = 0;
    return true;
}

static bool value_push(PdfValue *container, PdfValue *item) {
    if (container->count == container->capacity) {
        size_t next = container->capacity ? container->capacity * 2U : 8U;
        if (next > SIZE_MAX / sizeof(PdfValue)) return false;
        PdfValue *grown = realloc(container->items, next * sizeof(PdfValue));
        if (!grown) return false;
        container->items = grown; container->capacity = next;
    }
    container->items[container->count++] = *item;
    memset(item, 0, sizeof(*item));
    return true;
}

static int parse_literal_string(PdfLexer *lx, PdfValue *out) {
    ++lx->pos;
    unsigned char *buffer = NULL; size_t length = 0, capacity = 0;
    unsigned int nesting = 1;
    if (!byte_push(&buffer, &length, &capacity, 0)) return PR_OOM;
    length = 0;
    while (lx->pos < lx->len) {
        unsigned char c = lx->data[lx->pos++];
        if (c == '\\') {
            if (lx->pos >= lx->len) break;
            unsigned char e = lx->data[lx->pos++];
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '\r':
                    if (lx->pos < lx->len && lx->data[lx->pos] == '\n') ++lx->pos;
                    continue;
                case '\n': continue;
                default:
                    if (e >= '0' && e <= '7') {
                        unsigned int value = (unsigned int)(e - '0');
                        for (int k = 0; k < 2 && lx->pos < lx->len && lx->data[lx->pos] >= '0' && lx->data[lx->pos] <= '7'; ++k)
                            value = value * 8U + (unsigned int)(lx->data[lx->pos++] - '0');
                        c = (unsigned char)(value & 0xffU);
                    } else c = e;
                    break;
            }
        } else if (c == '(') {
            ++nesting;
        } else if (c == ')') {
            if (--nesting == 0) break;
        }
        if (!byte_push(&buffer, &length, &capacity, c)) { free(buffer); return PR_OOM; }
    }
    out->type = PV_STR; out->bytes = buffer; out->len = length;
    return PR_VALUE;
}

static int parse_hex_string(PdfLexer *lx, PdfValue *out) {
    ++lx->pos;
    unsigned char *buffer = NULL; size_t length = 0, capacity = 0;
    if (!byte_push(&buffer, &length, &capacity, 0)) return PR_OOM;
    length = 0;
    int high = -1;
    while (lx->pos < lx->len) {
        unsigned char c = lx->data[lx->pos++];
        if (c == '>') break;
        int v = hex_value(c);
        if (v < 0) continue;
        if (high < 0) high = v;
        else {
            if (!byte_push(&buffer, &length, &capacity, (unsigned char)(high * 16 + v))) { free(buffer); return PR_OOM; }
            high = -1;
        }
    }
    if (high >= 0 && !byte_push(&buffer, &length, &capacity, (unsigned char)(high * 16))) { free(buffer); return PR_OOM; }
    out->type = PV_STR; out->bytes = buffer; out->len = length;
    return PR_VALUE;
}

static int parse_name(PdfLexer *lx, PdfValue *out) {
    ++lx->pos;
    unsigned char *buffer = NULL; size_t length = 0, capacity = 0;
    if (!byte_push(&buffer, &length, &capacity, 0)) return PR_OOM;
    length = 0;
    while (lx->pos < lx->len && pdf_regular(lx->data[lx->pos]) && length < 1024U) {
        unsigned char c = lx->data[lx->pos++];
        if (c == '#' && lx->pos + 1U < lx->len && hex_value(lx->data[lx->pos]) >= 0 && hex_value(lx->data[lx->pos + 1U]) >= 0) {
            c = (unsigned char)(hex_value(lx->data[lx->pos]) * 16 + hex_value(lx->data[lx->pos + 1U]));
            lx->pos += 2U;
            if (c == 0) continue;
        }
        if (!byte_push(&buffer, &length, &capacity, c)) { free(buffer); return PR_OOM; }
    }
    while (lx->pos < lx->len && pdf_regular(lx->data[lx->pos])) ++lx->pos;
    out->type = PV_NAME; out->bytes = buffer; out->len = length;
    return PR_VALUE;
}

static bool parse_unsigned(PdfLexer *lx, long *value) {
    size_t start = lx->pos;
    long result = 0;
    while (lx->pos < lx->len && pdf_digit(lx->data[lx->pos])) {
        if (lx->pos - start >= 10U) return false;
        result = result * 10L + (long)(lx->data[lx->pos] - '0');
        ++lx->pos;
    }
    *value = result;
    return lx->pos > start;
}

static int parse_number(PdfLexer *lx, PdfValue *out) {
    bool negative = false;
    if (lx->data[lx->pos] == '+' || lx->data[lx->pos] == '-') { negative = lx->data[lx->pos] == '-'; ++lx->pos; }
    double value = 0.0, scale = 1.0;
    bool fraction = false, any = false;
    unsigned int digits = 0;
    while (lx->pos < lx->len) {
        unsigned char c = lx->data[lx->pos];
        if (pdf_digit(c)) {
            any = true;
            if (++digits < 40U) {
                if (fraction) { scale /= 10.0; value += (double)(c - '0') * scale; }
                else value = value * 10.0 + (double)(c - '0');
            }
            ++lx->pos;
        } else if (c == '.' && !fraction) { fraction = true; ++lx->pos; }
        else if (c == '-' || c == '+') ++lx->pos; /* tolerate "--5" style producer bugs */
        else break;
    }
    (void)any;
    out->type = PV_NUM;
    out->num = negative ? -value : value;
    out->integer = !fraction;
    if (out->integer && !negative && value < 1e10) {
        /* Indirect reference: "N G R". */
        size_t save = lx->pos;
        lx_skip_ws(lx);
        long generation = 0;
        if (lx->pos < lx->len && pdf_digit(lx->data[lx->pos]) && parse_unsigned(lx, &generation)) {
            lx_skip_ws(lx);
            if (lx->pos < lx->len && lx->data[lx->pos] == 'R' &&
                (lx->pos + 1U >= lx->len || !pdf_regular(lx->data[lx->pos + 1U]))) {
                ++lx->pos;
                out->type = PV_REF;
                out->ref_num = (long)value;
                return PR_VALUE;
            }
        }
        lx->pos = save;
    }
    return PR_VALUE;
}

static int pdf_parse(PdfLexer *lx, PdfValue *out, unsigned int depth) {
    memset(out, 0, sizeof(*out));
    lx_skip_ws(lx);
    if (lx->pos >= lx->len) return PR_EOF;
    if (depth > PDF_MAX_DEPTH || lx->budget == 0) { ++lx->pos; return PR_ERROR; }
    --lx->budget;
    unsigned char c = lx->data[lx->pos];
    if (c == '/') return parse_name(lx, out);
    if (c == '(') return parse_literal_string(lx, out);
    if (c == '<') {
        if (lx->pos + 1U < lx->len && lx->data[lx->pos + 1U] == '<') {
            lx->pos += 2U;
            out->type = PV_DICT;
            for (;;) {
                lx_skip_ws(lx);
                if (lx->pos >= lx->len) { pv_free(out); return PR_ERROR; }
                if (lx->data[lx->pos] == '>') {
                    lx->pos += (lx->pos + 1U < lx->len && lx->data[lx->pos + 1U] == '>') ? 2U : 1U;
                    return PR_VALUE;
                }
                PdfValue key, value;
                int rc = pdf_parse(lx, &key, depth + 1U);
                if (rc != PR_VALUE || key.type != PV_NAME) {
                    pv_free(&key); pv_free(out);
                    return rc == PR_OOM ? PR_OOM : PR_ERROR;
                }
                rc = pdf_parse(lx, &value, depth + 1U);
                if (rc == PR_KEYWORD) { memset(&value, 0, sizeof(value)); value.type = PV_NULL; rc = PR_VALUE; }
                if (rc != PR_VALUE) { pv_free(&key); pv_free(&value); pv_free(out); return rc == PR_OOM ? PR_OOM : PR_ERROR; }
                if (!value_push(out, &key) || !value_push(out, &value)) { pv_free(&key); pv_free(&value); pv_free(out); return PR_OOM; }
            }
        }
        return parse_hex_string(lx, out);
    }
    if (c == '[') {
        ++lx->pos;
        out->type = PV_ARRAY;
        for (;;) {
            lx_skip_ws(lx);
            if (lx->pos >= lx->len) { pv_free(out); return PR_ERROR; }
            if (lx->data[lx->pos] == ']') { ++lx->pos; return PR_VALUE; }
            PdfValue item;
            int rc = pdf_parse(lx, &item, depth + 1U);
            if (rc == PR_KEYWORD) continue; /* tolerate stray keywords inside arrays */
            if (rc != PR_VALUE) { pv_free(&item); pv_free(out); return rc == PR_OOM ? PR_OOM : PR_ERROR; }
            if (!value_push(out, &item)) { pv_free(&item); pv_free(out); return PR_OOM; }
        }
    }
    if (c == ')' || c == '>' || c == ']' || c == '{' || c == '}') { ++lx->pos; return PR_ERROR; }
    if (pdf_digit(c) || c == '+' || c == '-' || c == '.') return parse_number(lx, out);
    size_t start = lx->pos;
    while (lx->pos < lx->len && pdf_regular(lx->data[lx->pos])) ++lx->pos;
    size_t length = lx->pos - start;
    if (length == 4U && !memcmp(lx->data + start, "true", 4U)) { out->type = PV_BOOL; out->boolean = true; return PR_VALUE; }
    if (length == 5U && !memcmp(lx->data + start, "false", 5U)) { out->type = PV_BOOL; return PR_VALUE; }
    if (length == 4U && !memcmp(lx->data + start, "null", 4U)) { out->type = PV_NULL; return PR_VALUE; }
    lx->kw = lx->data + start;
    lx->kw_len = length;
    return PR_KEYWORD;
}

static bool kw_is(const PdfLexer *lx, const char *word) {
    size_t n = strlen(word);
    return lx->kw_len == n && memcmp(lx->kw, word, n) == 0;
}

static const PdfValue *dict_get(const PdfValue *dict, const char *key) {
    if (!dict || dict->type != PV_DICT) return NULL;
    size_t n = strlen(key);
    for (size_t i = 0; i + 1U < dict->count; i += 2U) {
        const PdfValue *k = &dict->items[i];
        if (k->type == PV_NAME && k->len == n && memcmp(k->bytes, key, n) == 0) return &dict->items[i + 1U];
    }
    return NULL;
}

static bool name_is(const PdfValue *value, const char *name) {
    size_t n = strlen(name);
    return value && value->type == PV_NAME && value->len == n && memcmp(value->bytes, name, n) == 0;
}

/* ------------------------------------------------------------------ object index */

static int compare_objs(const void *a, const void *b) {
    const PdfObj *left = a, *right = b;
    if (left->num != right->num) return left->num < right->num ? -1 : 1;
    if (left->order != right->order) return left->order > right->order ? -1 : 1;
    return 0;
}

static long lookup_index(const PdfDoc *doc, long num) {
    size_t low = 0, high = doc->obj_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2U;
        if (doc->objs[mid].num < num) low = mid + 1U; else high = mid;
    }
    if (low < doc->obj_count && doc->objs[low].num == num) return (long)low;
    return -1;
}

static bool add_object(PdfDoc *doc, const PdfObj *object) {
    if (doc->obj_count >= PDF_MAX_OBJECTS) {
        asgend_memory_doc_reason(doc->out, "object limit of %u reached", PDF_MAX_OBJECTS);
        doc->out->partial = true;
        return false;
    }
    if (doc->obj_count == doc->obj_cap) {
        size_t next = doc->obj_cap ? doc->obj_cap * 2U : 256U;
        PdfObj *grown = realloc(doc->objs, next * sizeof(*grown));
        if (!grown) { doc->oom = true; return false; }
        doc->objs = grown; doc->obj_cap = next;
    }
    doc->objs[doc->obj_count++] = *object;
    return true;
}

static void lexer_init(PdfLexer *lx, const unsigned char *data, size_t len, size_t pos) {
    memset(lx, 0, sizeof(*lx));
    lx->data = data; lx->len = len; lx->pos = pos; lx->budget = PDF_MAX_VALUE_NODES;
}

static bool header_at(const unsigned char *d, size_t len, size_t k, long *num, size_t *header_start) {
    if (k + 3U < len && pdf_regular(d[k + 3U])) return false;
    if (k == 0 || !pdf_ws(d[k - 1U])) return false;
    size_t p = k;
    while (p > 0 && pdf_ws(d[p - 1U])) --p;
    size_t gen_end = p;
    while (p > 0 && pdf_digit(d[p - 1U])) --p;
    if (p == gen_end || gen_end - p > 5U) return false;
    if (p == 0 || !pdf_ws(d[p - 1U])) return false;
    while (p > 0 && pdf_ws(d[p - 1U])) --p;
    size_t num_end = p;
    while (p > 0 && pdf_digit(d[p - 1U])) --p;
    if (p == num_end || num_end - p > 10U) return false;
    if (p > 0 && pdf_regular(d[p - 1U])) return false;
    long value = 0;
    for (size_t i = p; i < num_end; ++i) value = value * 10L + (long)(d[i] - '0');
    *num = value;
    *header_start = p;
    return true;
}

static void note_trailer_dict(PdfDoc *doc, const PdfValue *dict, size_t order) {
    if (!dict || dict->type != PV_DICT) return;
    if (dict_get(dict, "Encrypt")) doc->encrypted = true;
    if (doc->trailer_found && order < doc->trailer_order) {
        /* Older trailer: only fill gaps. */
        const PdfValue *root = dict_get(dict, "Root");
        if (doc->root_ref < 0 && root && root->type == PV_REF) doc->root_ref = root->ref_num;
        const PdfValue *info = dict_get(dict, "Info");
        if (doc->info_ref < 0 && info && info->type == PV_REF) doc->info_ref = info->ref_num;
        return;
    }
    const PdfValue *root = dict_get(dict, "Root");
    const PdfValue *info = dict_get(dict, "Info");
    if (root && root->type == PV_REF) doc->root_ref = root->ref_num;
    if (info && info->type == PV_REF) doc->info_ref = info->ref_num;
    doc->trailer_found = true;
    doc->trailer_order = order;
}

static void scan_objects(PdfDoc *doc) {
    const unsigned char *d = doc->data;
    size_t len = doc->len, p = 0;
    while (p + 3U <= len && !doc->oom) {
        const unsigned char *hit = find_bytes(d + p, len - p, "obj", 3U);
        if (!hit) break;
        size_t k = (size_t)(hit - d);
        long num = 0; size_t header = 0;
        if (!header_at(d, len, k, &num, &header)) { p = k + 3U; continue; }
        PdfObj object;
        memset(&object, 0, sizeof(object));
        object.num = num; object.order = header; object.container = -1; object.start = k + 3U;
        PdfLexer lx;
        lexer_init(&lx, d, len, k + 3U);
        PdfValue value;
        int rc = pdf_parse(&lx, &value, 0);
        if (rc == PR_OOM) { doc->oom = true; break; }
        size_t value_end = lx.pos > k + 3U ? lx.pos : k + 3U;
        if (rc == PR_VALUE && value.type == PV_DICT) {
            PdfLexer after = lx;
            lx_skip_ws(&after);
            if (after.pos + 6U <= len && !memcmp(d + after.pos, "stream", 6U)) {
                size_t q = after.pos + 6U;
                if (q < len && d[q] == '\r') ++q;
                if (q < len && d[q] == '\n') ++q;
                bool resolved = false;
                const PdfValue *length_value = dict_get(&value, "Length");
                if (length_value && length_value->type == PV_NUM && length_value->integer &&
                    length_value->num >= 0.0 && length_value->num <= (double)(len - q)) {
                    size_t stream_len = (size_t)length_value->num, t = q + stream_len;
                    while (t < len && pdf_ws(d[t])) ++t;
                    if (t + 9U <= len && !memcmp(d + t, "endstream", 9U)) {
                        object.stream_len = stream_len;
                        value_end = t + 9U;
                        resolved = true;
                    }
                }
                if (!resolved) {
                    const unsigned char *end_hit = find_bytes(d + q, len - q, "endstream", 9U);
                    if (end_hit) {
                        size_t e = (size_t)(end_hit - d), data_end = e;
                        if (data_end > q && d[data_end - 1U] == '\n') --data_end;
                        if (data_end > q && d[data_end - 1U] == '\r') --data_end;
                        object.stream_len = data_end - q;
                        value_end = e + 9U;
                    } else {
                        object.stream_len = len - q;
                        value_end = len;
                        doc->out->partial = true;
                        asgend_memory_doc_reason(doc->out, "unterminated stream in object %ld", num);
                    }
                }
                object.has_stream = true;
                object.stream_start = q;
                const PdfValue *type = dict_get(&value, "Type");
                if (name_is(type, "XRef")) note_trailer_dict(doc, &value, header);
            }
        }
        pv_free(&value);
        object.end = value_end;
        if (!add_object(doc, &object)) break;
        p = value_end > k + 3U ? value_end : k + 3U;
    }
    /* Classic trailers (possibly several after incremental updates). */
    size_t q = 0;
    while (q + 7U <= len && !doc->oom) {
        const unsigned char *hit = find_bytes(d + q, len - q, "trailer", 7U);
        if (!hit) break;
        size_t k = (size_t)(hit - d);
        PdfLexer lx;
        lexer_init(&lx, d, len, k + 7U);
        PdfValue value;
        int rc = pdf_parse(&lx, &value, 0);
        if (rc == PR_OOM) { doc->oom = true; break; }
        if (rc == PR_VALUE) note_trailer_dict(doc, &value, k);
        pv_free(&value);
        q = k + 7U;
    }
}

static const unsigned char *container_data(const PdfDoc *doc, const PdfObj *object, size_t *len) {
    if (object->container < 0) { *len = doc->len; return doc->data; }
    *len = doc->bufs[object->container].len;
    return doc->bufs[object->container].data;
}

static int load_object(PdfDoc *doc, long num, PdfValue *out) {
    memset(out, 0, sizeof(*out));
    long index = lookup_index(doc, num);
    if (index < 0) return -1;
    const PdfObj *object = &doc->objs[index];
    size_t len = 0;
    const unsigned char *data = container_data(doc, object, &len);
    if (object->start > len || object->end > len || object->start > object->end) return -1;
    PdfLexer lx;
    lexer_init(&lx, data, object->end, object->start);
    int rc = pdf_parse(&lx, out, 0);
    if (rc == PR_OOM) { doc->oom = true; return -1; }
    if (rc != PR_VALUE) { pv_free(out); return -1; }
    return 0;
}

/* Resolves indirect references. *storage must start zeroed; the caller frees it afterwards. */
static const PdfValue *deref(PdfDoc *doc, const PdfValue *value, PdfValue *storage) {
    for (int hops = 0; value && value->type == PV_REF; ++hops) {
        if (hops >= 8) return NULL;
        PdfValue next;
        if (load_object(doc, value->ref_num, &next) != 0) return NULL;
        pv_free(storage);
        *storage = next;
        value = storage;
    }
    return value;
}

/* ------------------------------------------------------------------ stream filters */

static unsigned char *ascii_hex_decode(const unsigned char *in, size_t len, size_t *out_len) {
    unsigned char *out = malloc(len / 2U + 2U);
    if (!out) return NULL;
    size_t n = 0; int high = -1;
    for (size_t i = 0; i < len; ++i) {
        if (in[i] == '>') break;
        int v = hex_value(in[i]);
        if (v < 0) continue;
        if (high < 0) high = v; else { out[n++] = (unsigned char)(high * 16 + v); high = -1; }
    }
    if (high >= 0) out[n++] = (unsigned char)(high * 16);
    *out_len = n;
    return out;
}

static unsigned char *ascii85_decode(const unsigned char *in, size_t len, size_t *out_len, bool *bad) {
    unsigned char *out = malloc((len / 5U + 1U) * 4U + 4U);
    if (!out) return NULL;
    size_t n = 0; uint64_t tuple = 0; unsigned int count = 0;
    size_t i = 0;
    if (len >= 2U && in[0] == '<' && in[1] == '~') i = 2U;
    for (; i < len; ++i) {
        unsigned char c = in[i];
        if (pdf_ws(c)) continue;
        if (c == '~') break;
        if (c == 'z' && count == 0) {
            if (n + 4U > (len / 5U + 1U) * 4U + 4U) break;
            memset(out + n, 0, 4U); n += 4U; continue;
        }
        if (c < '!' || c > 'u') { *bad = true; break; }
        tuple = tuple * 85U + (uint64_t)(c - '!');
        if (++count == 5U) {
            if (tuple > 0xffffffffULL) { *bad = true; break; }
            out[n++] = (unsigned char)(tuple >> 24U); out[n++] = (unsigned char)(tuple >> 16U);
            out[n++] = (unsigned char)(tuple >> 8U); out[n++] = (unsigned char)tuple;
            tuple = 0; count = 0;
        }
    }
    if (count > 1U && !*bad) {
        for (unsigned int k = count; k < 5U; ++k) tuple = tuple * 85U + 84U;
        if (tuple <= 0xffffffffULL)
            for (unsigned int k = 0; k + 1U < count; ++k) out[n++] = (unsigned char)(tuple >> (24U - 8U * k));
    }
    *out_len = n;
    return out;
}

/* Returns 0 with an owned buffer, 1 when the stream cannot be decoded (reason recorded), -1 OOM. */
static int stream_data(PdfDoc *doc, long num, unsigned char **out, size_t *out_len) {
    *out = NULL; *out_len = 0;
    long index = lookup_index(doc, num);
    if (index < 0 || !doc->objs[index].has_stream || doc->objs[index].container >= 0) return 1;
    PdfObj object = doc->objs[index];
    if (object.stream_start > doc->len || object.stream_len > doc->len - object.stream_start) return 1;
    PdfValue dict_storage;
    if (load_object(doc, num, &dict_storage) != 0) return doc->oom ? -1 : 1;
    PdfValue filter_storage, parms_storage;
    memset(&filter_storage, 0, sizeof(filter_storage));
    memset(&parms_storage, 0, sizeof(parms_storage));
    const PdfValue *filter = deref(doc, dict_get(&dict_storage, "Filter"), &filter_storage);
    const PdfValue *parms = deref(doc, dict_get(&dict_storage, "DecodeParms"), &parms_storage);
    const PdfValue *filters[4];
    size_t filter_count = 0;
    int result = 0;
    if (filter && filter->type == PV_NAME) filters[filter_count++] = filter;
    else if (filter && filter->type == PV_ARRAY) {
        for (size_t i = 0; i < filter->count; ++i) {
            if (filter_count == 4U) { asgend_memory_doc_reason(doc->out, "too many stream filters"); result = 1; break; }
            filters[filter_count++] = &filter->items[i];
        }
    }
    unsigned char *buffer = NULL; size_t length = object.stream_len;
    if (result == 0) {
        buffer = malloc(length + 1U);
        if (!buffer) result = -1;
        else { memcpy(buffer, doc->data + object.stream_start, length); buffer[length] = 0; }
    }
    for (size_t f = 0; result == 0 && f < filter_count; ++f) {
        const PdfValue *name = filters[f];
        const PdfValue *parm = parms && parms->type == PV_ARRAY ? (f < parms->count ? &parms->items[f] : NULL) : parms;
        PdfValue parm_storage;
        memset(&parm_storage, 0, sizeof(parm_storage));
        parm = deref(doc, parm, &parm_storage);
        const PdfValue *predictor = dict_get(parm, "Predictor");
        bool predicted = predictor && predictor->type == PV_NUM && predictor->num > 1.0;
        pv_free(&parm_storage);
        unsigned char *decoded = NULL; size_t decoded_len = 0;
        if (name_is(name, "FlateDecode") || name_is(name, "Fl")) {
            if (predicted) { asgend_memory_doc_reason(doc->out, "unsupported Flate predictor"); result = 1; break; }
            size_t remaining = doc->decoded_total < PDF_MAX_TOTAL_DECODED ? PDF_MAX_TOTAL_DECODED - doc->decoded_total : 0;
            if (remaining == 0) {
                asgend_memory_doc_reason(doc->out, "total decompression budget of %u bytes exhausted", PDF_MAX_TOTAL_DECODED);
                doc->out->partial = true; result = 1; break;
            }
            size_t cap = remaining < PDF_MAX_STREAM_BYTES ? remaining : PDF_MAX_STREAM_BYTES;
            bool limit_hit = false, corrupt = false;
            int rc = asgend_memory_inflate_bounded(buffer, length, false, cap, &decoded, &decoded_len, &limit_hit, &corrupt);
            if (rc == -2) { result = -1; break; }
            if (rc != 0) {
                asgend_memory_doc_reason(doc->out, "corrupt FlateDecode stream in object %ld", num);
                doc->out->partial = true; result = 1; break;
            }
            if (limit_hit) {
                asgend_memory_doc_reason(doc->out, "stream in object %ld exceeds the decompression limit", num);
                doc->out->partial = true;
            } else if (corrupt) {
                asgend_memory_doc_reason(doc->out, "truncated FlateDecode stream in object %ld", num);
                doc->out->partial = true;
            }
            doc->decoded_total += decoded_len;
        } else if (name_is(name, "ASCIIHexDecode") || name_is(name, "AHx")) {
            decoded = ascii_hex_decode(buffer, length, &decoded_len);
            if (!decoded) { result = -1; break; }
        } else if (name_is(name, "ASCII85Decode") || name_is(name, "A85")) {
            bool bad = false;
            decoded = ascii85_decode(buffer, length, &decoded_len, &bad);
            if (!decoded) { result = -1; break; }
            if (bad) { asgend_memory_doc_reason(doc->out, "malformed ASCII85 stream"); doc->out->partial = true; }
        } else {
            char filter_name[48] = "?";
            if (name && name->type == PV_NAME) (void)snprintf(filter_name, sizeof(filter_name), "%.40s", (const char *)name->bytes);
            asgend_memory_doc_reason(doc->out, "unsupported stream filter %s", filter_name);
            result = 1; break;
        }
        free(buffer);
        buffer = decoded; length = decoded_len;
    }
    pv_free(&filter_storage); pv_free(&parms_storage); pv_free(&dict_storage);
    if (result != 0) { free(buffer); if (result == -1) doc->oom = true; return result; }
    *out = buffer; *out_len = length;
    return 0;
}

static void load_object_streams(PdfDoc *doc) {
    size_t original = doc->obj_count;
    size_t embedded = 0;
    for (size_t i = 0; i < original && !doc->oom; ++i) {
        if (!doc->objs[i].has_stream || doc->objs[i].container >= 0) continue;
        PdfObj holder = doc->objs[i];
        PdfLexer lx;
        lexer_init(&lx, doc->data, holder.end, holder.start);
        PdfValue dict;
        int rc = pdf_parse(&lx, &dict, 0);
        if (rc == PR_OOM) { doc->oom = true; return; }
        bool is_objstm = rc == PR_VALUE && name_is(dict_get(&dict, "Type"), "ObjStm");
        const PdfValue *n_value = dict_get(&dict, "N"), *first_value = dict_get(&dict, "First");
        double n_number = n_value && n_value->type == PV_NUM ? n_value->num : -1.0;
        double first_number = first_value && first_value->type == PV_NUM ? first_value->num : -1.0;
        pv_free(&dict);
        if (!is_objstm) continue;
        if (n_number < 0.0 || first_number < 0.0 || n_number > (double)PDF_MAX_OBJSTM_OBJECTS) {
            doc->out->partial = true;
            asgend_memory_doc_reason(doc->out, "malformed object stream header");
            continue;
        }
        /* stream_data needs the sorted index; search the unsorted array via a temporary sort. */
        unsigned char *data = NULL; size_t data_len = 0;
        {
            /* Decode directly from this holder without lookup (holder may be shadowed). */
            PdfDoc *self = doc;
            long saved_count = (long)self->obj_count;
            (void)saved_count;
        }
        PdfObj saved = doc->objs[i];
        /* Temporarily expose only this object through lookup_index by using a one-entry view. */
        PdfObj *saved_objs = doc->objs; size_t saved_obj_count = doc->obj_count;
        doc->objs = &saved; doc->obj_count = 1U;
        rc = stream_data(doc, saved.num, &data, &data_len);
        doc->objs = saved_objs; doc->obj_count = saved_obj_count;
        if (rc == -1) { doc->oom = true; return; }
        if (rc != 0) { doc->out->partial = true; asgend_memory_doc_reason(doc->out, "object stream could not be decoded"); continue; }
        size_t first = (size_t)first_number, count = (size_t)n_number;
        if (first > data_len || embedded + count > PDF_MAX_OBJSTM_OBJECTS) {
            free(data); doc->out->partial = true; asgend_memory_doc_reason(doc->out, "object stream exceeds limits"); continue;
        }
        if (doc->buf_count == doc->buf_cap) {
            size_t next = doc->buf_cap ? doc->buf_cap * 2U : 16U;
            PdfBuf *grown = realloc(doc->bufs, next * sizeof(*grown));
            if (!grown) { free(data); doc->oom = true; return; }
            doc->bufs = grown; doc->buf_cap = next;
        }
        int buf_index = (int)doc->buf_count;
        doc->bufs[doc->buf_count].data = data;
        doc->bufs[doc->buf_count].len = data_len;
        doc->buf_count++;
        PdfLexer header;
        lexer_init(&header, data, first, 0);
        long previous_num = -1; size_t previous_offset = 0; bool have_previous = false;
        for (size_t k = 0; k < count; ++k) {
            lx_skip_ws(&header);
            long object_num = 0, offset = 0;
            if (!parse_unsigned(&header, &object_num)) break;
            lx_skip_ws(&header);
            if (!parse_unsigned(&header, &offset)) break;
            size_t absolute = first + (size_t)offset;
            if (absolute > data_len) break;
            if (have_previous && absolute >= first + previous_offset) {
                PdfObj inner;
                memset(&inner, 0, sizeof(inner));
                inner.num = previous_num; inner.order = holder.order; inner.container = buf_index;
                inner.start = first + previous_offset; inner.end = absolute;
                if (!add_object(doc, &inner)) return;
                ++embedded;
            }
            previous_num = object_num; previous_offset = (size_t)offset; have_previous = true;
        }
        if (have_previous && first + previous_offset <= data_len) {
            PdfObj inner;
            memset(&inner, 0, sizeof(inner));
            inner.num = previous_num; inner.order = holder.order; inner.container = buf_index;
            inner.start = first + previous_offset; inner.end = data_len;
            if (!add_object(doc, &inner)) return;
            ++embedded;
        }
    }
}

/* ------------------------------------------------------------------ encodings and fonts */

static const uint16_t winansi_80[32] = {
    0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
    0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
};

static const uint16_t pdfdoc_18[8] = { 0x02d8, 0x02c7, 0x02c6, 0x02d9, 0x02dd, 0x02db, 0x02da, 0x02dc };
static const uint16_t pdfdoc_80[33] = {
    0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203a, 0x2212, 0x2030, 0x201e, 0x201c, 0x201d, 0x2018,
    0x2019, 0x201a, 0x2122, 0xfb01, 0xfb02, 0x0141, 0x0152, 0x0160, 0x0178, 0x017d, 0x0131, 0x0142, 0x0153, 0x0161, 0x017e, 0,
    0x20ac
};

static void winansi_map(uint32_t map[256]) {
    for (unsigned int c = 0; c < 256U; ++c) {
        if (c < 0x20U) map[c] = c == 9U ? 9U : 0U;
        else if (c < 0x7fU) map[c] = c;
        else if (c == 0x7fU) map[c] = 0;
        else if (c < 0xa0U) map[c] = winansi_80[c - 0x80U];
        else map[c] = c;
    }
}

static uint32_t pdfdoc_codepoint(unsigned char c) {
    if (c >= 0x18U && c <= 0x1fU) return pdfdoc_18[c - 0x18U];
    if (c >= 0x80U && c <= 0xa0U) return pdfdoc_80[c - 0x80U];
    return c;
}

static const char *const latin1_glyphs[64] = {
    "Agrave", "Aacute", "Acircumflex", "Atilde", "Adieresis", "Aring", "AE", "Ccedilla",
    "Egrave", "Eacute", "Ecircumflex", "Edieresis", "Igrave", "Iacute", "Icircumflex", "Idieresis",
    "Eth", "Ntilde", "Ograve", "Oacute", "Ocircumflex", "Otilde", "Odieresis", "multiply",
    "Oslash", "Ugrave", "Uacute", "Ucircumflex", "Udieresis", "Yacute", "Thorn", "germandbls",
    "agrave", "aacute", "acircumflex", "atilde", "adieresis", "aring", "ae", "ccedilla",
    "egrave", "eacute", "ecircumflex", "edieresis", "igrave", "iacute", "icircumflex", "idieresis",
    "eth", "ntilde", "ograve", "oacute", "ocircumflex", "otilde", "odieresis", "divide",
    "oslash", "ugrave", "uacute", "ucircumflex", "udieresis", "yacute", "thorn", "ydieresis"
};

static const struct { const char *name; uint16_t codepoint; } glyph_table[] = {
    {"space", 0x20}, {"exclam", 0x21}, {"quotedbl", 0x22}, {"numbersign", 0x23}, {"dollar", 0x24},
    {"percent", 0x25}, {"ampersand", 0x26}, {"quotesingle", 0x27}, {"quoteright", 0x2019},
    {"parenleft", 0x28}, {"parenright", 0x29}, {"asterisk", 0x2a}, {"plus", 0x2b}, {"comma", 0x2c},
    {"hyphen", 0x2d}, {"minus", 0x2212}, {"period", 0x2e}, {"slash", 0x2f}, {"zero", 0x30},
    {"one", 0x31}, {"two", 0x32}, {"three", 0x33}, {"four", 0x34}, {"five", 0x35}, {"six", 0x36},
    {"seven", 0x37}, {"eight", 0x38}, {"nine", 0x39}, {"colon", 0x3a}, {"semicolon", 0x3b},
    {"less", 0x3c}, {"equal", 0x3d}, {"greater", 0x3e}, {"question", 0x3f}, {"at", 0x40},
    {"bracketleft", 0x5b}, {"backslash", 0x5c}, {"bracketright", 0x5d}, {"asciicircum", 0x5e},
    {"underscore", 0x5f}, {"grave", 0x60}, {"quoteleft", 0x2018}, {"braceleft", 0x7b}, {"bar", 0x7c},
    {"braceright", 0x7d}, {"asciitilde", 0x7e}, {"quotedblleft", 0x201c}, {"quotedblright", 0x201d},
    {"quotesinglbase", 0x201a}, {"quotedblbase", 0x201e}, {"endash", 0x2013}, {"emdash", 0x2014},
    {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
    {"perthousand", 0x2030}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203a},
    {"guillemotleft", 0xab}, {"guillemotright", 0xbb}, {"fi", 0xfb01}, {"fl", 0xfb02}, {"ff", 0xfb00},
    {"ffi", 0xfb03}, {"ffl", 0xfb04}, {"copyright", 0xa9}, {"registered", 0xae}, {"trademark", 0x2122},
    {"degree", 0xb0}, {"Euro", 0x20ac}, {"euro", 0x20ac}, {"sterling", 0xa3}, {"yen", 0xa5}, {"cent", 0xa2},
    {"section", 0xa7}, {"paragraph", 0xb6}, {"periodcentered", 0xb7}, {"exclamdown", 0xa1},
    {"questiondown", 0xbf}, {"nbspace", 0xa0}, {"nonbreakingspace", 0xa0}, {"florin", 0x192},
    {"OE", 0x152}, {"oe", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Zcaron", 0x17d},
    {"zcaron", 0x17e}, {"Ydieresis", 0x178}, {"dotlessi", 0x131}, {"Lslash", 0x141}, {"lslash", 0x142},
    {"circumflex", 0x2c6}, {"tilde", 0x2dc}, {"plusminus", 0xb1}, {"mu", 0xb5}, {"onehalf", 0xbd},
    {"onequarter", 0xbc}, {"threequarters", 0xbe}, {"ordfeminine", 0xaa}, {"ordmasculine", 0xba},
    {"logicalnot", 0xac}, {"dieresis", 0xa8}, {"acute", 0xb4}, {"cedilla", 0xb8}, {"macron", 0xaf},
    {"brokenbar", 0xa6}, {"currency", 0xa4}, {"twosuperior", 0xb2}, {"threesuperior", 0xb3},
    {"onesuperior", 0xb9}
};

static uint32_t glyph_codepoint(const unsigned char *raw, size_t raw_len) {
    char name[64];
    if (raw_len == 0 || raw_len >= sizeof(name)) return 0;
    memcpy(name, raw, raw_len); name[raw_len] = '\0';
    char *dot = strchr(name, '.');
    if (dot && dot != name) *dot = '\0';
    size_t len = strlen(name);
    if (len == 1U && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) return (uint32_t)name[0];
    if ((len == 7U && !strncmp(name, "uni", 3U)) || (len >= 5U && len <= 7U && name[0] == 'u' && name[1] != 'n')) {
        size_t start = name[1] == 'n' ? 3U : 1U;
        uint32_t value = 0;
        for (size_t i = start; i < len; ++i) {
            int h = hex_value((unsigned char)name[i]);
            if (h < 0) return 0;
            value = value * 16U + (uint32_t)h;
        }
        return value <= 0x10ffffU ? value : 0;
    }
    for (size_t i = 0; i < sizeof(glyph_table) / sizeof(glyph_table[0]); ++i)
        if (!strcmp(glyph_table[i].name, name)) return glyph_table[i].codepoint;
    for (size_t i = 0; i < 64U; ++i) if (!strcmp(latin1_glyphs[i], name)) return 0xc0U + (uint32_t)i;
    return 0;
}

static void font_free(PdfFont *font) {
    free(font->chars);
    free(font->ranges);
    memset(font, 0, sizeof(*font));
}

static void set_dst(const PdfValue *value, uint16_t dst[8], unsigned char *dst_len) {
    *dst_len = 0;
    if (!value) return;
    if (value->type == PV_NAME) {
        uint32_t cp = glyph_codepoint(value->bytes, value->len);
        if (cp && cp < 0x10000U) { dst[0] = (uint16_t)cp; *dst_len = 1; }
        return;
    }
    if (value->type != PV_STR) return;
    if (value->len == 1U) { dst[0] = value->bytes[0]; *dst_len = 1; return; }
    for (size_t i = 0; i + 1U < value->len && *dst_len < 8U; i += 2U)
        dst[(*dst_len)++] = (uint16_t)((value->bytes[i] << 8U) | value->bytes[i + 1U]);
}

static uint32_t be_code(const unsigned char *bytes, size_t len) {
    uint32_t code = 0;
    for (size_t i = 0; i < len && i < 4U; ++i) code = (code << 8U) | bytes[i];
    return code;
}

static bool cmap_add_char(PdfFont *font, uint32_t code, const PdfValue *dst) {
    if (font->char_count >= PDF_MAX_CMAP_CHARS) return true;
    if (font->char_count == font->char_cap) {
        size_t next = font->char_cap ? font->char_cap * 2U : 64U;
        PdfCmapChar *grown = realloc(font->chars, next * sizeof(*grown));
        if (!grown) return false;
        font->chars = grown; font->char_cap = next;
    }
    PdfCmapChar *entry = &font->chars[font->char_count];
    entry->code = code;
    set_dst(dst, entry->dst, &entry->dst_len);
    if (entry->dst_len) font->char_count++;
    return true;
}

static int compare_cmap_chars(const void *a, const void *b) {
    const PdfCmapChar *left = a, *right = b;
    return left->code < right->code ? -1 : (left->code > right->code ? 1 : 0);
}

static bool parse_cmap(PdfFont *font, const unsigned char *data, size_t len) {
    PdfLexer lx;
    lexer_init(&lx, data, len, 0);
    PdfValue *stack = calloc(PDF_MAX_CMAP_STACK, sizeof(*stack));
    if (!stack) return false;
    size_t sp = 0;
    bool ok = true;
    for (;;) {
        size_t before = lx.pos;
        PdfValue value;
        int rc = pdf_parse(&lx, &value, 0);
        if (rc == PR_EOF) break;
        if (rc == PR_OOM) { ok = false; break; }
        if (rc == PR_ERROR) { if (lx.pos == before) ++lx.pos; continue; }
        if (rc == PR_VALUE) {
            if (sp < PDF_MAX_CMAP_STACK) stack[sp++] = value; else pv_free(&value);
            continue;
        }
        if (kw_is(&lx, "endcodespacerange")) {
            for (size_t i = 0; i + 1U < sp; i += 2U) {
                if (stack[i].type != PV_STR || stack[i + 1U].type != PV_STR || stack[i].len == 0 || stack[i].len > 4U) continue;
                if (font->cs_count >= PDF_MAX_CODESPACES) break;
                PdfCodespace *cs = &font->cs[font->cs_count++];
                cs->nbytes = (unsigned char)stack[i].len;
                cs->lo = be_code(stack[i].bytes, stack[i].len);
                cs->hi = be_code(stack[i + 1U].bytes, stack[i + 1U].len);
            }
        } else if (kw_is(&lx, "endbfchar")) {
            for (size_t i = 0; ok && i + 1U < sp; i += 2U) {
                if (stack[i].type != PV_STR || stack[i].len == 0 || stack[i].len > 4U) continue;
                ok = cmap_add_char(font, be_code(stack[i].bytes, stack[i].len), &stack[i + 1U]);
            }
            font->has_cmap = true;
        } else if (kw_is(&lx, "endbfrange")) {
            for (size_t i = 0; ok && i + 2U < sp; i += 3U) {
                const PdfValue *lo = &stack[i], *hi = &stack[i + 1U], *dst = &stack[i + 2U];
                if (lo->type != PV_STR || hi->type != PV_STR || lo->len == 0 || lo->len > 4U || hi->len > 4U) continue;
                uint32_t lo_code = be_code(lo->bytes, lo->len), hi_code = be_code(hi->bytes, hi->len);
                if (hi_code < lo_code || hi_code - lo_code > 0xffffU) continue;
                if (dst->type == PV_ARRAY) {
                    for (size_t k = 0; ok && k < dst->count && k <= (size_t)(hi_code - lo_code); ++k)
                        ok = cmap_add_char(font, lo_code + (uint32_t)k, &dst->items[k]);
                } else if (dst->type == PV_STR) {
                    if (font->range_count >= PDF_MAX_CMAP_RANGES) continue;
                    if (font->range_count == font->range_cap) {
                        size_t next = font->range_cap ? font->range_cap * 2U : 16U;
                        PdfCmapRange *grown = realloc(font->ranges, next * sizeof(*grown));
                        if (!grown) { ok = false; break; }
                        font->ranges = grown; font->range_cap = next;
                    }
                    PdfCmapRange *range = &font->ranges[font->range_count];
                    range->lo = lo_code; range->hi = hi_code;
                    set_dst(dst, range->dst, &range->dst_len);
                    if (range->dst_len) font->range_count++;
                }
            }
            font->has_cmap = true;
        }
        for (size_t i = 0; i < sp; ++i) pv_free(&stack[i]);
        sp = 0;
        lx.budget = PDF_MAX_VALUE_NODES;
    }
    for (size_t i = 0; i < sp; ++i) pv_free(&stack[i]);
    free(stack);
    if (font->char_count > 1U) qsort(font->chars, font->char_count, sizeof(*font->chars), compare_cmap_chars);
    return ok;
}

static void apply_encoding_name(const PdfValue *name, uint32_t map[256]) {
    winansi_map(map);
    if (name_is(name, "StandardEncoding")) { map[0x27] = 0x2019; map[0x60] = 0x2018; }
    else if (name_is(name, "MacRomanEncoding")) { for (unsigned int c = 0x80U; c < 256U; ++c) map[c] = 0; }
}

static void build_font(PdfDoc *doc, const PdfValue *dict, PdfFont *font, long objnum) {
    memset(font, 0, sizeof(*font));
    font->objnum = objnum;
    winansi_map(font->simple_map);
    if (!dict || dict->type != PV_DICT) return;
    font->composite = name_is(dict_get(dict, "Subtype"), "Type0");
    PdfValue encoding_storage;
    memset(&encoding_storage, 0, sizeof(encoding_storage));
    const PdfValue *encoding = deref(doc, dict_get(dict, "Encoding"), &encoding_storage);
    if (encoding && encoding->type == PV_NAME) apply_encoding_name(encoding, font->simple_map);
    else if (encoding && encoding->type == PV_DICT) {
        apply_encoding_name(dict_get(encoding, "BaseEncoding"), font->simple_map);
        const PdfValue *differences = dict_get(encoding, "Differences");
        if (differences && differences->type == PV_ARRAY) {
            long code = -1;
            for (size_t i = 0; i < differences->count; ++i) {
                const PdfValue *item = &differences->items[i];
                if (item->type == PV_NUM) code = (long)item->num;
                else if (item->type == PV_NAME && code >= 0 && code < 256) {
                    font->simple_map[code] = glyph_codepoint(item->bytes, item->len);
                    ++code;
                }
            }
        }
    }
    pv_free(&encoding_storage);
    const PdfValue *to_unicode = dict_get(dict, "ToUnicode");
    if (to_unicode && to_unicode->type == PV_REF) {
        unsigned char *cmap = NULL; size_t cmap_len = 0;
        int rc = stream_data(doc, to_unicode->ref_num, &cmap, &cmap_len);
        if (rc == 0) {
            if (!parse_cmap(font, cmap, cmap_len)) doc->oom = true;
        } else if (rc == 1) {
            doc->out->partial = true;
            asgend_memory_doc_reason(doc->out, "ToUnicode CMap could not be decoded");
        }
        free(cmap);
    }
}

static const PdfFont *font_for_name(PdfDoc *doc, const PdfValue *fonts, const PdfValue *name) {
    if (!fonts || !name || name->type != PV_NAME) return &doc->default_font;
    const PdfValue *entry = dict_get(fonts, (const char *)name->bytes);
    if (!entry) return &doc->default_font;
    if (entry->type == PV_REF) {
        for (size_t i = 0; i < doc->font_count; ++i) if (doc->fonts[i].objnum == entry->ref_num) return &doc->fonts[i];
    }
    PdfValue storage;
    memset(&storage, 0, sizeof(storage));
    const PdfValue *dict = deref(doc, entry, &storage);
    PdfFont built;
    build_font(doc, dict, &built, entry->type == PV_REF ? entry->ref_num : -1);
    pv_free(&storage);
    if (entry->type == PV_REF && doc->font_count < PDF_MAX_FONTS) {
        if (doc->font_count == doc->font_cap) {
            size_t next = doc->font_cap ? doc->font_cap * 2U : 16U;
            PdfFont *grown = realloc(doc->fonts, next * sizeof(*grown));
            if (!grown) { font_free(&built); doc->oom = true; return &doc->default_font; }
            doc->fonts = grown; doc->font_cap = next;
        }
        doc->fonts[doc->font_count] = built;
        return &doc->fonts[doc->font_count++];
    }
    font_free(&doc->scratch_font);
    doc->scratch_font = built;
    return &doc->scratch_font;
}

static void emit_units(PdfDoc *doc, const uint16_t *units, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t unit = units[i];
        if (unit >= 0xd800U && unit <= 0xdbffU && i + 1U < count && units[i + 1U] >= 0xdc00U && units[i + 1U] <= 0xdfffU) {
            uint32_t cp = 0x10000U + ((unit - 0xd800U) << 10U) + (uint32_t)(units[i + 1U] - 0xdc00U);
            asgend_memory_text_codepoint(&doc->text, cp);
            ++i;
        } else asgend_memory_text_codepoint(&doc->text, unit);
    }
}

static bool cmap_lookup(const PdfFont *font, uint32_t code, PdfDoc *doc) {
    size_t low = 0, high = font->char_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2U;
        if (font->chars[mid].code < code) low = mid + 1U; else high = mid;
    }
    if (low < font->char_count && font->chars[low].code == code) {
        emit_units(doc, font->chars[low].dst, font->chars[low].dst_len);
        return true;
    }
    for (size_t i = 0; i < font->range_count; ++i) {
        const PdfCmapRange *range = &font->ranges[i];
        if (code < range->lo || code > range->hi) continue;
        uint16_t units[8];
        memcpy(units, range->dst, sizeof(units));
        units[range->dst_len - 1U] = (uint16_t)(units[range->dst_len - 1U] + (code - range->lo));
        emit_units(doc, units, range->dst_len);
        return true;
    }
    return false;
}

static void show_string(PdfDoc *doc, const PdfFont *font, const PdfValue *string) {
    if (!string || string->type != PV_STR) return;
    if (!font) font = &doc->default_font;
    const unsigned char *s = string->bytes;
    size_t n = string->len, i = 0;
    while (i < n && !doc->text.truncated) {
        if (font->has_cmap || font->composite) {
            size_t nbytes = 0;
            for (size_t width = 1; width <= 4U && !nbytes && i + width <= n; ++width) {
                uint32_t code = be_code(s + i, width);
                for (size_t k = 0; k < font->cs_count; ++k)
                    if (font->cs[k].nbytes == width && code >= font->cs[k].lo && code <= font->cs[k].hi) { nbytes = width; break; }
            }
            if (!nbytes) nbytes = font->composite ? 2U : 1U;
            if (i + nbytes > n) break;
            uint32_t code = be_code(s + i, nbytes);
            i += nbytes;
            if (font->has_cmap && cmap_lookup(font, code, doc)) continue;
            if (!font->composite && code < 256U && font->simple_map[code]) { asgend_memory_text_codepoint(&doc->text, font->simple_map[code]); continue; }
            doc->unmapped_codes++;
        } else {
            uint32_t cp = font->simple_map[s[i++]];
            if (cp) asgend_memory_text_codepoint(&doc->text, cp);
            else doc->unmapped_codes++;
        }
    }
}

/* ------------------------------------------------------------------ content streams */

typedef struct {
    PdfDoc *doc;
    PdfValue res_storage, fonts_storage, xobj_storage;
    const PdfValue *resources;
    const PdfValue *fonts;
    const PdfValue *xobjects;
    const PdfFont *font;
    double tm_y;
    bool positioned;
    bool force_newline;
    unsigned int depth;
} PdfContent;

static void interpret(PdfDoc *doc, const unsigned char *data, size_t len, const PdfValue *resources, unsigned int depth);

static void before_show(PdfContent *content) {
    PdfDoc *doc = content->doc;
    double delta = doc->has_line_y ? content->tm_y - doc->line_y : 0.0;
    if (delta < 0.0) delta = -delta;
    if (content->force_newline || (doc->has_line_y && delta > 1.0)) asgend_memory_text_separator(&doc->text, 2U);
    else if (content->positioned) asgend_memory_text_separator(&doc->text, 1U);
    content->force_newline = false;
    content->positioned = false;
    doc->line_y = content->tm_y;
    doc->has_line_y = true;
}

static void skip_inline_image(PdfLexer *lx) {
    if (lx->pos < lx->len && pdf_ws(lx->data[lx->pos])) ++lx->pos;
    while (lx->pos + 1U < lx->len) {
        if (lx->data[lx->pos] == 'E' && lx->data[lx->pos + 1U] == 'I' &&
            (lx->pos == 0 || pdf_ws(lx->data[lx->pos - 1U])) &&
            (lx->pos + 2U >= lx->len || pdf_ws(lx->data[lx->pos + 2U]))) { lx->pos += 2U; return; }
        ++lx->pos;
    }
    lx->pos = lx->len;
}

static void run_form(PdfContent *content, const PdfValue *name) {
    PdfDoc *doc = content->doc;
    if (!content->xobjects || !name || name->type != PV_NAME || content->depth >= PDF_MAX_FORM_DEPTH) return;
    const PdfValue *entry = dict_get(content->xobjects, (const char *)name->bytes);
    if (!entry || entry->type != PV_REF) return;
    PdfValue dict;
    if (load_object(doc, entry->ref_num, &dict) != 0) return;
    if (!name_is(dict_get(&dict, "Subtype"), "Form")) { pv_free(&dict); return; }
    unsigned char *data = NULL; size_t data_len = 0;
    int rc = stream_data(doc, entry->ref_num, &data, &data_len);
    if (rc == 0) {
        const PdfValue *form_resources = dict_get(&dict, "Resources");
        interpret(doc, data, data_len, form_resources ? form_resources : content->resources, content->depth + 1U);
    } else if (rc == 1) doc->out->partial = true;
    free(data);
    pv_free(&dict);
}

static void execute(PdfContent *content, PdfLexer *lx, PdfValue *ops, size_t count) {
    PdfDoc *doc = content->doc;
    const PdfValue *last = count ? &ops[count - 1U] : NULL;
    if (kw_is(lx, "BT")) { content->tm_y = 0.0; content->positioned = true; }
    else if (kw_is(lx, "Tf")) { if (count >= 1U) content->font = font_for_name(doc, content->fonts, &ops[0]); }
    else if (kw_is(lx, "Tj")) { before_show(content); show_string(doc, content->font, last); }
    else if (kw_is(lx, "'") || kw_is(lx, "\"")) { content->force_newline = true; before_show(content); show_string(doc, content->font, last); }
    else if (kw_is(lx, "TJ")) {
        before_show(content);
        if (last && last->type == PV_ARRAY) {
            for (size_t i = 0; i < last->count; ++i) {
                const PdfValue *item = &last->items[i];
                if (item->type == PV_STR) show_string(doc, content->font, item);
                else if (item->type == PV_NUM && item->num < -200.0) asgend_memory_text_separator(&doc->text, 1U);
            }
        }
    } else if (kw_is(lx, "Td") || kw_is(lx, "TD")) {
        if (count >= 2U && ops[1].type == PV_NUM) content->tm_y += ops[1].num;
        content->positioned = true;
    } else if (kw_is(lx, "Tm")) {
        if (count >= 6U && ops[5].type == PV_NUM) content->tm_y = ops[5].num;
        content->positioned = true;
    } else if (kw_is(lx, "T*")) content->force_newline = true;
    else if (kw_is(lx, "Do")) { if (count >= 1U) run_form(content, &ops[0]); }
    else if (kw_is(lx, "ID")) skip_inline_image(lx);
}

static void interpret(PdfDoc *doc, const unsigned char *data, size_t len, const PdfValue *resources, unsigned int depth) {
    PdfContent content;
    memset(&content, 0, sizeof(content));
    content.doc = doc;
    content.depth = depth;
    content.font = &doc->default_font;
    content.resources = resources;
    const PdfValue *res = deref(doc, resources, &content.res_storage);
    if (res && res->type == PV_DICT) {
        const PdfValue *fonts = deref(doc, dict_get(res, "Font"), &content.fonts_storage);
        if (fonts && fonts->type == PV_DICT) content.fonts = fonts;
        const PdfValue *xobjects = deref(doc, dict_get(res, "XObject"), &content.xobj_storage);
        if (xobjects && xobjects->type == PV_DICT) content.xobjects = xobjects;
    }
    /* A Resources reference shared with the parent is resolved again by the child; keep the
       resolved dictionary alive through res_storage while this content stream runs. */
    if (res && res != resources && res->type == PV_DICT) content.resources = res;
    PdfLexer lx;
    lexer_init(&lx, data, len, 0);
    PdfValue ops[PDF_MAX_OPERANDS];
    size_t count = 0;
    while (!doc->oom && !doc->text.truncated) {
        size_t before = lx.pos;
        PdfValue value;
        int rc = pdf_parse(&lx, &value, 0);
        if (rc == PR_EOF) break;
        if (rc == PR_OOM) { doc->oom = true; break; }
        if (rc == PR_ERROR) {
            if (lx.pos == before) ++lx.pos;
            for (size_t i = 0; i < count; ++i) pv_free(&ops[i]);
            count = 0; lx.budget = PDF_MAX_VALUE_NODES;
            continue;
        }
        if (rc == PR_VALUE) {
            if (count < PDF_MAX_OPERANDS) ops[count++] = value; else pv_free(&value);
            continue;
        }
        execute(&content, &lx, ops, count);
        for (size_t i = 0; i < count; ++i) pv_free(&ops[i]);
        count = 0;
        lx.budget = PDF_MAX_VALUE_NODES;
    }
    for (size_t i = 0; i < count; ++i) pv_free(&ops[i]);
    pv_free(&content.xobj_storage);
    pv_free(&content.fonts_storage);
    pv_free(&content.res_storage);
}

/* ------------------------------------------------------------------ pages */

static bool append_stream(PdfDoc *doc, long num, unsigned char **buffer, size_t *length) {
    unsigned char *data = NULL; size_t data_len = 0;
    int rc = stream_data(doc, num, &data, &data_len);
    if (rc == -1) return false;
    if (rc == 1) { doc->out->partial = true; asgend_memory_doc_reason(doc->out, "page content stream could not be decoded"); return true; }
    if (data_len > SIZE_MAX - *length - 2U) { free(data); return false; }
    unsigned char *grown = realloc(*buffer, *length + data_len + 2U);
    if (!grown) { free(data); return false; }
    *buffer = grown;
    memcpy(*buffer + *length, data, data_len);
    *length += data_len;
    (*buffer)[(*length)++] = '\n';
    (*buffer)[*length] = 0;
    free(data);
    return true;
}

static void process_page(PdfDoc *doc, const PdfValue *page, const PdfValue *resources) {
    if (++doc->pages > PDF_MAX_PAGES) {
        if (doc->pages == PDF_MAX_PAGES + 1U) { doc->out->partial = true; asgend_memory_doc_reason(doc->out, "page limit of %u reached", PDF_MAX_PAGES); }
        return;
    }
    const PdfValue *contents = dict_get(page, "Contents");
    unsigned char *buffer = NULL; size_t length = 0;
    bool ok = true;
    if (contents && contents->type == PV_REF) {
        long index = lookup_index(doc, contents->ref_num);
        if (index >= 0 && doc->objs[index].has_stream) ok = append_stream(doc, contents->ref_num, &buffer, &length);
        else {
            PdfValue storage;
            memset(&storage, 0, sizeof(storage));
            const PdfValue *array = deref(doc, contents, &storage);
            if (array && array->type == PV_ARRAY)
                for (size_t i = 0; ok && i < array->count; ++i)
                    if (array->items[i].type == PV_REF) ok = append_stream(doc, array->items[i].ref_num, &buffer, &length);
            pv_free(&storage);
        }
    } else if (contents && contents->type == PV_ARRAY) {
        for (size_t i = 0; ok && i < contents->count; ++i)
            if (contents->items[i].type == PV_REF) ok = append_stream(doc, contents->items[i].ref_num, &buffer, &length);
    }
    if (!ok) doc->oom = true;
    if (buffer && length) {
        doc->has_line_y = false;
        interpret(doc, buffer, length, resources, 0);
        asgend_memory_text_separator(&doc->text, 3U);
    }
    free(buffer);
}

static void walk_pages(PdfDoc *doc, const PdfValue *node_ref, const PdfValue *inherited, unsigned int depth) {
    if (doc->oom || doc->text.truncated) return;
    if (depth > PDF_MAX_TREE_DEPTH) { doc->out->partial = true; asgend_memory_doc_reason(doc->out, "page tree nesting limit reached"); return; }
    if (node_ref && node_ref->type == PV_REF) {
        long index = lookup_index(doc, node_ref->ref_num);
        if (index < 0) { doc->out->partial = true; asgend_memory_doc_reason(doc->out, "missing page tree object %ld", node_ref->ref_num); return; }
        if (doc->visited[index]) return;
        doc->visited[index] = 1;
    }
    PdfValue storage;
    memset(&storage, 0, sizeof(storage));
    const PdfValue *node = deref(doc, node_ref, &storage);
    if (!node || node->type != PV_DICT) { pv_free(&storage); return; }
    const PdfValue *resources = dict_get(node, "Resources");
    if (!resources) resources = inherited;
    const PdfValue *type = dict_get(node, "Type");
    const PdfValue *kids = dict_get(node, "Kids");
    if (name_is(type, "Pages") || (kids && !name_is(type, "Page"))) {
        PdfValue kids_storage;
        memset(&kids_storage, 0, sizeof(kids_storage));
        const PdfValue *list = deref(doc, kids, &kids_storage);
        if (list && list->type == PV_ARRAY)
            for (size_t i = 0; i < list->count && !doc->oom; ++i) walk_pages(doc, &list->items[i], resources, depth + 1U);
        pv_free(&kids_storage);
    } else {
        process_page(doc, node, resources);
    }
    pv_free(&storage);
}

static bool stream_is_auxiliary(const PdfValue *dict) {
    const PdfValue *type = dict_get(dict, "Type"), *subtype = dict_get(dict, "Subtype");
    return name_is(type, "ObjStm") || name_is(type, "XRef") || name_is(type, "Metadata") || name_is(type, "EmbeddedFile") ||
           name_is(subtype, "Image") || name_is(subtype, "XML") || name_is(subtype, "Type1C") || name_is(subtype, "CIDFontType0C") ||
           name_is(subtype, "OpenType") || dict_get(dict, "Length1") || dict_get(dict, "Length2") || dict_get(dict, "Length3");
}

static void fallback_streams(PdfDoc *doc) {
    for (size_t i = 0; i < doc->obj_count && !doc->oom && !doc->text.truncated; ++i) {
        if (!doc->objs[i].has_stream || doc->objs[i].container >= 0) continue;
        if (i > 0 && doc->objs[i - 1U].num == doc->objs[i].num) continue; /* shadowed revision */
        PdfValue dict;
        if (load_object(doc, doc->objs[i].num, &dict) != 0) continue;
        bool skip = stream_is_auxiliary(&dict);
        pv_free(&dict);
        if (skip) continue;
        unsigned char *data = NULL; size_t data_len = 0;
        if (stream_data(doc, doc->objs[i].num, &data, &data_len) != 0) { free(data); continue; }
        if (find_bytes(data, data_len, "BT", 2U) && find_bytes(data, data_len, "ET", 2U) && !find_bytes(data, data_len, "begincmap", 9U)) {
            doc->has_line_y = false;
            interpret(doc, data, data_len, NULL, 0);
            asgend_memory_text_separator(&doc->text, 3U);
        }
        free(data);
    }
}

static void emit_info_title(PdfDoc *doc) {
    if (doc->info_ref < 0) return;
    PdfValue info;
    if (load_object(doc, doc->info_ref, &info) != 0) return;
    PdfValue title_storage;
    memset(&title_storage, 0, sizeof(title_storage));
    const PdfValue *title = deref(doc, dict_get(&info, "Title"), &title_storage);
    if (title && title->type == PV_STR && title->len) {
        const unsigned char *s = title->bytes;
        size_t n = title->len;
        if (n >= 2U && s[0] == 0xfeU && s[1] == 0xffU) {
            uint16_t units[256]; size_t count = 0;
            for (size_t i = 2; i + 1U < n && count < 256U; i += 2U) units[count++] = (uint16_t)((s[i] << 8U) | s[i + 1U]);
            emit_units(doc, units, count);
        } else if (n >= 3U && s[0] == 0xefU && s[1] == 0xbbU && s[2] == 0xbfU) {
            asgend_memory_text_utf8(&doc->text, (const char *)s + 3, n - 3U);
        } else {
            for (size_t i = 0; i < n; ++i) asgend_memory_text_codepoint(&doc->text, pdfdoc_codepoint(s[i]));
        }
        asgend_memory_text_separator(&doc->text, 3U);
    }
    pv_free(&title_storage);
    pv_free(&info);
}

static bool raw_encrypt_marker(const unsigned char *data, size_t len) {
    size_t p = 0;
    while (p < len) {
        const unsigned char *hit = find_bytes(data + p, len - p, "/Encrypt", 8U);
        if (!hit) return false;
        size_t k = (size_t)(hit - data) + 8U;
        if (k >= len || !pdf_regular(data[k])) return true;
        p = k;
    }
    return false;
}

int asgend_memory_internal_extract_pdf(const unsigned char *data, size_t length,
                                       size_t max_text_bytes, AsgendMemoryDocText *out) {
    memset(out, 0, sizeof(*out));
    PdfDoc *doc = calloc(1U, sizeof(*doc));
    if (!doc) return -1;
    doc->data = data; doc->len = length; doc->out = out;
    doc->root_ref = -1; doc->info_ref = -1;
    winansi_map(doc->default_font.simple_map);
    doc->default_font.objnum = -1;
    int result = 0;
    if (asgend_memory_text_init(&doc->text, max_text_bytes) != 0) { free(doc); return -1; }
    size_t header_window = length < 1024U ? length : 1024U;
    if (!find_bytes(data, header_window, "%PDF-", 5U)) {
        out->unsupported = true;
        asgend_memory_doc_reason(out, "missing %%PDF- header");
        goto finish;
    }
    scan_objects(doc);
    if (doc->oom) { result = -1; goto finish; }
    if (doc->encrypted || (!doc->trailer_found && raw_encrypt_marker(data, length))) {
        out->unsupported = true;
        out->partial = false;
        out->reason[0] = '\0';
        asgend_memory_doc_reason(out, "encrypted");
        goto finish;
    }
    if (doc->obj_count == 0) {
        out->unsupported = true;
        asgend_memory_doc_reason(out, "no PDF objects found");
        goto finish;
    }
    qsort(doc->objs, doc->obj_count, sizeof(*doc->objs), compare_objs);
    load_object_streams(doc);
    if (doc->oom) { result = -1; goto finish; }
    qsort(doc->objs, doc->obj_count, sizeof(*doc->objs), compare_objs);
    doc->visited = calloc(doc->obj_count ? doc->obj_count : 1U, 1U);
    if (!doc->visited) { result = -1; goto finish; }
    emit_info_title(doc);
    bool walked = false;
    if (doc->root_ref >= 0) {
        PdfValue catalog;
        if (load_object(doc, doc->root_ref, &catalog) == 0) {
            const PdfValue *pages = dict_get(&catalog, "Pages");
            if (pages) { walk_pages(doc, pages, NULL, 0); walked = doc->pages > 0; }
            pv_free(&catalog);
        }
    }
    if (!walked && !doc->oom) {
        /* No usable page tree: look for a root Pages node without /Parent, then fall back to
           interpreting every content-like stream in object order. */
        for (size_t i = 0; i < doc->obj_count && !walked && !doc->oom; ++i) {
            if (i > 0 && doc->objs[i - 1U].num == doc->objs[i].num) continue;
            PdfValue dict;
            if (load_object(doc, doc->objs[i].num, &dict) != 0) continue;
            bool root_pages = name_is(dict_get(&dict, "Type"), "Pages") && !dict_get(&dict, "Parent");
            pv_free(&dict);
            if (root_pages) {
                PdfValue ref;
                memset(&ref, 0, sizeof(ref));
                ref.type = PV_REF; ref.ref_num = doc->objs[i].num;
                walk_pages(doc, &ref, NULL, 0);
                walked = doc->pages > 0;
            }
        }
        if (!walked && !doc->oom) {
            out->partial = true;
            asgend_memory_doc_reason(out, "page tree not found; scanned content streams in object order");
            fallback_streams(doc);
        }
    }
    if (doc->oom) { result = -1; goto finish; }
    if (doc->unmapped_codes > 0) {
        out->partial = true;
        asgend_memory_doc_reason(out, "%llu glyph codes had no Unicode mapping (font without ToUnicode)",
                                 (unsigned long long)doc->unmapped_codes);
    }
finish:
    for (size_t i = 0; i < doc->font_count; ++i) font_free(&doc->fonts[i]);
    free(doc->fonts);
    font_free(&doc->scratch_font);
    for (size_t i = 0; i < doc->buf_count; ++i) free(doc->bufs[i].data);
    free(doc->bufs);
    free(doc->objs);
    free(doc->visited);
    if (result == 0) {
        if (asgend_memory_text_finish(&doc->text, out) != 0) result = -1;
    } else asgend_memory_text_free(&doc->text);
    free(doc);
    if (result == 0 && !out->unsupported && out->text_len == 0) {
        out->unsupported = true;
        if (!out->reason[0]) asgend_memory_doc_reason(out, "no extractable text (image-only or empty PDF)");
    }
    return result;
}
