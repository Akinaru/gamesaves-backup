#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Growable byte buffer. */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} Buf;

bool buf_append(Buf *b, const void *data, size_t len);
void buf_free(Buf *b);

/* Reads a whole file into b. Returns false on error. */
bool read_file(const char *path, Buf *b);

/* Lowercase hex SHA-256 of data (out must hold 65 chars). */
void sha256_hex(const void *data, size_t len, char out[65]);

/* Copies UTF-8 text into out as plain ASCII (accents dropped), for the console font. */
void ascii_fold(const char *in, char *out, size_t out_size);

bool ends_with_ci(const char *s, const char *suffix);
