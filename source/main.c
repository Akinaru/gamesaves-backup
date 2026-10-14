#include <SDL2/SDL_image.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "api.h"
#include "config.h"
#include "images.h"
#include "saves.h"
#include "ui.h"

#define LIST_X 32
#define LIST_Y 100
#define LIST_W 420
#define ROW_H 76
#define VISIBLE_ROWS 7
#define PANEL_X 476
#define PANEL_Y 100
#define PANEL_W 772
#define PANEL_H 556
#define BOX_COLS 6
#define BOX_ROWS 5
#define BOX_PAGE (BOX_COLS * BOX_ROWS)
#define TOAST_NS 4000000000ULL

typedef enum { SCREEN_SPLASH, SCREEN_HOME, SCREEN_BOXES } Screen;

/* A page of the box screen: up to BOX_PAGE Pokémon of one server box. */
typedef struct {
    int box;
    int start;
    int count;
} Page;

static Config g_cfg;
static Game *g_games;
static size_t g_game_count;
static SaveSummary *g_saves;
static size_t g_save_count;
static AccountUid g_uid;
static char g_nickname[64];
static SDL_Texture *g_avatar;

static PadState g_pad;
static Screen g_screen = SCREEN_SPLASH;
static size_t g_cursor, g_top;
static char g_toast[200];
static SDL_Color g_toast_color;
static u64 g_toast_until;
static bool g_key_refused;

static const Item *g_box_item;
static SaveDetail g_detail;
static Page *g_pages;
static int g_page_count, g_page, g_box_cursor;

/* ---------- Helpers ---------- */

static u64 now_ns(void) {
    return armTicksToNs(armGetSystemTick());
}

static void toast(SDL_Color color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof g_toast, fmt, ap);
    va_end(ap);
    g_toast_color = color;
    g_toast_until = now_ns() + TOAST_NS;
}

/* "Pokémon Bouclier" -> "Bouclier", for the compact list. */
static const char *short_name(const Game *g) {
    static const char PREFIX[] = "Pokémon ";
    return strncmp(g->name, PREFIX, strlen(PREFIX)) ? g->name : g->name + strlen(PREFIX);
}

static void format_size(size_t size, char *out, size_t n) {
    if (size >= 1024 * 1024) snprintf(out, n, "%.1f Mo", size / (1024.0 * 1024.0));
    else snprintf(out, n, "%zu Ko", (size + 1023) / 1024);
    for (char *c = out; *c; c++) if (*c == '.') *c = ',';
}

/* 67180 -> "67 180" */
static void format_number(long v, char *out, size_t n) {
    char digits[32];
    int len = snprintf(digits, sizeof digits, "%ld", v), o = 0;
    for (int i = 0; i < len && o + 2 < (int)n; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ' ';
        out[o++] = digits[i];
    }
    out[o] = '\0';
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm). */
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

/* ISO 8601 UTC date -> "14/10/2026 à 18:12" in the console time zone. */
static void format_date(const char *iso, char *out, size_t n) {
    int y, mo, d, h, mi;
    if (sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
        snprintf(out, n, "?");
        return;
    }
    u64 ts = (u64)days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
    TimeCalendarTime cal;
    TimeCalendarAdditionalInfo info;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(ts, &cal, &info)))
        snprintf(out, n, "%02d/%02d/%04d à %02d:%02d", cal.day, cal.month, cal.year, cal.hour, cal.minute);
    else
        snprintf(out, n, "%02d/%02d/%04d à %02d:%02d UTC", d, mo, y, h, mi);
}

static SDL_Color game_color(const Game *g, int i) {
    if (g->has_colors) return ui_rgb(g->colors[i]);
    return i ? C_PANEL2 : C_ACCENT;
}

static void state_label(SaveState s, const char **label, SDL_Color *color) {
    switch (s) {
        case ST_NEW: *label = "Nouvelle"; *color = C_BLUE; break;
        case ST_CHANGED: *label = "Modifiée"; *color = C_AMBER; break;
        case ST_UP_TO_DATE: *label = "À jour"; *color = C_GREEN; break;
        case ST_SENT: *label = "Envoyée"; *color = C_GREEN; break;
        default: *label = "Erreur"; *color = C_RED; break;
    }
}

static void draw_sprite(const Mon *m, int x, int y, int w, int h) {
    char url[200];
    sprite_url(m->dex, m->shiny, m->egg, url, sizeof url);
    SDL_Texture *tex = image_get(url);
    if (tex) ui_image(tex, x, y, w, h);
    else if (url[0]) ui_circle(x + w / 2, y + h / 2, SDL_min(w, h) / 6, C_BORDER);
}

static const char *mon_name(const Mon *m) {
    if (m->egg) return "Œuf";
    return m->nickname[0] ? m->nickname : m->species[0] ? m->species : "???";
}

/* ---------- Common chrome ---------- */

static void draw_header(void) {
    ui_circle(52, 42, 18, C_ACCENT);
    ui_rect(34, 40, 37, 4, C_BG);
    ui_circle(52, 42, 6, C_TEXT);
    ui_text(30, 84, 16, C_TEXT, ALIGN_LEFT, 0, "aSaveBackup");
    ui_text(16, 86, 54, C_MUTED, ALIGN_LEFT, 0, "Backup de tes saves Pokémon");
    if (g_nickname[0]) {
        int x = SCREEN_W - 32;
        if (g_avatar) {
            ui_image(g_avatar, x - 52, 16, 52, 52);
            x -= 68;
        }
        ui_text(22, x, 16, C_TEXT, ALIGN_RIGHT, 300, g_nickname);
        ui_textf(16, x, 46, C_MUTED, ALIGN_RIGHT, "Slot : %s", g_cfg.slot);
    }
    ui_rect(0, 84, SCREEN_W, 1, C_BORDER);
}

/* Footer: button hints (glyph, label pairs, NULL-terminated), then the toast or a note. */
static void draw_footer(const char *const *hints, const char *note) {
    ui_rect(0, 668, SCREEN_W, 1, C_BORDER);
    int x = 32;
    for (int i = 0; hints[i]; i += 2) x += ui_hint(x, 684, hints[i], hints[i + 1]);
    if (g_toast[0] && now_ns() < g_toast_until)
        ui_text(20, SCREEN_W - 32, 685, g_toast_color, ALIGN_RIGHT, SCREEN_W - x - 48, g_toast);
    else if (note && note[0])
        ui_text(16, SCREEN_W - 32, 688, C_MUTED, ALIGN_RIGHT, SCREEN_W - x - 48, note);
}

/* ---------- Home screen ---------- */

static void draw_row(const Item *it, int x, int y, bool selected) {
    int h = ROW_H - 10;
    ui_round_rect(x, y, LIST_W, h, 12, selected ? C_PANEL2 : C_PANEL);
    if (selected) ui_round_border(x, y, LIST_W, h, 12, C_ACCENT);
    ui_rect(x + 14, y + 14, 5, h - 28, game_color(it->game, 0));

    const char *label;
    SDL_Color color;
    state_label(it->state, &label, &color);
    int pill_w = ui_text_width(16, label) + 24;
    ui_text(22, x + 34, y + 9, C_TEXT, ALIGN_LEFT, LIST_W - 34 - pill_w - 28, short_name(it->game));
    char size[32] = "";
    if (it->size) format_size(it->size, size, sizeof size);
    ui_textf(16, x + 34, y + 39, C_MUTED, ALIGN_LEFT, "%s%s%s", it->is_nds ? "DS (melonDS)" : "Switch",
             size[0] ? "  ·  " : "", size);
    ui_pill(x + LIST_W - 14, y + 19, ALIGN_RIGHT, color, label);
}

static void draw_stat(int x, int y, int w, const char *label, const char *value) {
    ui_round_rect(x, y, w, 80, 12, C_PANEL2);
    ui_text(16, x + 16, y + 12, C_MUTED, ALIGN_LEFT, 0, label);
    ui_text(24, x + 16, y + 38, C_TEXT, ALIGN_LEFT, w - 32, value);
}

static void draw_party(const SaveSummary *s, int x, int y) {
    ui_text(20, x, y, C_TEXT, ALIGN_LEFT, 0, "Équipe");
    int w = (PANEL_W - 48 - 5 * 12) / 6;
    for (int i = 0; i < PARTY_SIZE; i++) {
        int cx = x + i * (w + 12), cy = y + 34;
        if (i >= s->party_count) {
            ui_round_border(cx, cy, w, 170, 12, C_PANEL2);
            continue;
        }
        const Mon *m = &s->party[i];
        ui_round_rect(cx, cy, w, 170, 12, C_PANEL2);
        draw_sprite(m, cx + 4, cy + 4, w - 8, 108);
        if (m->shiny) ui_circle(cx + w - 14, cy + 14, 5, C_AMBER);
        ui_text(18, cx + w / 2, cy + 114, C_TEXT, ALIGN_CENTER, w - 12, mon_name(m));
        if (m->level) ui_textf(16, cx + w / 2, cy + 140, C_MUTED, ALIGN_CENTER, "N. %d", m->level);
    }
}

static void draw_message(int y, const char *title, const char *text) {
    ui_text(24, PANEL_X + PANEL_W / 2, y, C_TEXT, ALIGN_CENTER, PANEL_W - 64, title);
    ui_text_wrap(20, PANEL_X + 48, y + 44, PANEL_W - 96, C_MUTED, ALIGN_CENTER, text);
}

static void draw_detail(const Item *it) {
    int x = PANEL_X, y = PANEL_Y;
    ui_round_rect(x, y, PANEL_W, PANEL_H, 16, C_PANEL);

    /* Banner: game colors, box art, name, state and last backup. */
    ui_gradient(x + 16, y + 16, PANEL_W - 32, 152, ui_mix(game_color(it->game, 0), C_PANEL, 150),
                ui_mix(game_color(it->game, 1), C_PANEL, 150));
    char url[320];
    snprintf(url, sizeof url, "%s/assets/pokemon-saves/boxart/%s.jpg", g_cfg.url, it->game->id);
    SDL_Texture *art = image_get(url);
    int text_x = x + 40;
    if (art) {
        ui_image(art, x + 32, y + 28, 128, 128);
        text_x = x + 180;
    }
    ui_text(32, text_x, y + 30, C_TEXT, ALIGN_LEFT, x + PANEL_W - 40 - text_x, it->game->name);
    const char *label;
    SDL_Color color;
    state_label(it->state, &label, &color);
    int pw = ui_pill(text_x, y + 80, ALIGN_LEFT, color, label);
    char size[32] = "";
    if (it->size) format_size(it->size, size, sizeof size);
    ui_textf(18, text_x + pw + 14, y + 83, C_TEXT, ALIGN_LEFT, "%s%s%s", it->is_nds ? "DS (melonDS)" : "Switch",
             size[0] ? "  ·  " : "", size);
    if (it->has_remote) {
        char date[48];
        format_date(it->remote.updated_at, date, sizeof date);
        ui_textf(18, text_x, y + 122, C_TEXT, ALIGN_LEFT, "Dernier backup : %s  ·  %d version%s", date,
                 it->remote.versions, it->remote.versions > 1 ? "s" : "");
    } else {
        ui_text(18, text_x, y + 122, C_TEXT, ALIGN_LEFT, 0, "Jamais sauvegardée sur le serveur");
    }

    int body_y = y + 188;
    if (it->state == ST_ERROR && it->msg[0]) {
        ui_round_rect(x + 24, body_y, PANEL_W - 48, 48, 10, ui_mix(C_RED, C_PANEL, 40));
        ui_text(18, x + 40, body_y + 13, C_RED, ALIGN_LEFT, PANEL_W - 80, it->msg);
        body_y += 64;
    }
    const SaveSummary *s = &it->remote;
    if (!it->has_remote) {
        draw_message(body_y + 90, "Pas encore sur le serveur", "Appuie sur A pour envoyer cette save.");
        return;
    }
    if (!it->game->auto_read || !s->trainer[0]) {
        draw_message(body_y + 90, "Save stockée, sans résumé",
                     "Le serveur garde et versionne cette save, mais ne sait pas encore lire ce jeu : "
                     "pas de dresseur ni d'équipe à afficher.");
        return;
    }

    char play[32] = "—", money[32] = "—", badges[16] = "—";
    if (s->play_hours >= 0) snprintf(play, sizeof play, "%d h %02d", s->play_hours, s->play_minutes);
    if (s->money >= 0) format_number(s->money, money, sizeof money);
    if (s->badges >= 0) snprintf(badges, sizeof badges, "%d", s->badges);
    int w = (PANEL_W - 48 - 3 * 12) / 4;
    draw_stat(x + 24, body_y, w, "Dresseur", s->trainer);
    draw_stat(x + 24 + (w + 12), body_y, w, "Temps de jeu", play);
    draw_stat(x + 24 + 2 * (w + 12), body_y, w, "Argent", money);
    draw_stat(x + 24 + 3 * (w + 12), body_y, w, "Badges", badges);
    if (s->location[0]) ui_textf(18, x + 24, body_y + 92, C_MUTED, ALIGN_LEFT, "Lieu : %s", s->location);
    draw_party(s, x + 24, body_y + (s->location[0] ? 124 : 100));
}

static void draw_home(void) {
    size_t count;
    Item *items = saves_items(&count);
    if (count == 0) {
        ui_round_rect(LIST_X, LIST_Y, SCREEN_W - 64, PANEL_H, 16, C_PANEL);
        ui_text(28, SCREEN_W / 2, 300, C_TEXT, ALIGN_CENTER, 0, "Aucune save Pokémon trouvée");
        ui_text_wrap(20, 200, 350, SCREEN_W - 400, C_MUTED, ALIGN_CENTER,
                     "Aucun jeu Pokémon du catalogue n'a de save pour ce profil, et aucun .sav melonDS reconnu "
                     "dans le dossier configuré.");
        return;
    }
    for (size_t i = g_top; i < count && i < g_top + VISIBLE_ROWS; i++)
        draw_row(&items[i], LIST_X, LIST_Y + (int)(i - g_top) * ROW_H, i == g_cursor);
    if (g_top + VISIBLE_ROWS < count)
        ui_textf(16, LIST_X + LIST_W / 2, LIST_Y + VISIBLE_ROWS * ROW_H - 4, C_MUTED, ALIGN_CENTER,
                 "%zu autre(s) plus bas", count - g_top - VISIBLE_ROWS);
    draw_detail(&items[g_cursor]);
}

/* ---------- Box screen ---------- */

static const Mon *page_mon(int index) {
    const Page *p = &g_pages[g_page];
    return index < p->count ? &g_detail.boxes[p->box].mons[p->start + index] : NULL;
}

static void draw_boxes(void) {
    const Page *p = &g_pages[g_page];
    const Box *box = &g_detail.boxes[p->box];
    const char *name = box->name[0] ? box->name : "Boîte";
    char title[96];
    if (box->count > BOX_PAGE)
        snprintf(title, sizeof title, "%s  (%d/%d)", name, p->start / BOX_PAGE + 1, (box->count + BOX_PAGE - 1) / BOX_PAGE);
    else
        snprintf(title, sizeof title, "%s", name);

    int gx = LIST_X, gy = 156, cw = 108, ch = 96, grid_w = BOX_COLS * cw;
    ui_hint(gx + 4, 108, GLYPH_L, "");
    ui_hint(gx + grid_w - 34, 108, GLYPH_R, "");
    ui_text(24, gx + grid_w / 2, 100, C_TEXT, ALIGN_CENTER, grid_w - 120, title);
    char sub[128];
    snprintf(sub, sizeof sub, "%s  ·  page %d / %d", g_box_item->game->name, g_page + 1, g_page_count);
    ui_text(16, gx + grid_w / 2, 130, C_MUTED, ALIGN_CENTER, grid_w - 120, sub);

    for (int i = 0; i < BOX_PAGE; i++) {
        int cx = gx + (i % BOX_COLS) * cw, cy = gy + (i / BOX_COLS) * ch;
        bool selected = i == g_box_cursor;
        ui_round_rect(cx + 3, cy + 3, cw - 6, ch - 6, 10, selected ? C_PANEL2 : C_PANEL);
        if (selected) ui_round_border(cx + 3, cy + 3, cw - 6, ch - 6, 10, C_ACCENT);
        const Mon *m = page_mon(i);
        if (!m) continue;
        draw_sprite(m, cx + 6, cy + 4, cw - 12, ch - 8);
        if (m->shiny) ui_circle(cx + cw - 16, cy + 16, 5, C_AMBER);
    }

    /* Selected Pokémon */
    int px = 704, pw = SCREEN_W - 32 - px;
    ui_round_rect(px, PANEL_Y, pw, PANEL_H, 16, C_PANEL);
    const Mon *m = page_mon(g_box_cursor);
    if (!m) {
        ui_text(22, px + pw / 2, 340, C_MUTED, ALIGN_CENTER, 0, "Emplacement vide");
    } else {
        ui_round_rect(px + (pw - 240) / 2, 124, 240, 240, 120, C_PANEL2);
        draw_sprite(m, px + (pw - 240) / 2, 124, 240, 240);
        ui_text(32, px + pw / 2, 384, C_TEXT, ALIGN_CENTER, pw - 48, mon_name(m));
        int y = 430;
        if (!m->egg && m->species[0] && m->nickname[0] && strcmp(m->species, m->nickname)) {
            ui_text(20, px + pw / 2, y, C_MUTED, ALIGN_CENTER, pw - 48, m->species);
            y += 32;
        }
        if (m->dex && !m->egg) {
            ui_textf(20, px + pw / 2, y, C_MUTED, ALIGN_CENTER, "N° %03d", m->dex);
            y += 32;
        }
        if (m->level) {
            ui_textf(20, px + pw / 2, y, C_TEXT, ALIGN_CENTER, "Niveau %d", m->level);
            y += 32;
        }
        if (m->ot[0]) {
            char ot[96];
            snprintf(ot, sizeof ot, "Dresseur d'origine : %s", m->ot);
            ui_text(18, px + pw / 2, y, C_MUTED, ALIGN_CENTER, pw - 48, ot);
            y += 32;
        }
        if (m->shiny) ui_pill(px + pw / 2, y + 4, ALIGN_CENTER, C_AMBER, "Chromatique");
    }
    if (g_detail.dex_owned >= 0)
        ui_textf(16, px + pw / 2, PANEL_Y + PANEL_H - 36, C_MUTED, ALIGN_CENTER, "Pokédex : %d vus  ·  %d capturés",
                 g_detail.dex_seen, g_detail.dex_owned);
}

/* ---------- Frames ---------- */

/* Draws the current screen without presenting it. */
static void draw_screen(void) {
    ui_begin();
    draw_header();
    if (g_screen == SCREEN_HOME) {
        static const char *const hints[] = {GLYPH_A, "Envoyer", GLYPH_X, "Tout envoyer", GLYPH_Y, "Boîtes",
                                            GLYPH_PLUS, "Quitter", NULL};
        char note[96] = "";
        if (saves_skipped_nds())
            snprintf(note, sizeof note, "%d .sav ignoré(s) : ROM absente ou jeu inconnu", saves_skipped_nds());
        draw_home();
        draw_footer(hints, note);
    } else if (g_screen == SCREEN_BOXES) {
        static const char *const hints[] = {GLYPH_L, "", GLYPH_R, "Boîte", GLYPH_DPAD, "Choisir", GLYPH_B, "Retour",
                                            NULL};
        draw_boxes();
        draw_footer(hints, NULL);
    }
}

/* Shows a busy card over the current screen, for blocking tasks. */
static void busy(const char *title, const char *subtitle) {
    draw_screen();
    ui_overlay(title, subtitle);
}

static void scan_progress(const char *what) {
    busy("Lecture des saves…", what);
}

/* Error card. Waits for + before returning. */
static void fatal(const char *fmt, ...) {
    char message[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    static const char *const hints[] = {GLYPH_PLUS, "Quitter", NULL};
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
        ui_begin();
        draw_header();
        int w = 760, h = 300, x = (SCREEN_W - w) / 2, y = 190;
        ui_round_rect(x, y, w, h, 18, C_PANEL);
        ui_round_border(x, y, w, h, 18, C_BORDER);
        ui_circle(SCREEN_W / 2, y + 56, 26, ui_mix(C_RED, C_PANEL, 60));
        ui_text(30, SCREEN_W / 2, y + 38, C_RED, ALIGN_CENTER, 0, "!");
        ui_text(28, SCREEN_W / 2, y + 104, C_TEXT, ALIGN_CENTER, 0, "Une erreur est survenue");
        ui_text_wrap(20, x + 48, y + 152, w - 96, C_MUTED, ALIGN_CENTER, message);
        draw_footer(hints, NULL);
        ui_end();
    }
}

/* ---------- Pairing ---------- */

static void draw_pairing(const char *code, const char *error) {
    ui_begin();
    draw_header();
    int w = 820, h = 470, x = (SCREEN_W - w) / 2, y = 120;
    ui_round_rect(x, y, w, h, 18, C_PANEL);
    ui_text(30, SCREEN_W / 2, y + 32, C_TEXT, ALIGN_CENTER, 0, "Appairage de cette Switch");
    ui_text(20, SCREEN_W / 2, y + 84, C_MUTED, ALIGN_CENTER, 0, "Sur ton téléphone ou ton PC, ouvre :");
    char url[300];
    snprintf(url, sizeof url, "%s/apps/pokemon-saves/pair", g_cfg.url);
    ui_text(22, SCREEN_W / 2, y + 118, C_BLUE, ALIGN_CENTER, w - 48, url);
    ui_text(20, SCREEN_W / 2, y + 170, C_MUTED, ALIGN_CENTER, 0, "puis saisis ce code :");

    int n = strlen(code), cell = 92, cx = SCREEN_W / 2 - n * cell / 2;
    for (int i = 0; i < n; i++) {
        char ch[2] = {code[i], 0};
        ui_round_rect(cx + i * cell + 6, y + 214, cell - 12, 110, 14, C_PANEL2);
        ui_text(64, cx + i * cell + cell / 2, y + 226, C_TEXT, ALIGN_CENTER, 0, ch);
    }
    Uint32 t = SDL_GetTicks() / 300;
    for (int i = 0; i < 3; i++)
        ui_circle(SCREEN_W / 2 - 24 + i * 24, y + 370, 6, (int)(t % 3) == i ? C_ACCENT : C_BORDER);
    ui_text(18, SCREEN_W / 2, y + 396, error ? C_RED : C_MUTED, ALIGN_CENTER, w - 48,
            error ? error : "En attente de validation. Le code se renouvelle tout seul après 10 minutes.");
    static const char *const hints[] = {GLYPH_PLUS, "Annuler", NULL};
    draw_footer(hints, NULL);
    ui_end();
}

/*
 * Pairs the Switch: shows a short code to validate on the website, polls until
 * the device key arrives, then stores it on the SD card. Returns false if cancelled.
 */
static bool pair_device(void) {
    char device[96];
    snprintf(device, sizeof device, "Switch (%s)", g_nickname);
    while (true) {
        busy("Connexion au serveur…", "Demande d'un code d'appairage");
        char code[16], pairing_id[128];
        int interval;
        ApiResult r = api_pair_start(device, code, pairing_id, &interval);
        if (r.status != 201) {
            fatal("Appairage : %s", r.error);
            return false;
        }

        u64 next_poll = now_ns() + (u64)interval * 1000000000ULL;
        char error[200] = "";
        while (true) {
            if (!appletMainLoop()) return false;
            padUpdate(&g_pad);
            if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) return false;
            draw_pairing(code, error[0] ? error : NULL);
            if (now_ns() < next_poll) continue;

            bool approved;
            char key[256];
            r = api_pair_poll(pairing_id, &approved, key);
            next_poll = now_ns() + (u64)interval * 1000000000ULL;
            if (r.status == 404) break; /* expired: new code */
            if (r.status != 200) {
                snprintf(error, sizeof error, "%s. Nouvel essai…", r.error);
            } else if (approved) {
                snprintf(g_cfg.api_key, sizeof g_cfg.api_key, "%s", key);
                g_cfg.key_from_file = true;
                api_set_key(key);
                if (!config_save_key(key))
                    toast(C_AMBER, "Clé non enregistrée sur la SD : il faudra appairer de nouveau.");
                return true;
            } else {
                error[0] = '\0';
            }
        }
    }
}

/* ---------- Profile ---------- */

static void load_avatar(AccountProfile *profile) {
    u32 size = 0, out_size = 0;
    if (R_FAILED(accountProfileGetImageSize(profile, &size)) || !size) return;
    void *jpeg = malloc(size);
    if (jpeg && R_SUCCEEDED(accountProfileLoadImage(profile, jpeg, size, &out_size)))
        g_avatar = IMG_LoadTexture_RW(ui_renderer(), SDL_RWFromConstMem(jpeg, out_size), 1);
    free(jpeg);
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
                snprintf(err, err_size, "Aucun profil choisi (sélection annulée).");
                return false;
            }
            if (R_FAILED(rc) && (R_FAILED(accountGetLastOpenedUser(&g_uid)) || !accountUidIsValid(&g_uid))) {
                snprintf(err, err_size, "Aucun profil choisi (sélecteur : 0x%X, %d profil(s)).", rc, user_count);
                return false;
            }
        }
    }
    AccountProfile profile;
    AccountProfileBase base = {0};
    if (R_SUCCEEDED(accountGetProfile(&profile, g_uid))) {
        accountProfileGet(&profile, NULL, &base);
        load_avatar(&profile);
        accountProfileClose(&profile);
    }
    snprintf(g_nickname, sizeof g_nickname, "%s", base.nickname);
    return true;
}

/* ---------- Actions ---------- */

static void upload_one(Item *it) {
    char title[128], result[200];
    snprintf(title, sizeof title, "Envoi de %s…", it->game->name);
    busy(title, it->is_nds ? "Lecture du .sav puis envoi" : "Compression de la save puis envoi");
    if (saves_upload(it, result, sizeof result) == 401) g_key_refused = true;
    toast(it->state == ST_SENT ? C_GREEN : C_RED, "%s : %s", short_name(it->game), result);
}

static void upload_all(void) {
    size_t count, todo = 0, done = 0;
    Item *items = saves_items(&count);
    for (size_t i = 0; i < count; i++) todo += items[i].state == ST_NEW || items[i].state == ST_CHANGED;
    if (!todo) {
        toast(C_GREEN, "Tout est déjà à jour");
        return;
    }
    int sent = 0, errors = 0;
    for (size_t i = 0; i < count && !g_key_refused; i++) {
        Item *it = &items[i];
        if (it->state != ST_NEW && it->state != ST_CHANGED) continue;
        char title[64], result[200];
        snprintf(title, sizeof title, "Envoi %zu / %zu", ++done, todo);
        busy(title, it->game->name);
        if (saves_upload(it, result, sizeof result) == 401) g_key_refused = true;
        if (it->state == ST_SENT) sent++;
        else errors++;
    }
    toast(errors ? C_AMBER : C_GREEN, "%d envoyée(s), %d erreur(s)", sent, errors);
}

/* Loads the boxes of the server save and splits them into pages of BOX_PAGE. */
static void open_boxes(const Item *it) {
    if (!it->has_remote) {
        toast(C_AMBER, "Envoie d'abord cette save pour voir ses boîtes");
        return;
    }
    if (!it->game->auto_read) {
        toast(C_AMBER, "Le serveur ne sait pas encore lire les boîtes de ce jeu");
        return;
    }
    busy("Chargement des boîtes…", it->game->name);
    save_detail_free(&g_detail);
    ApiResult r = api_get_save_detail(it->remote.id, &g_detail);
    if (r.status != 200) {
        toast(C_RED, "Boîtes : %s", r.error);
        return;
    }
    free(g_pages);
    g_pages = NULL;
    g_page_count = 0;
    for (int b = 0; b < g_detail.box_count; b++)
        g_page_count += SDL_max(1, (g_detail.boxes[b].count + BOX_PAGE - 1) / BOX_PAGE);
    if (!g_page_count) {
        toast(C_AMBER, "Aucune boîte dans cette save");
        return;
    }
    g_pages = calloc(g_page_count, sizeof *g_pages);
    int n = 0;
    for (int b = 0; b < g_detail.box_count; b++) {
        int count = g_detail.boxes[b].count, start = 0;
        do {
            g_pages[n++] = (Page){b, start, SDL_min(BOX_PAGE, count - start)};
            start += BOX_PAGE;
        } while (start < count);
    }
    g_box_item = it;
    g_page = g_box_cursor = 0;
    g_screen = SCREEN_BOXES;
}

/* ---------- Input ---------- */

#define DIRECTIONS (HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight)

/* Pressed buttons, with auto-repeat on held directions. */
static u64 read_buttons(void) {
    static u64 repeat_at;
    padUpdate(&g_pad);
    u64 down = padGetButtonsDown(&g_pad), held = padGetButtons(&g_pad) & DIRECTIONS;
    u64 now = now_ns();
    if (down & DIRECTIONS) {
        repeat_at = now + 400000000ULL;
    } else if (held && now >= repeat_at) {
        down |= held;
        repeat_at = now + 90000000ULL;
    }
    return down;
}

/* Returns false when the user quits. */
static bool update_home(u64 b) {
    size_t count;
    Item *items = saves_items(&count);
    if (b & HidNpadButton_Plus) return false;
    if (!count) return true;
    if ((b & HidNpadButton_AnyDown) && g_cursor + 1 < count) g_cursor++;
    if ((b & HidNpadButton_AnyUp) && g_cursor > 0) g_cursor--;
    if (g_cursor < g_top) g_top = g_cursor;
    if (g_cursor >= g_top + VISIBLE_ROWS) g_top = g_cursor - VISIBLE_ROWS + 1;

    if (g_key_refused && (b & (HidNpadButton_A | HidNpadButton_X | HidNpadButton_Y))) {
        toast(C_RED, "Clé refusée (révoquée ?) : relance l'app pour appairer de nouveau.");
        return true;
    }
    if (b & HidNpadButton_A) upload_one(&items[g_cursor]);
    else if (b & HidNpadButton_X) upload_all();
    else if (b & HidNpadButton_Y) open_boxes(&items[g_cursor]);
    return true;
}

static void update_boxes(u64 b) {
    if (b & (HidNpadButton_B | HidNpadButton_Plus)) {
        g_screen = SCREEN_HOME;
        return;
    }
    int col = g_box_cursor % BOX_COLS, row = g_box_cursor / BOX_COLS;
    if (b & HidNpadButton_AnyLeft) col = (col + BOX_COLS - 1) % BOX_COLS;
    if (b & HidNpadButton_AnyRight) col = (col + 1) % BOX_COLS;
    if (b & HidNpadButton_AnyUp) row = (row + BOX_ROWS - 1) % BOX_ROWS;
    if (b & HidNpadButton_AnyDown) row = (row + 1) % BOX_ROWS;
    g_box_cursor = row * BOX_COLS + col;
    if (b & (HidNpadButton_R | HidNpadButton_ZR)) g_page = (g_page + 1) % g_page_count;
    if (b & (HidNpadButton_L | HidNpadButton_ZL)) g_page = (g_page + g_page_count - 1) % g_page_count;
}

/* ---------- Main ---------- */

static void run(void) {
    char err[160];
    busy("Démarrage…", NULL);
    if (!config_load(&g_cfg, err, sizeof err)) { fatal("%s", err); return; }
    if (!api_init(&g_cfg)) { fatal("Initialisation réseau impossible (romfs:/isrg-roots.pem manquant ?)"); return; }
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) { fatal("Service des comptes indisponible (0x%X)", rc); return; }
    if (!choose_user(err, sizeof err)) { fatal("%s", err); return; }
    if (!g_cfg.api_key[0] && !pair_device()) return;

    busy("Connexion au serveur…", g_cfg.url);
    ApiResult r = api_get_games(&g_games, &g_game_count);
    if (r.status == 401 && g_cfg.key_from_file) {
        /* The device key was revoked on the website: pair again. */
        config_forget_key();
        g_cfg.api_key[0] = '\0';
        if (!pair_device()) return;
        busy("Connexion au serveur…", g_cfg.url);
        r = api_get_games(&g_games, &g_game_count);
    }
    if (r.status == 401) { fatal("Clé API refusée : vérifie api_key dans %s", CONFIG_PATH); return; }
    if (r.status != 200) { fatal("Catalogue : %s", r.error); return; }
    busy("Connexion au serveur…", "Lecture des backups existants");
    r = api_get_saves(g_cfg.slot, &g_saves, &g_save_count);
    if (r.status != 200) { fatal("Backups : %s", r.error); return; }

    g_screen = SCREEN_HOME;
    saves_scan(&g_cfg, g_uid, g_games, g_game_count, g_saves, g_save_count, scan_progress);

    while (appletMainLoop()) {
        u64 b = read_buttons();
        if (g_screen == SCREEN_HOME) {
            if (!update_home(b)) break;
        } else {
            update_boxes(b);
        }
        draw_screen();
        ui_end();
    }
}

int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    romfsInit();
    socketInitializeDefault();

    if (ui_init() && images_init(ui_renderer())) run();

    save_detail_free(&g_detail);
    free(g_pages);
    images_exit();
    api_exit();
    if (g_avatar) SDL_DestroyTexture(g_avatar);
    ui_exit();
    free(g_games);
    free(g_saves);
    accountExit();
    socketExit();
    romfsExit();
    return 0;
}
