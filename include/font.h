#ifndef FONT_H
#define FONT_H

#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H

/*
 * How many ASCII characters we cache.
 * ASCII 32 = space, ASCII 126 = '~'
 * That covers all printable characters.
 */
#define GLYPH_COUNT 128

/*
 * One entry in our glyph cache.
 * Stores everything we need to draw a single character.
 */
typedef struct {
    SDL_Texture *texture;  /* GPU texture of this glyph          */
    int          width;    /* Width of the glyph in pixels        */
    int          height;   /* Height of the glyph in pixels       */
    int          bearing_x; /* Horizontal offset from cursor pos  */
    int          bearing_y; /* Vertical offset from baseline      */
    int          advance;   /* How far to move cursor after char  */
} Glyph;

/*
 * The Font struct holds the FreeType library state
 * and our entire glyph cache.
 */
typedef struct {
    FT_Library  library;           /* FreeType library handle          */
    FT_Face     face;              /* The loaded font face             */
    Glyph       cache[GLYPH_COUNT]; /* One cached glyph per character  */
    int         size;              /* Font size in pixels              */
    int         cell_width;        /* Width of one character cell      */
    int         cell_height;       /* Height of one character cell     */
} Font;

/* Function declarations */
int  font_init(Font *f, SDL_Renderer *renderer,
               const char *path, int size);
void font_draw_char(Font *f, SDL_Renderer *renderer,
                    char c, int x, int y,
                    Uint8 r, Uint8 g, Uint8 b);
void font_draw_string(Font *f, SDL_Renderer *renderer,
                      const char *str, int x, int y,
                      Uint8 r, Uint8 g, Uint8 b);
void font_destroy(Font *f);

#endif /* FONT_H */