#include "saves.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"
#include "zip.h"

#define MAX_ITEMS 128
#define MAX_UPLOAD_SIZE (64u * 1024 * 1024)
#define NO_SAVE_FOR_PROFILE "Aucune save pour ce profil"

static const Config *g_cfg;
static AccountUid g_uid;
static const Game *g_games;
static size_t g_game_count;
static const SaveSummary *g_remote;
static size_t g_remote_count;
static ProgressFn g_progress;
static Item g_items[MAX_ITEMS];
static size_t g_item_count;
static int g_skipped_nds;

Item *saves_items(size_t *count) {
    *count = g_item_count;
    return g_items;
}

int saves_skipped_nds(void) {
    return g_skipped_nds;
}

/* Builds the bytes to upload: a ZIP of the save folder (Switch) or the raw .sav (DS). */
static bool build_payload(const Item *it, Buf *out, int64_t *mtime, char *err, size_t err_size) {
    if (it->is_nds) {
        struct stat st;
        if (stat(it->sav_path, &st) != 0 || !read_file(it->sav_path, out)) {
            snprintf(err, err_size, "Lecture du fichier .sav impossible");
            return false;
        }
        *mtime = st.st_mtime;
        return true;
    }
    if (R_FAILED(fsdevMountSaveData("save", it->title_id, g_uid))) {
        snprintf(err, err_size, NO_SAVE_FOR_PROFILE);
        return false;
    }
    bool ok = zip_directory("save:/", out, mtime, err, err_size);
    fsdevUnmountDevice("save");
    return ok;
}

static void refresh_state(Item *it) {
    it->state = !it->has_remote ? ST_NEW : strcmp(it->remote.sha256, it->sha) ? ST_CHANGED : ST_UP_TO_DATE;
}

static void add_item(const Game *game, bool is_nds, u64 title_id, const char *sav_path) {
    if (g_item_count >= MAX_ITEMS) return;
    Item *it = &g_items[g_item_count];
    *it = (Item){.game = game, .is_nds = is_nds, .title_id = title_id};
    if (sav_path) snprintf(it->sav_path, sizeof it->sav_path, "%s", sav_path);
    for (size_t i = 0; i < g_remote_count; i++) {
        if (!strcmp(g_remote[i].game, game->id)) {
            it->has_remote = true;
            it->remote = g_remote[i];
        }
    }

    /* Two DS files for the same game would overwrite each other on the same slot. */
    for (size_t i = 0; i < g_item_count; i++) {
        if (g_items[i].game == game) {
            it->state = ST_ERROR;
            snprintf(it->msg, sizeof it->msg, "Doublon ignoré : %s", strrchr(it->sav_path, '/') + 1);
            g_item_count++;
            return;
        }
    }

    Buf data = {0};
    int64_t mtime = 0;
    char err[160];
    if (!build_payload(it, &data, &mtime, err, sizeof err)) {
        /* No save on this profile: the game is simply not listed. */
        if (!is_nds && !strcmp(err, NO_SAVE_FOR_PROFILE)) return;
        it->state = ST_ERROR;
        snprintf(it->msg, sizeof it->msg, "%s", err);
    } else {
        sha256_hex(data.data, data.size, it->sha);
        it->size = data.size;
        refresh_state(it);
    }
    buf_free(&data);
    g_item_count++;
}

static const Game *game_for_nds_code(const char code[4]) {
    for (size_t i = 0; i < g_game_count; i++) {
        for (int j = 0; j < g_games[i].nds_count; j++) {
            if (!strncmp(g_games[i].nds_codes[j], code, 3)) return &g_games[i];
        }
    }
    return NULL;
}

/* Reads the 4-char game code at offset 0x0C of the .nds next to the .sav. */
static bool nds_code_for_save(const char *sav_path, char code[5]) {
    char rom[512];
    snprintf(rom, sizeof rom, "%.*s.nds", (int)(strlen(sav_path) - 4), sav_path);
    FILE *f = fopen(rom, "rb");
    if (!f) return false;
    bool ok = fseek(f, 0x0C, SEEK_SET) == 0 && fread(code, 1, 4, f) == 4;
    fclose(f);
    code[4] = '\0';
    return ok;
}

static void scan_nds(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir_path, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_nds(path);
        } else if (ends_with_ci(e->d_name, ".sav")) {
            char code[5];
            const Game *game = nds_code_for_save(path, code) ? game_for_nds_code(code) : NULL;
            if (game) {
                g_progress(e->d_name);
                add_item(game, true, 0, path);
            } else {
                g_skipped_nds++;
            }
        }
    }
    closedir(dir);
}

void saves_scan(const Config *cfg, AccountUid uid, const Game *games, size_t game_count,
                const SaveSummary *remote, size_t remote_count, ProgressFn progress) {
    g_cfg = cfg;
    g_uid = uid;
    g_games = games;
    g_game_count = game_count;
    g_remote = remote;
    g_remote_count = remote_count;
    g_progress = progress;
    g_item_count = 0;
    g_skipped_nds = 0;
    for (size_t i = 0; i < game_count; i++) {
        if (!games[i].title_id[0]) continue;
        progress(games[i].name);
        add_item(&games[i], false, strtoull(games[i].title_id, NULL, 16), NULL);
    }
    scan_nds(cfg->nds_dir);
}

long saves_upload(Item *it, char *result, size_t result_size) {
    /* Items that failed before hashing (duplicates) are never uploaded. */
    if (it->state == ST_ERROR && it->sha[0] == '\0') {
        snprintf(result, result_size, "%s", it->msg);
        return 0;
    }
    Buf data = {0};
    int64_t mtime = 0;
    char err[160];
    if (!build_payload(it, &data, &mtime, err, sizeof err)) {
        it->state = ST_ERROR;
        snprintf(it->msg, sizeof it->msg, "%s", err);
        snprintf(result, result_size, "%s", err);
        return 0;
    }
    if (data.size > MAX_UPLOAD_SIZE) {
        it->state = ST_ERROR;
        snprintf(it->msg, sizeof it->msg, "Save trop grosse (plus de 64 Mo)");
        snprintf(result, result_size, "%s", it->msg);
        buf_free(&data);
        return 0;
    }
    sha256_hex(data.data, data.size, it->sha);
    it->size = data.size;

    char filename[256];
    if (it->is_nds) snprintf(filename, sizeof filename, "%s", strrchr(it->sav_path, '/') + 1);
    else snprintf(filename, sizeof filename, "%s.zip", it->game->id);

    char outcome[16];
    SaveSummary saved;
    ApiResult r = api_put_backup(it->game->id, g_cfg->slot, filename, mtime * 1000, &data, outcome, &saved);
    buf_free(&data);

    if (r.status == 200 || r.status == 201) {
        it->state = ST_SENT;
        it->msg[0] = '\0';
        it->has_remote = true;
        it->remote = saved;
        if (!strcmp(outcome, "created")) snprintf(result, result_size, "Save créée sur le serveur");
        else if (!strcmp(outcome, "unchanged")) snprintf(result, result_size, "Déjà à jour sur le serveur");
        else snprintf(result, result_size, "Save mise à jour, l'ancienne version est conservée");
    } else {
        it->state = ST_ERROR;
        snprintf(it->msg, sizeof it->msg, "%s", r.error);
        snprintf(result, result_size, "%s", r.error);
        /* Revoked device key: the next launch pairs again. */
        if (r.status == 401 && g_cfg->key_from_file) config_forget_key();
    }
    return r.status;
}
