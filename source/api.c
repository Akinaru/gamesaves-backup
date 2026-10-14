#include "api.h"

#include <ctype.h>
#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define CA_PATH "romfs:/cacert.pem"
#define API_PREFIX "/api/pokemon-saves"

static const Config *g_cfg;
static char g_auth_header[300];

bool api_init(const Config *cfg) {
    g_cfg = cfg;
    snprintf(g_auth_header, sizeof g_auth_header, "X-API-Key: %s", cfg->api_key);
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void api_exit(void) {
    curl_global_cleanup();
}

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *user) {
    return buf_append(user, ptr, size * nmemb) ? size * nmemb : 0;
}

/* One HTTP request. Retries network errors and 5xx twice (after 2 s, then 5 s). */
static ApiResult request(const char *method, const char *path, const Buf *body, Buf *response) {
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
        struct curl_slist *headers = curl_slist_append(NULL, g_auth_header);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_PATH);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "aSaveBackup/1.0");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
        if (body) {
            headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char *)body->data);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body->size);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        else snprintf(r.error, sizeof r.error, "reseau : %s", curl_easy_strerror(rc));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (r.status != 0 && r.status < 500) break;
        if (r.status >= 500) snprintf(r.error, sizeof r.error, "erreur serveur %ld", r.status);
    }

    if (r.status >= 400) {
        const char *message = NULL;
        json_t *root = json_loadb((const char *)response->data, response->size, 0, NULL);
        if (root) message = json_string_value(json_object_get(root, "error"));
        if (r.status == 401) snprintf(r.error, sizeof r.error, "cle API invalide");
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
    ApiResult r = request("GET", "/games", NULL, &response);
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
    ApiResult r = request("GET", "/backups", NULL, &response);
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
    ApiResult r = request("PUT", path, body, &response);
    outcome[0] = '\0';
    if (r.status == 200 || r.status == 201) {
        json_t *root = json_loadb((const char *)response.data, response.size, 0, NULL);
        copy_str(outcome, 16, json_object_get(root, "status"));
        json_decref(root);
    }
    buf_free(&response);
    return r;
}
