#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "util.h"

#define MAX_NDS_CODES 8

typedef struct {
    char id[32];
    char name[64];          /* ASCII-folded, for the console */
    char title_id[17];      /* Switch title ID, uppercase, empty if none */
    char nds_codes[MAX_NDS_CODES][4]; /* DS game code prefixes (3 chars, region-free) */
    int nds_count;
} Game;

typedef struct {
    char game[32];
    char slot[65];
    char sha256[65];
} Backup;

/* API call result. HTTP status, or 0 on network error. */
typedef struct {
    long status;
    char error[160]; /* ASCII message, empty on success */
} ApiResult;

bool api_init(const Config *cfg);
void api_exit(void);
void api_set_key(const char *key);

ApiResult api_get_games(Game **games, size_t *count);
ApiResult api_get_backups(Backup **backups, size_t *count);

/* POST /pair: starts a pairing. code receives the 6-char code to enter on the website. */
ApiResult api_pair_start(const char *device, char code[16], char pairing_id[128], int *interval);

/* GET /pair/:id: approved is true once the user validated the code; api_key then holds the device key. */
ApiResult api_pair_poll(const char *pairing_id, bool *approved, char api_key[256]);

/* PUT /backups/:game/:slot. outcome receives "created", "updated" or "unchanged". */
ApiResult api_put_backup(const char *game, const char *slot, const char *filename,
                         int64_t modified_ms, const Buf *body, char outcome[16]);
