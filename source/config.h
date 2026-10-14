#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CONFIG_PATH "sdmc:/config/atools-backup/config.ini"

typedef struct {
    char url[256];
    char api_key[256];
    char slot[65];
    char nds_dir[256];
} Config;

/* Loads CONFIG_PATH. On failure, writes a message into err and returns false. */
bool config_load(Config *cfg, char *err, size_t err_size);
