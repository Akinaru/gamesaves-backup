#include "zip.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#define DOS_DATE_1980_01_01 0x0021
#define MAX_ENTRIES 4096

typedef struct {
    char *names[MAX_ENTRIES];
    size_t count;
    int64_t latest_mtime;
} FileList;

static bool list_files(const char *root, const char *rel, FileList *list) {
    char path[1024];
    snprintf(path, sizeof path, "%s%s", root, rel);
    DIR *dir = opendir(path);
    if (!dir) return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child_rel[1024];
        snprintf(child_rel, sizeof child_rel, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        char child[1024];
        snprintf(child, sizeof child, "%s%s", root, child_rel);
        struct stat st;
        if (stat(child, &st) != 0) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            ok = list_files(root, child_rel, list);
        } else {
            if (list->count >= MAX_ENTRIES) { ok = false; break; }
            list->names[list->count++] = strdup(child_rel);
            if (st.st_mtime > list->latest_mtime) list->latest_mtime = st.st_mtime;
        }
    }
    closedir(dir);
    return ok;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool put16(Buf *b, uint16_t v) {
    uint8_t x[2] = {v & 0xFF, v >> 8};
    return buf_append(b, x, 2);
}

static bool put32(Buf *b, uint32_t v) {
    uint8_t x[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24};
    return buf_append(b, x, 4);
}

/* Raw deflate, level 6. Returns false if it does not make the data smaller. */
static bool deflate_raw(const Buf *in, Buf *out) {
    z_stream z = {0};
    if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    uLong bound = deflateBound(&z, in->size);
    out->data = malloc(bound);
    if (!out->data) { deflateEnd(&z); return false; }
    out->cap = bound;
    z.next_in = in->data;
    z.avail_in = in->size;
    z.next_out = out->data;
    z.avail_out = bound;
    int rc = deflate(&z, Z_FINISH);
    out->size = z.total_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END || out->size >= in->size) {
        buf_free(out);
        return false;
    }
    return true;
}

typedef struct {
    uint16_t method;
    uint32_t crc, csize, usize, offset;
} Entry;

bool zip_directory(const char *root, Buf *out, int64_t *latest_mtime, char *err, size_t err_size) {
    FileList *list = calloc(1, sizeof *list);
    Entry *entries = calloc(MAX_ENTRIES, sizeof *entries);
    bool ok = list && entries;
    if (ok && !list_files(root, "", list)) {
        snprintf(err, err_size, "lecture du dossier impossible");
        ok = false;
    }
    if (ok) qsort(list->names, list->count, sizeof list->names[0], compare_names);

    for (size_t i = 0; ok && i < list->count; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s%s", root, list->names[i]);
        Buf raw = {0}, packed = {0};
        if (!read_file(path, &raw)) {
            snprintf(err, err_size, "lecture impossible : %s", list->names[i]);
            ok = false;
            break;
        }
        Entry *e = &entries[i];
        e->crc = crc32(0, raw.data, raw.size);
        e->usize = raw.size;
        e->offset = out->size;
        bool deflated = raw.size > 0 && deflate_raw(&raw, &packed);
        const Buf *body = deflated ? &packed : &raw;
        e->method = deflated ? 8 : 0;
        e->csize = body->size;

        uint16_t name_len = strlen(list->names[i]);
        ok = put32(out, 0x04034b50) && put16(out, 20) && put16(out, 0) && put16(out, e->method) &&
             put16(out, 0) && put16(out, DOS_DATE_1980_01_01) && put32(out, e->crc) &&
             put32(out, e->csize) && put32(out, e->usize) && put16(out, name_len) && put16(out, 0) &&
             buf_append(out, list->names[i], name_len) &&
             (body->size == 0 || buf_append(out, body->data, body->size));
        buf_free(&raw);
        buf_free(&packed);
        if (!ok) snprintf(err, err_size, "memoire insuffisante");
    }

    if (ok) {
        uint32_t cd_offset = out->size;
        for (size_t i = 0; ok && i < list->count; i++) {
            Entry *e = &entries[i];
            uint16_t name_len = strlen(list->names[i]);
            ok = put32(out, 0x02014b50) && put16(out, 20) && put16(out, 20) && put16(out, 0) &&
                 put16(out, e->method) && put16(out, 0) && put16(out, DOS_DATE_1980_01_01) &&
                 put32(out, e->crc) && put32(out, e->csize) && put32(out, e->usize) &&
                 put16(out, name_len) && put16(out, 0) && put16(out, 0) && put16(out, 0) &&
                 put16(out, 0) && put32(out, 0) && put32(out, e->offset) &&
                 buf_append(out, list->names[i], name_len);
        }
        uint32_t cd_size = out->size - cd_offset;
        ok = ok && put32(out, 0x06054b50) && put16(out, 0) && put16(out, 0) &&
             put16(out, list->count) && put16(out, list->count) && put32(out, cd_size) &&
             put32(out, cd_offset) && put16(out, 0);
        if (!ok) snprintf(err, err_size, "memoire insuffisante");
    }

    if (ok && list->count == 0) {
        snprintf(err, err_size, "save vide");
        ok = false;
    }
    if (latest_mtime) *latest_mtime = list ? list->latest_mtime : 0;
    if (list) {
        for (size_t i = 0; i < list->count; i++) free(list->names[i]);
        free(list);
    }
    free(entries);
    if (!ok) buf_free(out);
    return ok;
}
