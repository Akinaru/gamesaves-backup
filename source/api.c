#include "api.h"

#include <ctype.h>
#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <switch.h>

#define ROOTS_PATH "romfs:/isrg-roots.pem"
#define API_PREFIX "/api/pokemon-saves"

static const Config *g_cfg;
static char g_auth_header[300];
static Buf g_roots;

/*
 * Trusts the bundled root certificates. The Switch SSL service imports one
 * certificate per call, so a bundle given as CURLOPT_CAINFO would only trust
 * its first entry: each PEM block is imported separately instead.
 */
static CURLcode import_roots(CURL *curl, void *ssl_ctx, void *user) {
    static const char BEGIN[] = "-----BEGIN CERTIFICATE-----", END[] = "-----END CERTIFICATE-----";
    const char *p = (const char *)g_roots.data, *end = p + g_roots.size;
    while ((p = memmem(p, end - p, BEGIN, strlen(BEGIN)))) {
        const char *stop = memmem(p, end - p, END, strlen(END));
        if (!stop) break;
        stop += strlen(END);
        /* A failure (e.g. root already in the system store) is not fatal: verification decides. */
        sslContextImportServerPki(ssl_ctx, p, stop - p, SslCertificateFormat_Pem, NULL);
        p = stop;
    }
    return CURLE_OK;
}

bool api_init(const Config *cfg) {
    g_cfg = cfg;
    api_set_key(cfg->api_key);
    return read_file(ROOTS_PATH, &g_roots) && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void api_set_key(const char *key) {
    snprintf(g_auth_header, sizeof g_auth_header, "X-API-Key: %s", key);
}

void api_exit(void) {
    curl_global_cleanup();
    buf_free(&g_roots);
}

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *user) {
    return buf_append(user, ptr, size * nmemb) ? size * nmemb : 0;
}

/*
 * One HTTP request to url. Retries network errors and 5xx up to `retries` times
 * (after 2 s, then 5 s). The API key is sent only when auth is true.
 */
static ApiResult fetch(const char *method, const char *url, const char *content_type,
                       const Buf *body, bool auth, int retries, Buf *response) {
    static const int RETRY_DELAYS[] = {2, 5};
    ApiResult r = {0};

    for (int attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0) svcSleepThread((s64)RETRY_DELAYS[attempt - 1] * 1000000000LL);
        buf_free(response);
        r = (ApiResult){0};

        CURL *curl = curl_easy_init();
        if (!curl) {
            snprintf(r.error, sizeof r.error, "curl indisponible");
            return r;
        }
        struct curl_slist *headers = auth ? curl_slist_append(NULL, g_auth_header) : NULL;
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, import_roots);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "aSaveBackup/2.0");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
        if (body) {
            char header[128];
            snprintf(header, sizeof header, "Content-Type: %s", content_type);
            headers = curl_slist_append(headers, header);
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)body->data);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body->size);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        else if (rc == CURLE_PEER_FAILED_VERIFICATION) {
            /* Usually a wrong console clock: the certificate looks not yet valid or expired. */
            time_t now = time(NULL);
            char date[32];
            strftime(date, sizeof date, "%d/%m/%Y %H:%M", gmtime(&now));
            snprintf(r.error, sizeof r.error, "Certificat refusé. Date de la Switch : %s UTC, vérifie-la.", date);
        } else snprintf(r.error, sizeof r.error, "Réseau : %s", curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (r.status != 0 && r.status < 500) break;
        if (r.status >= 500) snprintf(r.error, sizeof r.error, "Erreur serveur %ld", r.status);
    }

    if (r.status >= 400) {
        const char *message = NULL;
        json_t *root = json_loadb((const char *)response->data, response->size, 0, NULL);
        if (root) message = json_string_value(json_object_get(root, "error"));
        if (r.status == 401) snprintf(r.error, sizeof r.error, "Clé API refusée");
        else if (message) snprintf(r.error, sizeof r.error, "%s", message);
        else snprintf(r.error, sizeof r.error, "HTTP %ld", r.status);
        json_decref(root);
    }
    return r;
}

static ApiResult request(const char *method, const char *path, const char *content_type,
                         const Buf *body, bool auth, Buf *response) {
    char url[1024];
    snprintf(url, sizeof url, "%s%s%s", g_cfg->url, API_PREFIX, path);
    return fetch(method, url, content_type, body, auth, 2, response);
}

ApiResult api_download(const char *url, Buf *response) {
    return fetch("GET", url, NULL, NULL, false, 0, response);
}

static void copy_str(char *dst, size_t size, json_t *value) {
    const char *s = json_string_value(value);
    snprintf(dst, size, "%s", s ? s : "");
}

static long int_or(json_t *value, long fallback) {
    return json_is_integer(value) ? (long)json_integer_value(value) : fallback;
}

/* Parses the JSON body of response as an array (or object when want_array is false). */
static json_t *parse_body(const Buf *response, bool want_array, ApiResult *r, const char *what) {
    json_t *root = json_loadb((const char *)response->data, response->size, 0, NULL);
    if (want_array ? json_is_array(root) : json_is_object(root)) return root;
    json_decref(root);
    r->status = 0;
    snprintf(r->error, sizeof r->error, "Réponse %s illisible", what);
    return NULL;
}

static uint32_t parse_color(json_t *value) {
    const char *s = json_string_value(value);
    return s && s[0] == '#' ? (uint32_t)strtoul(s + 1, NULL, 16) : 0x444444;
}

ApiResult api_get_games(Game **games, size_t *count) {
    Buf response = {0};
    ApiResult r = request("GET", "/games", NULL, NULL, true, &response);
    *games = NULL;
    *count = 0;
    json_t *root = r.status == 200 ? parse_body(&response, true, &r, "/games") : NULL;
    if (root) {
        *games = calloc(json_array_size(root) + 1, sizeof **games);
        size_t i;
        json_t *item;
        json_array_foreach(root, i, item) {
            Game *g = &(*games)[(*count)++];
            copy_str(g->id, sizeof g->id, json_object_get(item, "id"));
            copy_str(g->name, sizeof g->name, json_object_get(item, "name"));
            if (!g->name[0]) snprintf(g->name, sizeof g->name, "%s", g->id);
            g->gen = int_or(json_object_get(item, "gen"), 0);
            g->auto_read = json_is_true(json_object_get(item, "autoRead"));
            json_t *colors = json_object_get(item, "colors");
            if (json_array_size(colors) == 2) {
                g->has_colors = true;
                g->colors[0] = parse_color(json_array_get(colors, 0));
                g->colors[1] = parse_color(json_array_get(colors, 1));
            }
            copy_str(g->title_id, sizeof g->title_id, json_object_get(item, "switchTitleId"));
            for (char *c = g->title_id; *c; c++) *c = toupper((unsigned char)*c);
            size_t j;
            json_t *code;
            json_array_foreach(json_object_get(item, "ndsGameCodes"), j, code) {
                if (g->nds_count < MAX_NDS_CODES) copy_str(g->nds_codes[g->nds_count++], 4, code);
            }
        }
        json_decref(root);
    }
    buf_free(&response);
    return r;
}

static void parse_mon(json_t *item, Mon *m) {
    *m = (Mon){0};
    m->dex = int_or(json_object_get(item, "dex"), 0);
    copy_str(m->species, sizeof m->species, json_object_get(item, "species"));
    copy_str(m->nickname, sizeof m->nickname, json_object_get(item, "nickname"));
    copy_str(m->ot, sizeof m->ot, json_object_get(item, "otName"));
    m->level = int_or(json_object_get(item, "level"), 0);
    m->shiny = json_is_true(json_object_get(item, "shiny"));
    m->egg = json_is_true(json_object_get(item, "egg"));
}

static void parse_summary(json_t *item, SaveSummary *s) {
    *s = (SaveSummary){0};
    copy_str(s->id, sizeof s->id, json_object_get(item, "id"));
    copy_str(s->game, sizeof s->game, json_object_get(json_object_get(item, "game"), "id"));
    copy_str(s->sha256, sizeof s->sha256, json_object_get(item, "sha256"));
    copy_str(s->trainer, sizeof s->trainer, json_object_get(item, "trainer"));
    copy_str(s->location, sizeof s->location, json_object_get(item, "location"));
    copy_str(s->updated_at, sizeof s->updated_at, json_object_get(item, "updatedAt"));
    s->money = int_or(json_object_get(item, "money"), -1);
    json_t *play = json_object_get(item, "playTime");
    s->play_hours = int_or(json_object_get(play, "hours"), -1);
    s->play_minutes = int_or(json_object_get(play, "minutes"), 0);
    s->badges = int_or(json_object_get(item, "badges"), -1);
    s->versions = int_or(json_object_get(item, "versions"), 0);
    size_t i;
    json_t *mon;
    json_array_foreach(json_object_get(item, "party"), i, mon) {
        if (s->party_count < PARTY_SIZE) parse_mon(mon, &s->party[s->party_count++]);
    }
}

ApiResult api_get_saves(const char *slot, SaveSummary **saves, size_t *count) {
    char path[128];
    snprintf(path, sizeof path, "/saves?slot=%s", slot);
    Buf response = {0};
    ApiResult r = request("GET", path, NULL, NULL, true, &response);
    *saves = NULL;
    *count = 0;
    json_t *root = r.status == 200 ? parse_body(&response, true, &r, "/saves") : NULL;
    if (root) {
        *saves = calloc(json_array_size(root) + 1, sizeof **saves);
        size_t i;
        json_t *item;
        json_array_foreach(root, i, item) parse_summary(item, &(*saves)[(*count)++]);
        json_decref(root);
    }
    buf_free(&response);
    return r;
}

ApiResult api_get_save_detail(const char *id, SaveDetail *d) {
    char path[128];
    snprintf(path, sizeof path, "/saves/%s", id);
    Buf response = {0};
    ApiResult r = request("GET", path, NULL, NULL, true, &response);
    *d = (SaveDetail){.trainer_id = -1, .dex_owned = -1, .dex_seen = -1};
    json_t *root = r.status == 200 ? parse_body(&response, false, &r, "/saves/:id") : NULL;
    if (root) {
        copy_str(d->rival, sizeof d->rival, json_object_get(root, "rival"));
        d->trainer_id = int_or(json_object_get(root, "trainerId"), -1);
        json_t *dex = json_object_get(root, "pokedex");
        d->dex_owned = int_or(json_object_get(dex, "owned"), -1);
        d->dex_seen = int_or(json_object_get(dex, "seen"), -1);
        json_t *boxes = json_object_get(root, "boxes");
        d->boxes = calloc(json_array_size(boxes) + 1, sizeof *d->boxes);
        size_t i;
        json_t *box;
        json_array_foreach(boxes, i, box) {
            Box *b = &d->boxes[d->box_count++];
            copy_str(b->name, sizeof b->name, json_object_get(box, "name"));
            json_t *mons = json_object_get(box, "mons");
            b->mons = calloc(json_array_size(mons) + 1, sizeof *b->mons);
            size_t j;
            json_t *mon;
            json_array_foreach(mons, j, mon) parse_mon(mon, &b->mons[b->count++]);
        }
        json_decref(root);
    }
    buf_free(&response);
    return r;
}

void save_detail_free(SaveDetail *d) {
    for (int i = 0; i < d->box_count; i++) free(d->boxes[i].mons);
    free(d->boxes);
    *d = (SaveDetail){0};
}

ApiResult api_pair_start(const char *device, char code[16], char pairing_id[128], int *interval) {
    json_t *payload = json_pack("{s:s}", "device", device);
    char *text = json_dumps(payload, JSON_COMPACT);
    json_decref(payload);
    Buf body = {0}, response = {0};
    buf_append(&body, text, strlen(text));
    free(text);

    ApiResult r = request("POST", "/pair", "application/json", &body, false, &response);
    buf_free(&body);
    json_t *root = r.status == 201 ? parse_body(&response, false, &r, "/pair") : NULL;
    if (root) {
        copy_str(code, 16, json_object_get(root, "code"));
        copy_str(pairing_id, 128, json_object_get(root, "pairingId"));
        *interval = int_or(json_object_get(root, "interval"), 5);
        if (*interval <= 0) *interval = 5;
        if (!code[0] || !pairing_id[0]) {
            r.status = 0;
            snprintf(r.error, sizeof r.error, "Réponse /pair illisible");
        }
        json_decref(root);
    } else if (r.status != 0 && !r.error[0]) {
        snprintf(r.error, sizeof r.error, "HTTP %ld", r.status);
    }
    buf_free(&response);
    return r;
}

ApiResult api_pair_poll(const char *pairing_id, bool *approved, char api_key[256]) {
    char path[160];
    snprintf(path, sizeof path, "/pair/%s", pairing_id);
    Buf response = {0};
    ApiResult r = request("GET", path, NULL, NULL, false, &response);
    *approved = false;
    api_key[0] = '\0';
    json_t *root = r.status == 200 ? parse_body(&response, false, &r, "/pair/:id") : NULL;
    if (root) {
        const char *status = json_string_value(json_object_get(root, "status"));
        if (status && !strcmp(status, "approved")) {
            copy_str(api_key, 256, json_object_get(root, "apiKey"));
            *approved = api_key[0] != '\0';
        }
        json_decref(root);
    }
    buf_free(&response);
    return r;
}

ApiResult api_put_backup(const char *game, const char *slot, const char *filename,
                         int64_t modified_ms, const Buf *body, char outcome[16], SaveSummary *saved) {
    char path[512];
    CURL *curl = curl_easy_init();
    char *name = curl ? curl_easy_escape(curl, filename, 0) : NULL;
    int n = snprintf(path, sizeof path, "/backups/%s/%s?filename=%s", game, slot, name ? name : "save.bin");
    if (modified_ms > 0) snprintf(path + n, sizeof path - n, "&modified=%lld", (long long)modified_ms);
    curl_free(name);
    curl_easy_cleanup(curl);

    Buf response = {0};
    ApiResult r = request("PUT", path, "application/octet-stream", body, true, &response);
    outcome[0] = '\0';
    json_t *root = r.status == 200 || r.status == 201 ? parse_body(&response, false, &r, "PUT /backups") : NULL;
    if (root) {
        copy_str(outcome, 16, json_object_get(root, "status"));
        parse_summary(json_object_get(root, "save"), saved);
        json_decref(root);
    }
    buf_free(&response);
    return r;
}
