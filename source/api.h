#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "util.h"

#define MAX_NDS_CODES 8
#define PARTY_SIZE 6

typedef struct {
    char id[32];
    char name[64];          /* UTF-8, e.g. "Pokémon Bouclier" */
    int gen;
    bool auto_read;         /* the server reads trainer, party and boxes */
    bool has_colors;
    uint32_t colors[2];     /* 0xRRGGBB gradient, when the server sends it */
    char title_id[17];      /* Switch title ID, uppercase, empty if none */
    char nds_codes[MAX_NDS_CODES][4]; /* DS game code prefixes (3 chars, region-free) */
    int nds_count;
} Game;

typedef struct {
    int dex;                /* 0 for an empty slot */
    char species[32];
    char nickname[32];
    char ot[32];
    int level;              /* 0 if unknown */
    bool shiny;
    bool egg;
} Mon;

/* Server-side summary of the save stored in our slot. Unknown fields: empty string or -1. */
typedef struct {
    char id[48];
    char game[32];
    char sha256[65];
    char trainer[32];
    char location[64];
    char updated_at[32];    /* ISO 8601 */
    long money;
    int play_hours, play_minutes;
    int badges;
    int versions;
    Mon party[PARTY_SIZE];
    int party_count;
} SaveSummary;

typedef struct {
    char name[32];
    Mon *mons;
    int count;
} Box;

typedef struct {
    Box *boxes;
    int box_count;
    char rival[32];
    long trainer_id;        /* -1 if unknown */
    int dex_owned, dex_seen; /* -1 if unknown */
} SaveDetail;

/* API call result. HTTP status, or 0 on network error. */
typedef struct {
    long status;
    char error[160]; /* UTF-8 message, empty on success */
} ApiResult;

bool api_init(const Config *cfg);
void api_exit(void);
void api_set_key(const char *key);

ApiResult api_get_games(Game **games, size_t *count);

/* GET /saves?slot=: summaries of the saves stored in this slot. */
ApiResult api_get_saves(const char *slot, SaveSummary **saves, size_t *count);

/* GET /saves/:id: boxes, Pokédex and rival. Free with save_detail_free. */
ApiResult api_get_save_detail(const char *id, SaveDetail *detail);
void save_detail_free(SaveDetail *detail);

/* POST /pair: starts a pairing. code receives the 6-char code to enter on the website. */
ApiResult api_pair_start(const char *device, char code[16], char pairing_id[128], int *interval);

/* GET /pair/:id: approved is true once the user validated the code; api_key then holds the device key. */
ApiResult api_pair_poll(const char *pairing_id, bool *approved, char api_key[256]);

/*
 * PUT /backups/:game/:slot. outcome receives "created", "updated" or "unchanged",
 * and saved the server summary of the stored save.
 */
ApiResult api_put_backup(const char *game, const char *slot, const char *filename,
                         int64_t modified_ms, const Buf *body, char outcome[16], SaveSummary *saved);

/* GET of any HTTPS URL (images), without the API key and without retries. Thread-safe. */
ApiResult api_download(const char *url, Buf *response);
