#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "util.h"

/*
 * Zips every file under root (for example "save:/") into out.
 * The archive is deterministic: entries sorted by path, fixed 1980-01-01 date,
 * deflate level 6 (or stored when deflate does not shrink), no extra fields.
 * latest_mtime receives the newest file modification time (0 if unknown).
 */
bool zip_directory(const char *root, Buf *out, int64_t *latest_mtime, char *err, size_t err_size);
