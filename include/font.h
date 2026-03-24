#ifndef FONT_H
#define FONT_H

/*
 * font.h — FreeType glyph cache interface.
 *
 * Declares the Font struct, Glyph struct, and all functions
 * for loading a .ttf file and rendering text with SDL2.
 */

#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H

/* Number of ASCII codepoints cached (0–127) */
#define GLYPH_COUNT 128

/*
 * Glyph — cached data for one character.
 *
 * texture   — SDL2 GPU texture of this glyph (white, RGBA)
 * width     — bitmap width in pixels
 * height    — bitmap height in pixels
 * bearing_x — horizontal offset from pen position to glyph left
 * bearing_y — vertical offset from baseline to glyph top
 *             (positive = above baseline)
 * advance   — how far to move the pen after drawing this glyph
 */
typedef struct {
    SDL_Texture *texture;
    int          width;
    int          height;
    int          bearing_x;
    int          bearing_y;
    int          advance;
} Glyph;

/*
 * Font — the loaded font face and its glyph cache.
 *
 * library    — FreeType library handle
 * face       — the loaded font face (one .ttf file)
 * cache[]    — one Glyph entry per ASCII codepoint
 * size       — requested font size in pixels
 * cell_width — width  of one character cell (= 'M'.advance)
 * cell_height— height of one character cell (ascender+descender+leading)
 * ascender   — pixels from baseline to top of tallest glyph
 *              Used in font_draw_char() for correct vertical placement.
 *              Without this field, descenders like p/g/y render wrong.
 */
typedef struct {
    FT_Library  library;
    FT_Face     face;
    Glyph       cache[GLYPH_COUNT];
    int         size;
    int         cell_width;
    int         cell_height;
    int         ascender;    /* font-wide ascender in pixels */
} Font;

/* ── API ── */

/*
 * font_init — load a .ttf file and pre-render all printable
 * ASCII characters into GPU textures.
 * Returns 0 on success, -1 on failure.
 */
int  font_init(Font *f, SDL_Renderer *renderer,
               const char *path, int size);

/*
 * font_draw_char — draw one character at cell position (x, y).
 * x, y = top-left of the character cell.
 * r, g, b = text color (0–255 each).
 */
void font_draw_char(Font *f, SDL_Renderer *renderer,
                    char c, int x, int y,
                    Uint8 r, Uint8 g, Uint8 b);

/*
 * font_draw_string — draw a null-terminated string at (x, y).
 * Advances x by each character's advance width.
 */
void font_draw_string(Font *f, SDL_Renderer *renderer,
                      const char *str, int x, int y,
                      Uint8 r, Uint8 g, Uint8 b);

/*
 * font_destroy — free all GPU textures and FreeType resources.
 * Call before exiting.
 */
void font_destroy(Font *f);

#endif /* FONT_H */