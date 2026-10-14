#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

#include "api.h"
#include "config.h"

typedef enum { ST_NEW, ST_CHANGED, ST_UP_TO_DATE, ST_SENT, ST_ERROR } SaveState;

/* One save found on the console, and what the server holds for it. */
typedef struct {
    const Game *game;
    bool is_nds;
    u64 title_id;
    char sav_path[512];   /* DS only */
    char sha[65];         /* empty when the save could not be read */
    size_t size;
    SaveState state;
    char msg[160];        /* last error */
    bool has_remote;
    SaveSummary remote;
} Item;

typedef void (*ProgressFn)(const char *what);

/*
 * Lists the saves of the catalog games: Switch saves of the profile uid, then the
 * melonDS .sav files under cfg->nds_dir. remote holds the server summaries of our slot.
 */
void saves_scan(const Config *cfg, AccountUid uid, const Game *games, size_t game_count,
                const SaveSummary *remote, size_t remote_count, ProgressFn progress);

Item *saves_items(size_t *count);

/* .sav files skipped: no .nds next to them, or a game outside the catalog. */
int saves_skipped_nds(void);

/*
 * Uploads one save. result receives a message for the user.
 * Returns the HTTP status (0 when nothing was sent, 401 when the key is refused).
 */
long saves_upload(Item *it, char *result, size_t result_size);
