#pragma once

#include <SDL2/SDL.h>
#include <stdbool.h>

/*
 * Remote images (sprites, box art) cached on the SD card. A background thread
 * downloads them; the render thread turns cached files into textures.
 */
bool images_init(SDL_Renderer *renderer);
void images_exit(void);

/* Texture for url, or NULL while it downloads or if it failed. Render thread only. */
SDL_Texture *image_get(const char *url);

/* Pokémon sprite URL (PokeAPI). dex 0 with egg false gives an empty string. */
void sprite_url(int dex, bool shiny, bool egg, char *out, size_t size);
