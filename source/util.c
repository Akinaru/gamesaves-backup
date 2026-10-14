#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

bool buf_append(Buf *b, const void *data, size_t len) {
    if (b->size + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->size + len) cap *= 2;
        uint8_t *p = realloc(b->data, cap);
        if (!p) return false;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->size, data, len);
    b->size += len;
    return true;
}

void buf_free(Buf *b) {
    free(b->data);
    *b = (Buf){0};
}

bool read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t chunk[64 * 1024];
    size_t n;
    bool ok = true;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (!buf_append(b, chunk, n)) { ok = false; break; }
    }
    if (ferror(f)) ok = false;
    fclose(f);
    return ok;
}

void sha256_hex(const void *data, size_t len, char out[65]) {
    uint8_t hash[SHA256_HASH_SIZE];
    sha256CalculateHash(hash, data, len);
    for (int i = 0; i < SHA256_HASH_SIZE; i++) sprintf(out + i * 2, "%02x", hash[i]);
}

bool ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}
