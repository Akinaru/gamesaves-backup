#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "api.h"
#include "config.h"
#include "util.h"
#include "zip.h"

#define MAX_ITEMS 128
#define VISIBLE_ROWS 32
#define MAX_UPLOAD_SIZE (64u * 1024 * 1024)

typedef enum { ST_NEW, ST_CHANGED, ST_UP_TO_DATE, ST_SENT, ST_ERROR } State;

typedef struct {
    const Game *game;
    bool is_nds;
    u64 title_id;
    char sav_path[512]; /* DS only */
    char sha[65];
    size_t size;
    State state;
    char msg[160];
} Item;

static Config g_cfg;
static Game *g_games;
static size_t g_game_count;
static Backup *g_backups;
static size_t g_backup_count;
static AccountUid g_uid;
static char g_nickname[64];
static Item g_items[MAX_ITEMS];
static size_t g_item_count;
static int g_skipped_nds;
static bool g_key_invalid;

static void status_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("\x1b[44;1H\x1b[2K");
    vprintf(fmt, ap);
    va_end(ap);
    consoleUpdate(NULL);
}

static const char *server_sha(const char *game_id) {
    for (size_t i = 0; i < g_backup_count; i++) {
        if (!strcmp(g_backups[i].game, game_id) && !strcmp(g_backups[i].slot, g_cfg.slot))
            return g_backups[i].sha256;
    }
    return NULL;
}

/* Builds the bytes to upload: a ZIP of the save folder (Switch) or the raw .sav (DS). */
static bool build_payload(const Item *it, Buf *out, int64_t *mtime, char *err, size_t err_size) {
    if (it->is_nds) {
        struct stat st;
        if (stat(it->sav_path, &st) != 0 || !read_file(it->sav_path, out)) {
            snprintf(err, err_size, "lecture impossible");
            return false;
        }
        *mtime = st.st_mtime;
        return true;
    }
    if (R_FAILED(fsdevMountSaveData("save", it->title_id, g_uid))) {
        snprintf(err, err_size, "aucune save pour ce profil");
        return false;
    }
    bool ok = zip_directory("save:/", out, mtime, err, err_size);
    fsdevUnmountDevice("save");
    return ok;
}

static void refresh_state(Item *it) {
    const char *remote = server_sha(it->game->id);
    it->state = !remote ? ST_NEW : strcmp(remote, it->sha) ? ST_CHANGED : ST_UP_TO_DATE;
}

static void add_item(const Game *game, bool is_nds, u64 title_id, const char *sav_path) {
    if (g_item_count >= MAX_ITEMS) return;
    Item *it = &g_items[g_item_count];
    *it = (Item){.game = game, .is_nds = is_nds, .title_id = title_id};
    if (sav_path) snprintf(it->sav_path, sizeof it->sav_path, "%s", sav_path);

    /* Two DS files for the same game would overwrite each other on the same slot. */
    for (size_t i = 0; i < g_item_count; i++) {
        if (g_items[i].game == game) {
            it->state = ST_ERROR;
            snprintf(it->msg, sizeof it->msg, "doublon, ignore : %s", strrchr(it->sav_path, '/') + 1);
            g_item_count++;
            return;
        }
    }

    Buf data = {0};
    int64_t mtime = 0;
    char err[80];
    if (!build_payload(it, &data, &mtime, err, sizeof err)) {
        /* No save on this profile: the game is simply not listed. */
        if (!is_nds && !strcmp(err, "aucune save pour ce profil")) return;
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

static void scan_switch(void) {
    for (size_t i = 0; i < g_game_count; i++) {
        if (!g_games[i].title_id[0]) continue;
        status_line("Lecture : %s", g_games[i].name);
        add_item(&g_games[i], false, strtoull(g_games[i].title_id, NULL, 16), NULL);
    }
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
                status_line("Lecture : %s", e->d_name);
                add_item(game, true, 0, path);
            } else {
                g_skipped_nds++;
            }
        }
    }
    closedir(dir);
}

static const char *state_label(State s) {
    switch (s) {
        case ST_NEW: return "\x1b[33mNOUVELLE\x1b[0m";
        case ST_CHANGED: return "\x1b[33mMODIFIEE\x1b[0m";
        case ST_UP_TO_DATE: return "\x1b[32mA JOUR  \x1b[0m";
        case ST_SENT: return "\x1b[32mENVOYEE \x1b[0m";
        default: return "\x1b[31mERREUR  \x1b[0m";
    }
}

static void render(size_t cursor, size_t top) {
    consoleClear();
    printf("\x1b[36maSaveBackup\x1b[0m  profil : %s  slot : %s\n", g_nickname, g_cfg.slot);
    printf("--------------------------------------------------------------------------------\n");
    if (g_item_count == 0) printf("Aucune save Pokemon trouvee.\n");
    for (size_t i = top; i < g_item_count && i < top + VISIBLE_ROWS; i++) {
        const Item *it = &g_items[i];
        printf("%s [%s] %-28.28s %-6s", i == cursor ? ">" : " ", state_label(it->state),
               it->game->name, it->is_nds ? "DS" : "Switch");
        if (it->msg[0]) printf(" %.30s", it->msg);
        else if (it->size) printf(" %zu Ko", (it->size + 1023) / 1024);
        printf("\n");
    }
    if (g_skipped_nds) printf("\n%d fichier(s) .sav ignore(s) : ROM .nds absente ou jeu hors catalogue.\n", g_skipped_nds);
    printf("\x1b[42;1H--------------------------------------------------------------------------------");
    printf("\x1b[43;1HA : envoyer  X : envoyer toutes les nouvelles/modifiees  + : quitter");
    consoleUpdate(NULL);
}

static void upload(Item *it) {
    /* Items that failed before hashing (duplicates) are never uploaded. */
    if (g_key_invalid || (it->state == ST_ERROR && it->sha[0] == '\0')) return;
    status_line("Envoi : %s...", it->game->name);

    Buf data = {0};
    int64_t mtime = 0;
    char err[80];
    if (!build_payload(it, &data, &mtime, err, sizeof err)) {
        it->state = ST_ERROR;
        snprintf(it->msg, sizeof it->msg, "%s", err);
        return;
    }
    if (data.size > MAX_UPLOAD_SIZE) {
        it->state = ST_ERROR;
        snprintf(it->msg, sizeof it->msg, "trop gros (> 64 Mo)");
        buf_free(&data);
        return;
    }
    sha256_hex(data.data, data.size, it->sha);
    it->size = data.size;

    char filename[256];
    if (it->is_nds) snprintf(filename, sizeof filename, "%s", strrchr(it->sav_path, '/') + 1);
    else snprintf(filename, sizeof filename, "%s.zip", it->game->id);

    char outcome[16];
    ApiResult r = api_put_backup(it->game->id, g_cfg.slot, filename, mtime * 1000, &data, outcome);
    buf_free(&data);

    if (r.status == 200 || r.status == 201) {
        it->state = ST_SENT;
        snprintf(it->msg, sizeof it->msg, "%s", outcome);
    } else {
        it->state = ST_ERROR;
        snprintf(it->msg, sizeof it->msg, "%s", r.error);
        if (r.status == 401) {
            g_key_invalid = true;
            /* Revoked device key: the next launch pairs again. */
            if (g_cfg.key_from_file) config_forget_key();
        }
    }
}

/*
 * Picks the Switch profile whose saves are backed up: the preselected user,
 * else the only user, else the system selector, else the last opened user.
 */
static bool choose_user(char *err, size_t err_size) {
    AccountUid users[ACC_USER_LIST_SIZE];
    s32 user_count = 0;
    Result rc = accountGetPreselectedUser(&g_uid);
    if (R_FAILED(rc) || !accountUidIsValid(&g_uid)) {
        rc = accountListAllUsers(users, ACC_USER_LIST_SIZE, &user_count);
        if (R_SUCCEEDED(rc) && user_count == 1) {
            g_uid = users[0];
        } else {
            PselUserSelectionSettings settings = {0};
            rc = pselShowUserSelector(&g_uid, &settings);
            if (R_SUCCEEDED(rc) && !accountUidIsValid(&g_uid)) {
                snprintf(err, err_size, "aucun profil choisi (selection annulee)");
                return false;
            }
            if (R_FAILED(rc) && (R_FAILED(accountGetLastOpenedUser(&g_uid)) || !accountUidIsValid(&g_uid))) {
                snprintf(err, err_size, "aucun profil choisi (selecteur : 0x%X, %d profil(s))", rc, user_count);
                return false;
            }
        }
    }
    AccountProfile profile;
    AccountProfileBase base = {0};
    if (R_SUCCEEDED(accountGetProfile(&profile, g_uid))) {
        accountProfileGet(&profile, NULL, &base);
        accountProfileClose(&profile);
    }
    ascii_fold(base.nickname, g_nickname, sizeof g_nickname);
    return true;
}

/* Prints a fatal error and waits for + before exiting. */
static void fatal(PadState *pad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("\n\x1b[31mErreur :\x1b[0m ");
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n\nAppuie sur + pour quitter.\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_Plus) break;
    }
}

static void show_pairing_code(const char *code) {
    consoleClear();
    printf("\x1b[36maSaveBackup\x1b[0m  appairage de cette Switch\n");
    printf("--------------------------------------------------------------------------------\n\n");
    printf("1. Sur ton telephone ou ton PC, ouvre :\n\n");
    printf("   \x1b[36m%s/apps/pokemon-saves/pair\x1b[0m\n\n", g_cfg.url);
    printf("2. Saisis ce code :\n\n");
    printf("            \x1b[33;1m%s\x1b[0m\n\n", code);
    printf("Le code est valable 10 minutes. Un nouveau code s'affiche ensuite.\n");
    printf("\x1b[43;1H+ : annuler");
    consoleUpdate(NULL);
}

/*
 * Pairs the Switch: shows a short code to validate on the website, polls until
 * the device key arrives, then stores it on the SD card. Returns false if cancelled.
 */
static bool pair_device(PadState *pad) {
    char device[96];
    snprintf(device, sizeof device, "Switch (%s)", g_nickname);
    while (true) {
        char code[16], pairing_id[128];
        int interval;
        ApiResult r = api_pair_start(device, code, pairing_id, &interval);
        if (r.status != 201) {
            fatal(pad, "appairage : %s", r.error);
            return false;
        }
        show_pairing_code(code);

        u64 next_poll = armGetSystemTick() + armNsToTicks((u64)interval * 1000000000ULL);
        bool expired = false;
        while (!expired) {
            if (!appletMainLoop()) return false;
            padUpdate(pad);
            if (padGetButtonsDown(pad) & HidNpadButton_Plus) return false;
            if (armGetSystemTick() < next_poll) {
                svcSleepThread(50000000LL);
                continue;
            }
            bool approved;
            char key[256];
            r = api_pair_poll(pairing_id, &approved, key);
            next_poll = armGetSystemTick() + armNsToTicks((u64)interval * 1000000000ULL);
            if (r.status == 404) {
                expired = true;
            } else if (r.status != 200) {
                status_line("\x1b[31m%s\x1b[0m, nouvel essai...", r.error);
            } else if (approved) {
                snprintf(g_cfg.api_key, sizeof g_cfg.api_key, "%s", key);
                g_cfg.key_from_file = true;
                api_set_key(key);
                consoleClear();
                if (!config_save_key(key)) {
                    printf("\x1b[33mAttention :\x1b[0m cle non enregistree dans %s.\n", KEY_PATH);
                    printf("Il faudra appairer de nouveau au prochain lancement.\n\n");
                }
                return true;
            }
        }
    }
}

static void run(PadState *pad) {
    char err[160];
    if (!config_load(&g_cfg, err, sizeof err)) { fatal(pad, "%s", err); return; }
    if (!api_init(&g_cfg)) { fatal(pad, "initialisation reseau impossible (romfs:/isrg-roots.pem ?)"); return; }
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) { fatal(pad, "service des comptes indisponible (0x%X)", rc); return; }
    if (!choose_user(err, sizeof err)) { fatal(pad, "%s", err); return; }
    if (!g_cfg.api_key[0] && !pair_device(pad)) return;

    printf("Connexion a %s...\n", g_cfg.url);
    consoleUpdate(NULL);
    ApiResult r = api_get_games(&g_games, &g_game_count);
    if (r.status == 401 && g_cfg.key_from_file) {
        /* The device key was revoked on the website: pair again. */
        config_forget_key();
        g_cfg.api_key[0] = '\0';
        if (!pair_device(pad)) return;
        r = api_get_games(&g_games, &g_game_count);
    }
    if (r.status == 401) { fatal(pad, "cle API refusee, verifie api_key dans %s", CONFIG_PATH); return; }
    if (r.status != 200) { fatal(pad, "catalogue : %s", r.error); return; }
    r = api_get_backups(&g_backups, &g_backup_count);
    if (r.status != 200) { fatal(pad, "backups : %s", r.error); return; }

    scan_switch();
    scan_nds(g_cfg.nds_dir);

    size_t cursor = 0, top = 0;
    render(cursor, top);
    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        if (down & HidNpadButton_Plus) break;
        if ((down & HidNpadButton_AnyDown) && cursor + 1 < g_item_count) cursor++;
        if ((down & HidNpadButton_AnyUp) && cursor > 0) cursor--;
        if (cursor < top) top = cursor;
        if (cursor >= top + VISIBLE_ROWS) top = cursor - VISIBLE_ROWS + 1;

        if ((down & HidNpadButton_A) && g_item_count) {
            upload(&g_items[cursor]);
        } else if (down & HidNpadButton_X) {
            int sent = 0, errors = 0;
            for (size_t i = 0; i < g_item_count; i++) {
                Item *it = &g_items[i];
                if (it->state != ST_NEW && it->state != ST_CHANGED) continue;
                upload(it);
                if (it->state == ST_SENT) sent++;
                else errors++;
            }
            render(cursor, top);
            status_line("Termine : %d envoyee(s), %d erreur(s).", sent, errors);
            continue;
        } else if (!(down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))) {
            continue;
        }
        render(cursor, top);
        if (g_key_invalid) status_line("\x1b[31mCle refusee (revoquee ?) : relance l'app pour appairer de nouveau.\x1b[0m");
    }
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    romfsInit();
    socketInitializeDefault();

    run(&pad);

    api_exit();
    free(g_games);
    free(g_backups);
    accountExit();
    socketExit();
    romfsExit();
    consoleExit(NULL);
    return 0;
}
