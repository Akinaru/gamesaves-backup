#pragma once

#include <SDL2/SDL.h>
#include <stdbool.h>

#define SCREEN_W 1280
#define SCREEN_H 720

/* Palette */
#define C_BG       ((SDL_Color){15, 17, 23, 255})
#define C_PANEL    ((SDL_Color){26, 29, 39, 255})
#define C_PANEL2   ((SDL_Color){35, 39, 54, 255})
#define C_BORDER   ((SDL_Color){50, 56, 74, 255})
#define C_TEXT     ((SDL_Color){232, 234, 240, 255})
#define C_MUTED    ((SDL_Color){139, 145, 165, 255})
#define C_ACCENT   ((SDL_Color){227, 53, 13, 255})
#define C_GREEN    ((SDL_Color){52, 199, 89, 255})
#define C_AMBER    ((SDL_Color){245, 166, 35, 255})
#define C_RED      ((SDL_Color){255, 69, 58, 255})
#define C_BLUE     ((SDL_Color){76, 141, 255, 255})
#define C_DIM      ((SDL_Color){0, 0, 0, 170})

/* Nintendo button glyphs (shared "NintendoExt" font). */
#define GLYPH_A     "\xee\x83\xa0"
#define GLYPH_B     "\xee\x83\xa1"
#define GLYPH_X     "\xee\x83\xa2"
#define GLYPH_Y     "\xee\x83\xa3"
#define GLYPH_L     "\xee\x83\xa4"
#define GLYPH_R     "\xee\x83\xa5"
#define GLYPH_DPAD  "\xee\x83\xaa"
#define GLYPH_PLUS  "\xee\x83\xaf"

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } Align;

bool ui_init(void);
void ui_exit(void);
SDL_Renderer *ui_renderer(void);

void ui_begin(void);   /* clears the frame */
void ui_end(void);     /* presents it */

/* Draws UTF-8 text with its top at y. Returns the drawn width. max_w > 0 shortens it with "…". */
int ui_text(int size, int x, int y, SDL_Color color, Align align, int max_w, const char *text);
int ui_textf(int size, int x, int y, SDL_Color color, Align align, const char *fmt, ...);
int ui_text_width(int size, const char *text);

/* Draws text wrapped on words inside width w. Returns the height used. */
int ui_text_wrap(int size, int x, int y, int w, SDL_Color color, Align align, const char *text);

void ui_rect(int x, int y, int w, int h, SDL_Color color);
void ui_round_rect(int x, int y, int w, int h, int radius, SDL_Color color);
void ui_round_border(int x, int y, int w, int h, int radius, SDL_Color color);
void ui_circle(int x, int y, int r, SDL_Color color);
void ui_gradient(int x, int y, int w, int h, SDL_Color left, SDL_Color right);

/* Draws tex scaled to fit inside the box, centered. */
void ui_image(SDL_Texture *tex, int x, int y, int w, int h);

/* Small rounded label, e.g. a status. Returns its width. */
int ui_pill(int x, int y, Align align, SDL_Color color, const char *text);

/* Button glyph followed by a label. Returns the width used. */
int ui_hint(int x, int y, const char *glyph, const char *label);

/* Dimmed screen with a centered card: used while a blocking task runs. Presents immediately. */
void ui_overlay(const char *title, const char *subtitle);

SDL_Color ui_rgb(unsigned rgb);

/* Opaque mix of color over base: amount 0 gives base, 255 gives color. */
SDL_Color ui_mix(SDL_Color color, SDL_Color base, int amount);
