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
 * One HTTP request. Retries network errors and 5xx twice (after 2 s, then 5 s).
 * The API key is sent only when auth is true (the pairing routes are public).
 */
static ApiResult request(const char *method, const char *path, const char *content_type,
                         const Buf *body, bool auth, Buf *response) {
    static const int RETRY_DELAYS[] = {2, 5};
    char url[1024];
    snprintf(url, sizeof url, "%s%s%s", g_cfg->url, API_PREFIX, path);
    ApiResult r = {0};

    for (int attempt = 0; attempt <= 2; attempt++) {
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
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "aSaveBackup/1.0");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
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
            snprintf(r.error, sizeof r.error, "certificat refuse. Date de la Switch : %s UTC, verifie-la", date);
        } else snprintf(r.error, sizeof r.error, "reseau : %s", curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (r.status != 0 && r.status < 500) break;
        if (r.status >= 500) snprintf(r.error, sizeof r.error, "erreur serveur %ld", r.status);
    }

    if (r.status >= 400) {
        const char *message = NULL;
        json_t *root = json_loadb((const char *)response->data, response->size, 0, NULL);
        if (root) message = json_string_value(json_object_get(root, "error"));
        if (r.status == 401) snprintf(r.error, sizeof r.error, "cle API refusee");
        else if (message) ascii_fold(message, r.error, sizeof r.error);
        else snprintf(r.error, sizeof r.error, "HTTP %ld", r.status);
        json_decref(root);
    }
    return r;
}

static void copy_str(char *dst, size_t size, json_t *value) {
    const char *s = json_string_value(value);
    snprintf(dst, size, "%s", s ? s : "");
}

ApiResult api_get_games(Game **games, size_t *count) {
    Buf response = {0};
    ApiResult r = request("GET", "/games", NULL, NULL, true, &response);
    *games = NULL;
    *count = 0;
    if (r.status == 200) {
        json_t *root = json_loadb((const char *)response.data, response.size, 0, NULL);
        if (json_is_array(root)) {
            *games = calloc(json_array_size(root), sizeof **games);
            size_t i;
            json_t *item;
            json_array_foreach(root, i, item) {
                Game *g = &(*games)[(*count)++];
                copy_str(g->id, sizeof g->id, json_object_get(item, "id"));
                const char *name = json_string_value(json_object_get(item, "name"));
                ascii_fold(name ? name : g->id, g->name, sizeof g->name);
                copy_str(g->title_id, sizeof g->title_id, json_object_get(item, "switchTitleId"));
                for (char *c = g->title_id; *c; c++) *c = toupper((unsigned char)*c);
                size_t j;
                json_t *code;
                json_array_foreach(json_object_get(item, "ndsGameCodes"), j, code) {
                    if (g->nds_count < MAX_NDS_CODES) copy_str(g->nds_codes[g->nds_count++], 4, code);
                }
            }
        } else {
            r.status = 0;
            snprintf(r.error, sizeof r.error, "reponse /games illisible");
        }
        json_decref(root);
    }
    buf_free(&response);
    return r;
}

ApiResult api_get_backups(Backup **backups, size_t *count) {
    Buf response = {0};
    ApiResult r = request("GET", "/backups", NULL, NULL, true, &response);
    *backups = NULL;
    *count = 0;
    if (r.status == 200) {
        json_t *root = json_loadb((const char *)response.data, response.size, 0, NULL);
        if (json_is_array(root)) {
            *backups = calloc(json_array_size(root), sizeof **backups);
            size_t i;
            json_t *item;
            json_array_foreach(root, i, item) {
                Backup *b = &(*backups)[(*count)++];
                copy_str(b->game, sizeof b->game, json_object_get(item, "game"));
                copy_str(b->slot, sizeof b->slot, json_object_get(item, "slot"));
                copy_str(b->sha256, sizeof b->sha256, json_object_get(item, "sha256"));
            }
        } else {
            r.status = 0;
            snprintf(r.error, sizeof r.error, "reponse /backups illisible");
        }
        json_decref(root);
    }
    buf_free(&response);
    return r;
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
    if (r.status == 201) {
        json_t *root = json_loadb((const char *)response.data, response.size, 0, NULL);
        copy_str(code, 16, json_object_get(root, "code"));
        copy_str(pairing_id, 128, json_object_get(root, "pairingId"));
        *interval = json_integer_value(json_object_get(root, "interval"));
        if (*interval <= 0) *interval = 5;
        if (!code[0] || !pairing_id[0]) {
            r.status = 0;
            snprintf(r.error, sizeof r.error, "reponse /pair illisible");
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
    if (r.status == 200) {
        json_t *root = json_loadb((const char *)response.data, response.size, 0, NULL);
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
                         int64_t modified_ms, const Buf *body, char outcome[16]) {
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
    if (r.status == 200 || r.status == 201) {
        json_t *root = json_loadb((const char *)response.data, response.size, 0, NULL);
        copy_str(outcome, 16, json_object_get(root, "status"));
        json_decref(root);
    }
    buf_free(&response);
    return r;
}
