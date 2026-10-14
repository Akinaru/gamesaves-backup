#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CONFIG_DIR "sdmc:/config/atools-backup"
#define CONFIG_PATH CONFIG_DIR "/config.ini"
/* Device key received by pairing, written by the app. */
#define KEY_PATH CONFIG_DIR "/device.key"

typedef struct {
    char url[256];
    char api_key[256];
    char slot[65];
    char nds_dir[256];
    bool key_from_file; /* api_key comes from KEY_PATH, not config.ini */
} Config;

/*
 * Loads CONFIG_PATH (optional) and the device key. api_key stays empty when the
 * Switch is not paired yet. On failure, writes a message into err and returns false.
 */
bool config_load(Config *cfg, char *err, size_t err_size);

bool config_save_key(const char *key);
void config_forget_key(void);
