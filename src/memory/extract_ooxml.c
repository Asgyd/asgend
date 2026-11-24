#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

/* Bounded Office Open XML (DOCX/XLSX/PPTX) text extraction.
 * The ZIP central directory is authoritative for sizes, but local headers are cross-checked.
 * No archive member is written to disk; XML DTDs and external entities are never expanded. */
#define OOXML_MAX_ENTRIES 4096U
#define OOXML_MAX_ENTRY_BYTES (16U * 1024U * 1024U)
#define OOXML_MAX_TOTAL_XML_BYTES (64U * 1024U * 1024U)
#define OOXML_MAX_SHARED_STRINGS 200000U
#define OOXML_MAX_SHARED_TEXT_BYTES (16U * 1024U * 1024U)
#define OOXML_MAX_NAME_BYTES 1024U

typedef struct {
    char *name;
    uint16_t flags;
    uint16_t method;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_offset;
    bool duplicate;
} ZipEntry;

typedef enum { OOXML_UNKNOWN = 0, OOXML_DOCX, OOXML_XLSX, OOXML_PPTX } OoxmlKind;

typedef struct {
    char **items;
    size_t *lengths;
    size_t count;
    size_t capacity;
    size_t total_bytes;
} SharedStrings;

static uint16_t get_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static bool range_ok(size_t length, uint64_t offset, uint64_t bytes) {
    return offset <= (uint64_t)length && bytes <= (uint64_t)length - offset;
}

static void free_entries(ZipEntry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; ++i) free(entries[i].name);
    free(entries);
}

static int parse_central_directory(const unsigned char *data, size_t length,
                                   ZipEntry **entries_out, size_t *count_out,
                                   AsgendMemoryDocText *out) {
    *entries_out = NULL;
    *count_out = 0;
    if (length < 22U) {
        asgend_memory_doc_reason(out, "not a valid OOXML ZIP package (missing ZIP trailer)");
        return 1;
    }
    size_t lower = length > 65557U ? length - 65557U : 0U;
    size_t eocd = SIZE_MAX;
    for (size_t pos = length - 22U;; --pos) {
        if (get_u32(data + pos) == 0x06054b50U) {
            uint16_t comment_len = get_u16(data + pos + 20U);
            if (range_ok(length, pos, 22U + comment_len) && pos + 22U + comment_len == length) {
                eocd = pos;
                break;
            }
        }
        if (pos == lower) break;
    }
    if (eocd == SIZE_MAX) {
        asgend_memory_doc_reason(out, "not a valid OOXML ZIP package (central directory trailer missing)");
        return 1;
    }
    uint16_t disk = get_u16(data + eocd + 4U);
    uint16_t cd_disk = get_u16(data + eocd + 6U);
    uint16_t disk_count = get_u16(data + eocd + 8U);
    uint16_t total_count = get_u16(data + eocd + 10U);
    uint32_t cd_size = get_u32(data + eocd + 12U);
    uint32_t cd_offset = get_u32(data + eocd + 16U);
    if (disk != 0U || cd_disk != 0U || disk_count != total_count ||
        total_count == 0xffffU || cd_size == 0xffffffffU || cd_offset == 0xffffffffU) {
        asgend_memory_doc_reason(out, "multi-disk and ZIP64 OOXML packages are unsupported");
        return 1;
    }
    if (total_count == 0U || total_count > OOXML_MAX_ENTRIES ||
        !range_ok(length, cd_offset, cd_size) || (uint64_t)cd_offset + cd_size > eocd) {
        asgend_memory_doc_reason(out, "OOXML ZIP central directory exceeds safety limits or is malformed");
        return 1;
    }
    ZipEntry *entries = calloc(total_count, sizeof(*entries));
    if (!entries) return -1;
    size_t pos = cd_offset;
    size_t count = 0;
    for (; count < total_count; ++count) {
        if (!range_ok(length, pos, 46U) || get_u32(data + pos) != 0x02014b50U) {
            asgend_memory_doc_reason(out, "malformed OOXML ZIP central directory entry");
            free_entries(entries, count);
            return 1;
        }
        uint16_t flags = get_u16(data + pos + 8U);
        uint16_t method = get_u16(data + pos + 10U);
        uint32_t crc = get_u32(data + pos + 16U);
        uint32_t compressed = get_u32(data + pos + 20U);
        uint32_t uncompressed = get_u32(data + pos + 24U);
        uint16_t name_len = get_u16(data + pos + 28U);
        uint16_t extra_len = get_u16(data + pos + 30U);
        uint16_t comment_len = get_u16(data + pos + 32U);
        uint16_t disk_start = get_u16(data + pos + 34U);
        uint32_t local_offset = get_u32(data + pos + 42U);
        uint64_t record_len = 46U + (uint64_t)name_len + extra_len + comment_len;
        if (name_len == 0U || name_len > OOXML_MAX_NAME_BYTES || disk_start != 0U ||
            compressed == 0xffffffffU || uncompressed == 0xffffffffU || local_offset == 0xffffffffU ||
            !range_ok(length, pos, record_len)) {
            asgend_memory_doc_reason(out, "unsupported or malformed OOXML ZIP entry metadata");
            free_entries(entries, count);
            return 1;
        }
        const unsigned char *name = data + pos + 46U;
        if (memchr(name, '\0', name_len) != NULL) {
            asgend_memory_doc_reason(out, "OOXML ZIP entry contains an invalid NUL byte in its name");
            free_entries(entries, count);
            return 1;
        }
        entries[count].name = malloc((size_t)name_len + 1U);
        if (!entries[count].name) { free_entries(entries, count); return -1; }
        memcpy(entries[count].name, name, name_len);
        entries[count].name[name_len] = '\0';
        entries[count].flags = flags;
        entries[count].method = method;
        entries[count].crc32 = crc;
        entries[count].compressed_size = compressed;
        entries[count].uncompressed_size = uncompressed;
        entries[count].local_offset = local_offset;
        for (size_t j = 0; j < count; ++j) {
            if (strcmp(entries[j].name, entries[count].name) == 0) {
                entries[j].duplicate = true;
                entries[count].duplicate = true;
            }
        }
        pos += (size_t)record_len;
    }
    if (pos != (size_t)cd_offset + cd_size) {
        asgend_memory_doc_reason(out, "OOXML ZIP central directory size does not match its entries");
        free_entries(entries, count);
        return 1;
    }
    *entries_out = entries;
    *count_out = count;
    return 0;
}

static bool entry_name_is(const ZipEntry *entry, const char *name) {
    return entry && entry->name && strcmp(entry->name, name) == 0;
}

static bool entry_has_prefix_suffix(const ZipEntry *entry, const char *prefix, const char *suffix) {
    if (!entry || !entry->name) return false;
    size_t n = strlen(entry->name), p = strlen(prefix), s = strlen(suffix);
    return n > p + s && strncmp(entry->name, prefix, p) == 0 && strcmp(entry->name + n - s, suffix) == 0;
}

static bool entry_is_sheet(const ZipEntry *entry) {
    if (!entry_has_prefix_suffix(entry, "xl/worksheets/sheet", ".xml")) return false;
    const char *p = entry->name + strlen("xl/worksheets/sheet");
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) ++p;
    return strcmp(p, ".xml") == 0;
}

static bool entry_is_doc_part(const ZipEntry *entry) {
    return entry_name_is(entry, "word/document.xml") ||
           entry_has_prefix_suffix(entry, "word/header", ".xml") ||
           entry_has_prefix_suffix(entry, "word/footer", ".xml") ||
           entry_name_is(entry, "word/footnotes.xml") || entry_name_is(entry, "word/endnotes.xml") ||
           entry_name_is(entry, "word/comments.xml");
}

static bool entry_is_slide_part(const ZipEntry *entry) {
    if (entry_has_prefix_suffix(entry, "ppt/slides/slide", ".xml")) {
        const char *p = entry->name + strlen("ppt/slides/slide");
        if (isdigit((unsigned char)*p)) { while (isdigit((unsigned char)*p)) ++p; if (strcmp(p, ".xml") == 0) return true; }
    }
    if (entry_has_prefix_suffix(entry, "ppt/notesSlides/notesSlide", ".xml")) {
        const char *p = entry->name + strlen("ppt/notesSlides/notesSlide");
        if (isdigit((unsigned char)*p)) { while (isdigit((unsigned char)*p)) ++p; if (strcmp(p, ".xml") == 0) return true; }
    }
    return false;
}

static OoxmlKind detect_kind(const ZipEntry *entries, size_t count) {
    bool content_types = false, doc = false, workbook = false, presentation = false;
    for (size_t i = 0; i < count; ++i) {
        if (entry_name_is(&entries[i], "[Content_Types].xml")) content_types = true;
        if (entry_name_is(&entries[i], "word/document.xml")) doc = true;
        if (entry_name_is(&entries[i], "xl/workbook.xml")) workbook = true;
        if (entry_name_is(&entries[i], "ppt/presentation.xml")) presentation = true;
    }
    if (!content_types) return OOXML_UNKNOWN;
    if (workbook && !doc && !presentation) return OOXML_XLSX;
    if (doc && !workbook && !presentation) return OOXML_DOCX;
    if (presentation && !workbook && !doc) return OOXML_PPTX;
    return OOXML_UNKNOWN;
}

static int read_zip_entry(const unsigned char *archive, size_t archive_len, const ZipEntry *entry,
                          unsigned char **output, size_t *output_len, bool *limit_or_bad,
                          AsgendMemoryDocText *doc) {
    *output = NULL; *output_len = 0; *limit_or_bad = false;
    if (entry->duplicate) {
        *limit_or_bad = true;
        asgend_memory_doc_reason(doc, "duplicate OOXML ZIP part ignored");
        return 1;
    }
    if ((entry->flags & 1U) != 0U) {
        *limit_or_bad = true;
        asgend_memory_doc_reason(doc, "encrypted OOXML ZIP part is unsupported");
        return 1;
    }
    if ((entry->flags & 0x2040U) != 0U || (entry->method != 0U && entry->method != 8U)) {
        *limit_or_bad = true;
        asgend_memory_doc_reason(doc, "unsupported ZIP compression or strong-encryption flags in OOXML part");
        return 1;
    }
    if (entry->uncompressed_size > OOXML_MAX_ENTRY_BYTES || entry->compressed_size > archive_len ||
        entry->uncompressed_size > OOXML_MAX_TOTAL_XML_BYTES) {
        *limit_or_bad = true;
        asgend_memory_doc_reason(doc, "OOXML XML part exceeds the 16 MiB decompression limit");
        return 1;
    }
    if (!range_ok(archive_len, entry->local_offset, 30U) ||
        get_u32(archive + entry->local_offset) != 0x04034b50U) {
        *limit_or_bad = true;
        asgend_memory_doc_reason(doc, "malformed OOXML local ZIP header");
        return 1;
    }
    const unsigned char *local = archive + entry->local_offset;
    uint16_t local_flags = get_u16(local + 6U), local_method = get_u16(local + 8U);
    uint16_t local_name_len = get_u16(local + 26U), local_extra_len = get_u16(local + 28U);
    if (local_flags != entry->flags || local_method != entry->method || local_name_len == 0U ||
        !range_ok(archive_len, (uint64_t)entry->local_offset + 30U,
                  (uint64_t)local_name_len + local_extra_len + entry->compressed_size) ||
        local_name_len != strlen(entry->name) ||
        memcmp(local + 30U, entry->name, local_name_len) != 0) {
        *limit_or_bad = true;
        asgend_memory_doc_reason(doc, "OOXML local ZIP header conflicts with central directory");
        return 1;
    }
    const unsigned char *compressed = local + 30U + local_name_len + local_extra_len;
    size_t expected = entry->uncompressed_size;
    unsigned char *plain = malloc(expected + 1U);
    if (!plain) return -1;
    size_t actual = expected;
    bool valid = true;
    if (entry->method == 0U) {
        if (entry->compressed_size != entry->uncompressed_size) valid = false;
        else if (expected) memcpy(plain, compressed, expected);
    } else {
        z_stream stream;
        memset(&stream, 0, sizeof(stream));
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) { free(plain); return -1; }
        stream.next_in = (Bytef *)(uintptr_t)compressed;
        stream.avail_in = entry->compressed_size;
        stream.next_out = plain;
        stream.avail_out = (uInt)(expected + 1U);
        int zrc = Z_OK;
        while (zrc == Z_OK) {
            zrc = inflate(&stream, Z_NO_FLUSH);
            if (zrc == Z_OK && stream.avail_out == 0U) break;
        }
        actual = (size_t)stream.total_out;
        valid = zrc == Z_STREAM_END && actual == expected && stream.total_in == entry->compressed_size;
        inflateEnd(&stream);
    }
    if (valid) {
        uLong crc = crc32(0L, Z_NULL, 0);
        size_t pos = 0;
        while (pos < actual) {
            uInt take = (uInt)((actual - pos) > UINT_MAX ? UINT_MAX : (actual - pos));
            crc = crc32(crc, plain + pos, take);
            pos += take;
        }
        valid = (uint32_t)crc == entry->crc32;
    }
    if (!valid) {
        free(plain);
        *limit_or_bad = true;
        asgend_memory_doc_reason(doc, "corrupt or size-mismatched OOXML ZIP part");
        return 1;
    }
    plain[actual] = 0U;
    *output = plain;
    *output_len = actual;
    return 0;
}

static bool xml_name_char(unsigned char c) {
    return isalnum(c) || c == '_' || c == ':' || c == '-' || c == '.';
}

static void append_xml_entity(AsgendMemoryTextBuffer *buffer, const unsigned char *text,
                              size_t length, bool *partial) {
    if (length == 2U && text[0] == 'l' && text[1] == 't') { asgend_memory_text_codepoint(buffer, '<'); return; }
    if (length == 2U && text[0] == 'g' && text[1] == 't') { asgend_memory_text_codepoint(buffer, '>'); return; }
    if (length == 3U && memcmp(text, "amp", 3U) == 0) { asgend_memory_text_codepoint(buffer, '&'); return; }
    if (length == 4U && memcmp(text, "quot", 4U) == 0) { asgend_memory_text_codepoint(buffer, '"'); return; }
    if (length == 4U && memcmp(text, "apos", 4U) == 0) { asgend_memory_text_codepoint(buffer, '\''); return; }
    if (length >= 2U && text[0] == '#') {
        uint32_t codepoint = 0U;
        size_t i = 1U;
        unsigned int base = 10U;
        if (i < length && (text[i] == 'x' || text[i] == 'X')) { base = 16U; ++i; }
        bool any = false;
        for (; i < length; ++i) {
            unsigned char c = text[i];
            unsigned int digit;
            if (c >= '0' && c <= '9') digit = (unsigned int)(c - '0');
            else if (base == 16U && c >= 'a' && c <= 'f') digit = (unsigned int)(c - 'a' + 10);
            else if (base == 16U && c >= 'A' && c <= 'F') digit = (unsigned int)(c - 'A' + 10);
            else { any = false; break; }
            if (digit >= base || codepoint > (0x10ffffU - digit) / base) { any = false; break; }
            codepoint = codepoint * base + digit;
            any = true;
        }
        if (any) { asgend_memory_text_codepoint(buffer, codepoint); return; }
    }
    /* Custom entities would require DTD processing. Never expand them. */
    if (partial) *partial = true;
}

static void append_xml_decoded(AsgendMemoryTextBuffer *buffer, const unsigned char *text,
                               size_t length, bool *partial) {
    size_t i = 0U;
    while (i < length && !buffer->truncated && !buffer->oom) {
        if (text[i] != '&') {
            size_t start = i++;
            while (i < length && text[i] != '&') ++i;
            asgend_memory_text_utf8(buffer, (const char *)text + start, i - start);
            continue;
        }
        size_t semi = i + 1U;
        while (semi < length && semi - i <= 16U && text[semi] != ';' && text[semi] != '<' && text[semi] != '&') ++semi;
        if (semi < length && text[semi] == ';' && semi - i <= 16U) {
            append_xml_entity(buffer, text + i + 1U, semi - i - 1U, partial);
            i = semi + 1U;
        } else {
            if (partial) *partial = true;
            asgend_memory_text_codepoint(buffer, '&');
            ++i;
        }
    }
}

static bool tag_local_name(const unsigned char *tag, size_t tag_len,
                           char *local, size_t local_size, bool *closing, bool *self_closing) {
    size_t i = 0U;
    *closing = false; *self_closing = false;
    while (i < tag_len && isspace(tag[i])) ++i;
    if (i < tag_len && tag[i] == '/') { *closing = true; ++i; }
    while (i < tag_len && isspace(tag[i])) ++i;
    size_t start = i;
    while (i < tag_len && xml_name_char(tag[i])) ++i;
    if (i == start || i - start >= local_size) return false;
    size_t colon = start;
    for (size_t j = start; j < i; ++j) if (tag[j] == ':') colon = j + 1U;
    size_t n = i - colon;
    if (n == 0U || n >= local_size) return false;
    memcpy(local, tag + colon, n); local[n] = '\0';
    for (size_t j = i; j < tag_len; ++j) {
        if (tag[j] == '/' && j + 1U == tag_len) *self_closing = true;
    }
    return true;
}

static size_t find_subsequence(const unsigned char *data, size_t length, size_t from,
                               const char *needle) {
    size_t n = strlen(needle);
    if (n == 0U || from > length || n > length - from) return SIZE_MAX;
    for (size_t i = from; i <= length - n; ++i) if (memcmp(data + i, needle, n) == 0) return i;
    return SIZE_MAX;
}

/* Extract Word/PowerPoint text nodes. XML declarations, comments, DTDs and attributes are ignored. */
static void xml_extract_t_nodes(const unsigned char *xml, size_t length,
                                AsgendMemoryTextBuffer *buffer, bool *partial) {
    bool in_text = false;
    size_t i = 0U;
    while (i < length && !buffer->truncated && !buffer->oom) {
        if (xml[i] != '<') {
            size_t start = i++;
            while (i < length && xml[i] != '<') ++i;
            if (in_text) append_xml_decoded(buffer, xml + start, i - start, partial);
            continue;
        }
        if (i + 4U <= length && memcmp(xml + i, "<!--", 4U) == 0) {
            size_t end = find_subsequence(xml, length, i + 4U, "-->");
            if (end == SIZE_MAX) { *partial = true; break; }
            i = end + 3U; continue;
        }
        if (i + 9U <= length && memcmp(xml + i, "<![CDATA[", 9U) == 0) {
            size_t end = find_subsequence(xml, length, i + 9U, "]]>");
            if (end == SIZE_MAX) { *partial = true; break; }
            if (in_text) asgend_memory_text_utf8(buffer, (const char *)xml + i + 9U, end - i - 9U);
            i = end + 3U; continue;
        }
        if (i + 2U <= length && (xml[i + 1U] == '?' || xml[i + 1U] == '!')) {
            /* Skip processing instructions and declarations; do not process DTD entities. */
            size_t end = i + 2U;
            unsigned char quote = 0U;
            int bracket = 0;
            for (; end < length; ++end) {
                unsigned char c = xml[end];
                if (quote) { if (c == quote) quote = 0U; continue; }
                if (c == '\'' || c == '"') { quote = c; continue; }
                if (c == '[') ++bracket;
                else if (c == ']' && bracket > 0) --bracket;
                else if (c == '>' && bracket == 0) break;
            }
            if (end >= length) { *partial = true; break; }
            if (xml[i + 1U] == '!') *partial = true;
            i = end + 1U; continue;
        }
        size_t end = i + 1U;
        unsigned char quote = 0U;
        for (; end < length; ++end) {
            unsigned char c = xml[end];
            if (quote) { if (c == quote) quote = 0U; continue; }
            if (c == '\'' || c == '"') { quote = c; continue; }
            if (c == '>') break;
        }
        if (end >= length) { *partial = true; break; }
        char local[32]; bool closing, self_closing;
        if (tag_local_name(xml + i + 1U, end - i - 1U, local, sizeof(local), &closing, &self_closing)) {
            if (!closing && !strcmp(local, "t") && !self_closing) in_text = true;
            else if (closing && !strcmp(local, "t")) in_text = false;
            if (!closing && (!strcmp(local, "p") || !strcmp(local, "tr") || !strcmp(local, "sp")))
                asgend_memory_text_separator(buffer, 2U);
            if (!closing && (!strcmp(local, "tab") || !strcmp(local, "br")))
                asgend_memory_text_separator(buffer, !strcmp(local, "tab") ? 1U : 2U);
            if (closing && (!strcmp(local, "p") || !strcmp(local, "tr") || !strcmp(local, "sp")))
                asgend_memory_text_separator(buffer, 2U);
        }
        i = end + 1U;
    }
    if (in_text) *partial = true;
}

static void free_shared_strings(SharedStrings *strings) {
    if (!strings) return;
    for (size_t i = 0; i < strings->count; ++i) free(strings->items[i]);
    free(strings->items); free(strings->lengths);
    memset(strings, 0, sizeof(*strings));
}

static int push_shared_string(SharedStrings *strings, char *value, size_t length) {
    if (strings->count >= OOXML_MAX_SHARED_STRINGS || length > OOXML_MAX_SHARED_TEXT_BYTES - strings->total_bytes) return 1;
    if (strings->count == strings->capacity) {
        size_t next = strings->capacity ? strings->capacity * 2U : 128U;
        if (next > OOXML_MAX_SHARED_STRINGS) next = OOXML_MAX_SHARED_STRINGS;
        char **new_items = realloc(strings->items, next * sizeof(*new_items));
        if (!new_items) return -1;
        strings->items = new_items;
        size_t *new_lengths = realloc(strings->lengths, next * sizeof(*new_lengths));
        if (!new_lengths) return -1;
        strings->lengths = new_lengths;
        strings->capacity = next;
    }
    strings->items[strings->count] = value;
    strings->lengths[strings->count] = length;
    strings->count++;
    strings->total_bytes += length;
    return 0;
}

static int parse_shared_strings(const unsigned char *xml, size_t length, SharedStrings *strings,
                                bool *partial, AsgendMemoryDocText *doc) {
    size_t pos = 0U;
    while (pos < length) {
        size_t start = find_subsequence(xml, length, pos, "<si");
        if (start == SIZE_MAX) break;
        if (start + 3U < length && !(isspace(xml[start + 3U]) || xml[start + 3U] == '>' || xml[start + 3U] == '/')) {
            pos = start + 3U; continue;
        }
        size_t gt = find_subsequence(xml, length, start, ">");
        size_t end = gt == SIZE_MAX ? SIZE_MAX : find_subsequence(xml, length, gt + 1U, "</si>");
        if (gt == SIZE_MAX || end == SIZE_MAX) {
            *partial = true;
            asgend_memory_doc_reason(doc, "malformed XLSX sharedStrings.xml");
            break;
        }
        AsgendMemoryTextBuffer one;
        if (asgend_memory_text_init(&one, OOXML_MAX_SHARED_TEXT_BYTES) != 0) return -1;
        bool one_partial = false;
        xml_extract_t_nodes(xml + gt + 1U, end - gt - 1U, &one, &one_partial);
        AsgendMemoryDocText finished = {0};
        if (asgend_memory_text_finish(&one, &finished) != 0) return -1;
        if (one_partial) *partial = true;
        int rc = push_shared_string(strings, finished.text, finished.text_len);
        if (rc != 0) {
            free(finished.text);
            if (rc < 0) return -1;
            *partial = true;
            asgend_memory_doc_reason(doc, "XLSX shared-string table exceeds safety limits");
            return 0;
        }
        pos = end + 5U;
    }
    return 0;
}

static bool get_attr_value(const unsigned char *tag, size_t length, const char *wanted,
                           const unsigned char **value, size_t *value_len) {
    size_t i = 0U;
    while (i < length && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\r' && tag[i] != '\n') ++i;
    while (i < length) {
        while (i < length && isspace(tag[i])) ++i;
        size_t start = i;
        while (i < length && xml_name_char(tag[i])) ++i;
        if (i == start) { ++i; continue; }
        size_t name_len = i - start;
        while (i < length && isspace(tag[i])) ++i;
        if (i >= length || tag[i] != '=') {
            while (i < length && !isspace(tag[i])) ++i;
            continue;
        }
        ++i; while (i < length && isspace(tag[i])) ++i;
        if (i >= length || (tag[i] != '\'' && tag[i] != '"')) return false;
        unsigned char quote = tag[i++];
        size_t val_start = i;
        while (i < length && tag[i] != quote) ++i;
        if (i >= length) return false;
        bool match = (name_len == strlen(wanted) && memcmp(tag + start, wanted, name_len) == 0);
        if (match) { *value = tag + val_start; *value_len = i - val_start; return true; }
        ++i;
    }
    return false;
}

static size_t find_xml_element_content(const unsigned char *xml, size_t length, size_t from,
                                       const char *element, size_t *content_start, size_t *content_end) {
    char opening[40], closing[40];
    (void)snprintf(opening, sizeof(opening), "<%s", element);
    (void)snprintf(closing, sizeof(closing), "</%s>", element);
    size_t start = find_subsequence(xml, length, from, opening);
    if (start == SIZE_MAX) return SIZE_MAX;
    size_t gt = find_subsequence(xml, length, start, ">");
    size_t end = gt == SIZE_MAX ? SIZE_MAX : find_subsequence(xml, length, gt + 1U, closing);
    if (gt == SIZE_MAX || end == SIZE_MAX) return SIZE_MAX;
    *content_start = gt + 1U;
    *content_end = end;
    return start;
}

static void append_decoded_trimmed(AsgendMemoryTextBuffer *buffer, const unsigned char *text,
                                   size_t length, bool *partial) {
    while (length && isspace(text[0])) { ++text; --length; }
    while (length && isspace(text[length - 1U])) --length;
    append_xml_decoded(buffer, text, length, partial);
}

static void append_xlsx_cell(const unsigned char *cell, size_t length, const SharedStrings *strings,
                             AsgendMemoryTextBuffer *buffer, bool *partial) {
    const unsigned char *type = NULL; size_t type_len = 0U;
    (void)get_attr_value(cell, length, "t", &type, &type_len);
    size_t content_start = 0U, content_end = 0U;
    if (type && type_len == 1U && type[0] == 's') {
        if (find_xml_element_content(cell, length, 0U, "v", &content_start, &content_end) == SIZE_MAX) {
            *partial = true; return;
        }
        while (content_start < content_end && isspace(cell[content_start])) ++content_start;
        while (content_end > content_start && isspace(cell[content_end - 1U])) --content_end;
        size_t index = 0U;
        if (content_start == content_end) { *partial = true; return; }
        for (size_t i = content_start; i < content_end; ++i) {
            if (!isdigit(cell[i]) || index > (SIZE_MAX - 9U) / 10U) { *partial = true; return; }
            index = index * 10U + (size_t)(cell[i] - '0');
        }
        if (index >= strings->count) { *partial = true; return; }
        asgend_memory_text_utf8(buffer, strings->items[index], strings->lengths[index]);
        return;
    }
    if (type && type_len == 9U && memcmp(type, "inlineStr", 9U) == 0) {
        xml_extract_t_nodes(cell, length, buffer, partial);
        return;
    }
    if (type && type_len == 3U && memcmp(type, "str", 3U) == 0) {
        if (find_xml_element_content(cell, length, 0U, "v", &content_start, &content_end) != SIZE_MAX)
            append_decoded_trimmed(buffer, cell + content_start, content_end - content_start, partial);
        return;
    }
    if (find_xml_element_content(cell, length, 0U, "v", &content_start, &content_end) != SIZE_MAX) {
        /* Numeric and boolean cells are useful searchable facts too. */
        append_decoded_trimmed(buffer, cell + content_start, content_end - content_start, partial);
    }
}

static void parse_xlsx_sheet(const unsigned char *xml, size_t length, const SharedStrings *strings,
                             AsgendMemoryTextBuffer *buffer, bool *partial) {
    size_t row_pos = 0U;
    bool saw_row = false;
    while (row_pos < length && !buffer->truncated && !buffer->oom) {
        size_t row_start = find_subsequence(xml, length, row_pos, "<row");
        if (row_start == SIZE_MAX) break;
        if (row_start + 4U < length && !(isspace(xml[row_start + 4U]) || xml[row_start + 4U] == '>' || xml[row_start + 4U] == '/')) {
            row_pos = row_start + 4U; continue;
        }
        size_t row_gt = find_subsequence(xml, length, row_start, ">");
        size_t row_end = row_gt == SIZE_MAX ? SIZE_MAX : find_subsequence(xml, length, row_gt + 1U, "</row>");
        if (row_gt == SIZE_MAX || row_end == SIZE_MAX) { *partial = true; break; }
        saw_row = true;
        size_t pos = row_gt + 1U;
        bool has_cell = false;
        while (pos < row_end && !buffer->truncated && !buffer->oom) {
            size_t cell_start = find_subsequence(xml, row_end, pos, "<c");
            if (cell_start == SIZE_MAX) break;
            if (cell_start + 2U < row_end && !(isspace(xml[cell_start + 2U]) || xml[cell_start + 2U] == '>' || xml[cell_start + 2U] == '/')) {
                pos = cell_start + 2U; continue;
            }
            size_t cell_gt = find_subsequence(xml, row_end, cell_start, ">");
            if (cell_gt == SIZE_MAX) { *partial = true; break; }
            size_t cell_end = find_subsequence(xml, row_end, cell_gt + 1U, "</c>");
            if (cell_end == SIZE_MAX) {
                /* Empty/self-closing cells are allowed. */
                pos = cell_gt + 1U; continue;
            }
            if (has_cell) asgend_memory_text_separator(buffer, 1U);
            append_xlsx_cell(xml + cell_start + 1U, cell_end - cell_start - 1U, strings, buffer, partial);
            has_cell = true;
            pos = cell_end + 4U;
        }
        if (has_cell) asgend_memory_text_separator(buffer, 2U);
        row_pos = row_end + 6U;
    }
    if (!saw_row) *partial = true;
}

static int process_part(const unsigned char *archive, size_t archive_len, const ZipEntry *entry,
                        size_t *total_xml_bytes, AsgendMemoryTextBuffer *buffer,
                        bool *partial, AsgendMemoryDocText *doc) {
    if (entry->uncompressed_size > OOXML_MAX_TOTAL_XML_BYTES - *total_xml_bytes) {
        *partial = true;
        asgend_memory_doc_reason(doc, "total OOXML XML decompression budget of 64 MiB exceeded");
        return 1;
    }
    unsigned char *xml = NULL; size_t xml_len = 0U; bool bad = false;
    int rc = read_zip_entry(archive, archive_len, entry, &xml, &xml_len, &bad, doc);
    if (rc != 0) { if (bad) *partial = true; return rc; }
    *total_xml_bytes += xml_len;
    asgend_memory_text_separator(buffer, 3U);
    xml_extract_t_nodes(xml, xml_len, buffer, partial);
    free(xml);
    return 0;
}

int asgend_memory_internal_extract_ooxml(const unsigned char *data, size_t length,
                                         size_t max_text_bytes, AsgendMemoryDocText *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!data || length == 0U || length > ASGEND_MEMORY_DOC_MAX_INPUT) {
        out->unsupported = true;
        asgend_memory_doc_reason(out, "OOXML input is empty or exceeds the 64 MiB input limit");
        return 0;
    }
    ZipEntry *entries = NULL; size_t entry_count = 0U;
    int zip_rc = parse_central_directory(data, length, &entries, &entry_count, out);
    if (zip_rc < 0) return -1;
    if (zip_rc != 0) { out->unsupported = true; return 0; }
    OoxmlKind kind = detect_kind(entries, entry_count);
    if (kind == OOXML_UNKNOWN) {
        free_entries(entries, entry_count);
        out->unsupported = true;
        asgend_memory_doc_reason(out, "ZIP archive is not a supported DOCX, XLSX, or PPTX package");
        return 0;
    }
    AsgendMemoryTextBuffer buffer;
    if (asgend_memory_text_init(&buffer, max_text_bytes) != 0) { free_entries(entries, entry_count); return -1; }
    bool partial = false;
    size_t total_xml_bytes = 0U;
    if (kind == OOXML_XLSX) {
        SharedStrings shared = {0};
        for (size_t i = 0; i < entry_count; ++i) {
            if (!entry_name_is(&entries[i], "xl/sharedStrings.xml")) continue;
            if (entries[i].uncompressed_size > OOXML_MAX_ENTRY_BYTES) {
                partial = true; asgend_memory_doc_reason(out, "XLSX sharedStrings.xml exceeds the 16 MiB limit"); break;
            }
            unsigned char *xml = NULL; size_t xml_len = 0U; bool bad = false;
            int rc = read_zip_entry(data, length, &entries[i], &xml, &xml_len, &bad, out);
            if (rc != 0) { if (bad) partial = true; break; }
            total_xml_bytes += xml_len;
            if (parse_shared_strings(xml, xml_len, &shared, &partial, out) != 0) { free(xml); free_shared_strings(&shared); asgend_memory_text_free(&buffer); free_entries(entries, entry_count); return -1; }
            free(xml);
            break;
        }
        bool had_sheet = false;
        for (size_t i = 0; i < entry_count && !buffer.truncated && !buffer.oom; ++i) {
            if (!entry_is_sheet(&entries[i])) continue;
            had_sheet = true;
            if (entries[i].uncompressed_size > OOXML_MAX_ENTRY_BYTES ||
                entries[i].uncompressed_size > OOXML_MAX_TOTAL_XML_BYTES - total_xml_bytes) {
                partial = true; asgend_memory_doc_reason(out, "XLSX worksheet XML exceeds decompression safety limits"); continue;
            }
            unsigned char *xml = NULL; size_t xml_len = 0U; bool bad = false;
            int rc = read_zip_entry(data, length, &entries[i], &xml, &xml_len, &bad, out);
            if (rc != 0) { if (bad) partial = true; continue; }
            total_xml_bytes += xml_len;
            asgend_memory_text_separator(&buffer, 3U);
            parse_xlsx_sheet(xml, xml_len, &shared, &buffer, &partial);
            free(xml);
        }
        if (!had_sheet) { partial = true; asgend_memory_doc_reason(out, "XLSX package has no readable worksheets"); }
        free_shared_strings(&shared);
    } else {
        bool found_main = false;
        for (size_t i = 0; i < entry_count && !buffer.truncated && !buffer.oom; ++i) {
            bool selected = kind == OOXML_DOCX ? entry_is_doc_part(&entries[i]) : entry_is_slide_part(&entries[i]);
            if (!selected) continue;
            if (kind == OOXML_DOCX && entry_name_is(&entries[i], "word/document.xml")) found_main = true;
            if (kind == OOXML_PPTX && entry_has_prefix_suffix(&entries[i], "ppt/slides/slide", ".xml")) found_main = true;
            (void)process_part(data, length, &entries[i], &total_xml_bytes, &buffer, &partial, out);
        }
        if (!found_main) { partial = true; asgend_memory_doc_reason(out, "OOXML package has no main document/slide part"); }
    }
    free_entries(entries, entry_count);
    if (buffer.oom) { asgend_memory_text_free(&buffer); return -1; }
    if (asgend_memory_text_finish(&buffer, out) != 0) return -1;
    out->partial = partial;
    if (!out->text_len) {
        free(out->text); out->text = NULL;
        out->unsupported = true;
        if (!out->reason[0]) asgend_memory_doc_reason(out, "no extractable text found in OOXML document");
        return 0;
    }
    if (out->truncated && !out->reason[0]) asgend_memory_doc_reason(out, "extracted text reached configured limit");
    if (partial && !out->reason[0]) asgend_memory_doc_reason(out, "some OOXML parts could not be extracted");
    return 0;
}
