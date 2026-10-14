#include "images.h"

#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "api.h"
#include "config.h"

#define CACHE_DIR CONFIG_DIR "/cache"
#define MAX_IMAGES 1024
#define SPRITES_URL "https://raw.githubusercontent.com/PokeAPI/sprites/master/sprites/pokemon"

typedef enum { IMG_QUEUED, IMG_ON_DISK, IMG_LOADED, IMG_FAILED } ImageState;

typedef struct {
    char url[200];
    ImageState state; /* written by the worker under g_lock */
    SDL_Texture *tex;
} Image;

static SDL_Renderer *g_renderer;
static Image g_images[MAX_IMAGES];
static int g_count;
static int g_queue[MAX_IMAGES];
static int g_queue_head, g_queue_tail;
static SDL_mutex *g_lock;
static SDL_sem *g_pending;
static SDL_Thread *g_thread;
static volatile bool g_quit;

static void cache_path(const char *url, char *out, size_t size) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (const char *p = url; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    snprintf(out, size, CACHE_DIR "/%08lx.img", (unsigned long)h);
}

static int worker(void *unused) {
    while (true) {
        SDL_SemWait(g_pending);
        if (g_quit) return 0;
        SDL_LockMutex(g_lock);
        int index = g_queue[g_queue_head++ % MAX_IMAGES];
        char url[200];
        snprintf(url, sizeof url, "%s", g_images[index].url);
        SDL_UnlockMutex(g_lock);

        char path[256], tmp[260];
        cache_path(url, path, sizeof path);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        Buf data = {0};
        bool ok = api_download(url, &data).status == 200 && data.size > 0;
        if (ok) {
            FILE *f = fopen(tmp, "wb");
            ok = f && fwrite(data.data, 1, data.size, f) == data.size;
            if (f) ok = fclose(f) == 0 && ok;
            ok = ok && rename(tmp, path) == 0;
            if (!ok) remove(tmp);
        }
        buf_free(&data);

        SDL_LockMutex(g_lock);
        g_images[index].state = ok ? IMG_ON_DISK : IMG_FAILED;
        SDL_UnlockMutex(g_lock);
    }
}

bool images_init(SDL_Renderer *renderer) {
    g_renderer = renderer;
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    mkdir(CACHE_DIR, 0777);
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    g_lock = SDL_CreateMutex();
    g_pending = SDL_CreateSemaphore(0);
    g_thread = g_lock && g_pending ? SDL_CreateThreadWithStackSize(worker, "images", 512 * 1024, NULL) /* curl + TLS need a large stack */ : NULL;
    return g_thread != NULL;
}

void images_exit(void) {
    if (g_thread) {
        g_quit = true;
        SDL_SemPost(g_pending);
        SDL_WaitThread(g_thread, NULL);
    }
    for (int i = 0; i < g_count; i++)
        if (g_images[i].tex) SDL_DestroyTexture(g_images[i].tex);
    if (g_pending) SDL_DestroySemaphore(g_pending);
    if (g_lock) SDL_DestroyMutex(g_lock);
    IMG_Quit();
}

SDL_Texture *image_get(const char *url) {
    if (!url[0] || !g_thread) return NULL;
    int index = -1;
    for (int i = 0; i < g_count; i++) {
        if (!strcmp(g_images[i].url, url)) { index = i; break; }
    }
    char path[256];
    cache_path(url, path, sizeof path);
    if (index < 0) {
        if (g_count >= MAX_IMAGES) return NULL;
        index = g_count++;
        Image *img = &g_images[index];
        snprintf(img->url, sizeof img->url, "%s", url);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) {
            img->state = IMG_ON_DISK;
        } else {
            img->state = IMG_QUEUED;
            SDL_LockMutex(g_lock);
            g_queue[g_queue_tail++ % MAX_IMAGES] = index;
            SDL_UnlockMutex(g_lock);
            SDL_SemPost(g_pending);
        }
    }

    Image *img = &g_images[index];
    SDL_LockMutex(g_lock);
    ImageState state = img->state;
    SDL_UnlockMutex(g_lock);
    if (state == IMG_ON_DISK) {
        img->tex = IMG_LoadTexture(g_renderer, path);
        if (!img->tex) remove(path); /* corrupt file: downloaded again next launch */
        else if (strstr(url, ".jpg")) SDL_SetTextureScaleMode(img->tex, SDL_ScaleModeLinear); /* photos, not pixel art */
        img->state = img->tex ? IMG_LOADED : IMG_FAILED;
    }
    return img->tex;
}

void sprite_url(int dex, bool shiny, bool egg, char *out, size_t size) {
    if (egg) snprintf(out, size, SPRITES_URL "/egg.png");
    else if (dex > 0) snprintf(out, size, SPRITES_URL "/%s%d.png", shiny ? "shiny/" : "", dex);
    else if (size) out[0] = '\0';
}
