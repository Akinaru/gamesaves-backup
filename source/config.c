#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static void copy(char *dst, size_t size, const char *src) {
    snprintf(dst, size, "%s", src);
}

/* Slot format required by the API: ^[A-Za-z0-9._-]{1,64}$ */
static bool valid_slot(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 64) return false;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '_' && *s != '-') return false;
    }
    return true;
}

bool config_load(Config *cfg, char *err, size_t err_size) {
    memset(cfg, 0, sizeof *cfg);
    copy(cfg->url, sizeof cfg->url, "https://atools.akinaru.fr");
    copy(cfg->slot, sizeof cfg->slot, "switch");
    copy(cfg->nds_dir, sizeof cfg->nds_dir, "sdmc:/roms/nds");

    FILE *f = fopen(CONFIG_PATH, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s), *value = trim(eq + 1);
        if (!strcmp(key, "url")) copy(cfg->url, sizeof cfg->url, value);
        else if (!strcmp(key, "api_key")) copy(cfg->api_key, sizeof cfg->api_key, value);
        else if (!strcmp(key, "slot")) copy(cfg->slot, sizeof cfg->slot, value);
        else if (!strcmp(key, "nds_dir")) copy(cfg->nds_dir, sizeof cfg->nds_dir, value);
    }
    if (f) fclose(f);

    if (!cfg->api_key[0] && (f = fopen(KEY_PATH, "r"))) {
        if (fgets(line, sizeof line, f)) copy(cfg->api_key, sizeof cfg->api_key, trim(line));
        fclose(f);
        cfg->key_from_file = cfg->api_key[0] != '\0';
    }

    size_t n = strlen(cfg->url);
    while (n > 0 && cfg->url[n - 1] == '/') cfg->url[--n] = '\0';
    n = strlen(cfg->nds_dir);
    while (n > 0 && cfg->nds_dir[n - 1] == '/') cfg->nds_dir[--n] = '\0';

    if (!valid_slot(cfg->slot)) {
        snprintf(err, err_size, "slot invalide (lettres, chiffres, . _ - ; 64 max)");
        return false;
    }
    return true;
}

bool config_save_key(const char *key) {
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(KEY_PATH, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s\n", key) > 0;
    return fclose(f) == 0 && ok;
}

void config_forget_key(void) {
    remove(KEY_PATH);
}
