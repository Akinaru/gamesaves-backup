#include "ui.h"

#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL_ttf.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define MAX_FONT_SIZE 128
#define TEXT_CACHE_SIZE 384

typedef struct {
    bool used;
    bool ext;
    int size;
    Uint32 rgba;
    char text[200];
    SDL_Texture *tex;
    int w, h;
    Uint32 last_used;
} CachedText;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static PlFontData g_std_font, g_ext_font;
static TTF_Font *g_fonts[2][MAX_FONT_SIZE + 1]; /* [ext][size] */
static CachedText g_cache[TEXT_CACHE_SIZE];
static Uint32 g_frame;

bool ui_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) return false;
    if (R_FAILED(plInitialize(PlServiceType_User)) ||
        R_FAILED(plGetSharedFontByType(&g_std_font, PlSharedFontType_Standard)) ||
        R_FAILED(plGetSharedFontByType(&g_ext_font, PlSharedFontType_NintendoExt)))
        return false;
    g_window = SDL_CreateWindow("aSaveBackup", 0, 0, SCREEN_W, SCREEN_H, 0);
    if (!g_window) return false;
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) return false;
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    return true;
}

void ui_exit(void) {
    for (int i = 0; i < TEXT_CACHE_SIZE; i++)
        if (g_cache[i].tex) SDL_DestroyTexture(g_cache[i].tex);
    for (int e = 0; e < 2; e++)
        for (int s = 0; s <= MAX_FONT_SIZE; s++)
            if (g_fonts[e][s]) TTF_CloseFont(g_fonts[e][s]);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    plExit();
    TTF_Quit();
    SDL_Quit();
}

SDL_Renderer *ui_renderer(void) {
    return g_renderer;
}

void ui_begin(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {} /* input comes from libnx pads; SDL only needs its queue drained */
    g_frame++;
    SDL_SetRenderDrawColor(g_renderer, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(g_renderer);
}

void ui_end(void) {
    SDL_RenderPresent(g_renderer);
}

static TTF_Font *font(bool ext, int size) {
    if (size < 1 || size > MAX_FONT_SIZE) size = 20;
    if (!g_fonts[ext][size]) {
        PlFontData *data = ext ? &g_ext_font : &g_std_font;
        g_fonts[ext][size] = TTF_OpenFontRW(SDL_RWFromConstMem(data->address, data->size), 1, size);
    }
    return g_fonts[ext][size];
}

static Uint32 pack(SDL_Color c) {
    return (Uint32)c.r << 24 | c.g << 16 | c.b << 8 | c.a;
}

/* Rendered text texture, from a small LRU cache. */
static CachedText *text_texture(bool ext, int size, SDL_Color color, const char *text) {
    Uint32 rgba = pack(color);
    CachedText *slot = &g_cache[0];
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        CachedText *c = &g_cache[i];
        if (c->used && c->ext == ext && c->size == size && c->rgba == rgba && !strcmp(c->text, text)) {
            c->last_used = g_frame;
            return c;
        }
        if (!c->used || c->last_used < slot->last_used) slot = c;
    }
    TTF_Font *f = font(ext, size);
    SDL_Surface *surface = f ? TTF_RenderUTF8_Blended(f, text, color) : NULL;
    if (!surface) return NULL;
    if (slot->tex) SDL_DestroyTexture(slot->tex);
    *slot = (CachedText){.used = true, .ext = ext, .size = size, .rgba = rgba, .last_used = g_frame};
    snprintf(slot->text, sizeof slot->text, "%s", text);
    slot->tex = SDL_CreateTextureFromSurface(g_renderer, surface);
    slot->w = surface->w;
    slot->h = surface->h;
    SDL_FreeSurface(surface);
    return slot;
}

static int draw_text(bool ext, int size, int x, int y, SDL_Color color, Align align, const char *text) {
    if (!text[0]) return 0;
    CachedText *c = text_texture(ext, size, color, text);
    if (!c || !c->tex) return 0;
    if (align == ALIGN_CENTER) x -= c->w / 2;
    else if (align == ALIGN_RIGHT) x -= c->w;
    SDL_Rect dst = {x, y, c->w, c->h};
    SDL_RenderCopy(g_renderer, c->tex, NULL, &dst);
    return c->w;
}

int ui_text_width(int size, const char *text) {
    int w = 0;
    TTF_Font *f = font(false, size);
    if (f && text[0]) TTF_SizeUTF8(f, text, &w, NULL);
    return w;
}

int ui_text(int size, int x, int y, SDL_Color color, Align align, int max_w, const char *text) {
    if (max_w > 0 && ui_text_width(size, text) > max_w) {
        char shortened[200];
        size_t n = snprintf(shortened, sizeof shortened - 4, "%s", text);
        if (n > sizeof shortened - 5) n = sizeof shortened - 5;
        while (n > 0) {
            do n--; while (n > 0 && (shortened[n] & 0xC0) == 0x80); /* stay on a UTF-8 boundary */
            strcpy(shortened + n, "…");
            if (ui_text_width(size, shortened) <= max_w) break;
        }
        return draw_text(false, size, x, y, color, align, shortened);
    }
    return draw_text(false, size, x, y, color, align, text);
}

int ui_text_wrap(int size, int x, int y, int w, SDL_Color color, Align align, const char *text) {
    int line_h = size * 3 / 2, top = y;
    char line[200];
    const char *p = text;
    while (*p) {
        /* Grow the line word by word while it fits. */
        size_t len = 0, fit = 0;
        while (true) {
            while (p[len] == ' ') len++;
            while (p[len] && p[len] != ' ') len++;
            if (len >= sizeof line) break;
            memcpy(line, p, len);
            line[len] = '\0';
            if (fit && ui_text_width(size, line) > w) break;
            fit = len;
            if (!p[len]) break;
        }
        if (!fit) fit = len < sizeof line ? len : sizeof line - 1;
        memcpy(line, p, fit);
        line[fit] = '\0';
        int lx = align == ALIGN_CENTER ? x + w / 2 : align == ALIGN_RIGHT ? x + w : x;
        draw_text(false, size, lx, y, color, align, line);
        y += line_h;
        p += fit;
        while (*p == ' ') p++;
    }
    return y - top;
}

int ui_textf(int size, int x, int y, SDL_Color color, Align align, const char *fmt, ...) {
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    return draw_text(false, size, x, y, color, align, text);
}

void ui_rect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(g_renderer, &r);
}

/*
 * Filled rounded rectangle as one triangle fan. Unlike SDL2_gfx boxes, nothing is
 * drawn twice, so translucent colors stay even.
 */
void ui_round_rect(int x, int y, int w, int h, int radius, SDL_Color c) {
    enum { MAX_SEGMENTS = 24 };
    radius = SDL_min(radius, SDL_min(w, h) / 2);
    if (radius <= 0) {
        ui_rect(x, y, w, h, c);
        return;
    }
    int segments = SDL_clamp(radius / 3, 4, MAX_SEGMENTS);
    static const float CORNER_ANGLES[4] = {-90, 0, 90, 180};
    float cx[4] = {x + w - radius, x + w - radius, x + radius, x + radius};
    float cy[4] = {y + radius, y + h - radius, y + h - radius, y + radius};
    SDL_Vertex v[1 + 4 * (MAX_SEGMENTS + 1)];
    int idx[3 * 4 * (MAX_SEGMENTS + 1)];
    int n = 1, k = 0;
    v[0] = (SDL_Vertex){{x + w / 2.0f, y + h / 2.0f}, c, {0, 0}};
    for (int corner = 0; corner < 4; corner++) {
        for (int i = 0; i <= segments; i++) {
            float a = (CORNER_ANGLES[corner] + 90.0f * i / segments) * (float)M_PI / 180.0f;
            v[n++] = (SDL_Vertex){{cx[corner] + radius * SDL_cosf(a), cy[corner] + radius * SDL_sinf(a)}, c, {0, 0}};
        }
    }
    for (int i = 1; i < n; i++) {
        idx[k++] = 0;
        idx[k++] = i;
        idx[k++] = i + 1 < n ? i + 1 : 1;
    }
    SDL_RenderGeometry(g_renderer, NULL, v, n, idx, k);
}

void ui_round_border(int x, int y, int w, int h, int radius, SDL_Color c) {
    roundedRectangleRGBA(g_renderer, x, y, x + w - 1, y + h - 1, radius, c.r, c.g, c.b, c.a);
    roundedRectangleRGBA(g_renderer, x + 1, y + 1, x + w - 2, y + h - 2, radius, c.r, c.g, c.b, c.a);
}

void ui_circle(int x, int y, int r, SDL_Color c) {
    ui_round_rect(x - r, y - r, 2 * r, 2 * r, r, c);
}

void ui_gradient(int x, int y, int w, int h, SDL_Color a, SDL_Color b) {
    SDL_Vertex v[4] = {
        {{x, y}, a, {0, 0}},
        {{x + w, y}, b, {0, 0}},
        {{x + w, y + h}, b, {0, 0}},
        {{x, y + h}, a, {0, 0}},
    };
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(g_renderer, NULL, v, 4, idx, 6);
}

void ui_image(SDL_Texture *tex, int x, int y, int w, int h) {
    int tw, th;
    if (!tex || SDL_QueryTexture(tex, NULL, NULL, &tw, &th) != 0 || !tw || !th) return;
    float scale = SDL_min((float)w / tw, (float)h / th);
    SDL_Rect dst = {x + (w - (int)(tw * scale)) / 2, y + (h - (int)(th * scale)) / 2, (int)(tw * scale), (int)(th * scale)};
    SDL_RenderCopy(g_renderer, tex, NULL, &dst);
}

int ui_pill(int x, int y, Align align, SDL_Color color, const char *text) {
    int w = ui_text_width(16, text) + 24;
    if (align == ALIGN_RIGHT) x -= w;
    else if (align == ALIGN_CENTER) x -= w / 2;
    /* Opaque tint of the color over the panel, so it reads the same on any background. */
    SDL_Color bg = ui_mix(color, C_PANEL, 64);
    ui_round_rect(x, y, w, 28, 14, bg);
    draw_text(false, 16, x + 12, y + 4, color, ALIGN_LEFT, text);
    return w;
}

int ui_hint(int x, int y, const char *glyph, const char *label) {
    int w = draw_text(true, 26, x, y - 2, C_TEXT, ALIGN_LEFT, glyph);
    if (!label[0]) return w + 6;
    w += 8;
    w += draw_text(false, 20, x + w, y + 1, C_TEXT, ALIGN_LEFT, label);
    return w + 32;
}

void ui_overlay(const char *title, const char *subtitle) {
    ui_rect(0, 0, SCREEN_W, SCREEN_H, C_DIM);
    int w = 620, h = 170, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    ui_round_rect(x, y, w, h, 18, C_PANEL);
    ui_round_border(x, y, w, h, 18, C_BORDER);
    /* Three dots that move with time, as a busy indicator. */
    Uint32 t = SDL_GetTicks() / 250;
    for (int i = 0; i < 3; i++)
        ui_circle(SCREEN_W / 2 - 24 + i * 24, y + 40, 7, (int)(t % 3) == i ? C_ACCENT : C_BORDER);
    ui_text(28, SCREEN_W / 2, y + 66, C_TEXT, ALIGN_CENTER, w - 48, title);
    if (subtitle) ui_text(20, SCREEN_W / 2, y + 110, C_MUTED, ALIGN_CENTER, w - 48, subtitle);
    ui_end();
}

SDL_Color ui_rgb(unsigned rgb) {
    return (SDL_Color){rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF, 255};
}

SDL_Color ui_mix(SDL_Color color, SDL_Color base, int amount) {
    return (SDL_Color){(color.r * amount + base.r * (255 - amount)) / 255, (color.g * amount + base.g * (255 - amount)) / 255,
                       (color.b * amount + base.b * (255 - amount)) / 255, 255};
}
