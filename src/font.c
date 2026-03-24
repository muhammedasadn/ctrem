/*
 * font.c — FreeType glyph cache and text rendering.
 *
 * Loads a .ttf font file, rasterises every printable ASCII
 * character into SDL2 GPU textures, and caches them.
 * Drawing a character is then just a fast GPU texture blit.
 *
 * Fix for "p/g/y look wrong":
 *   The vertical position of each glyph must be calculated
 *   from the font's ascender (how high above the baseline
 *   the tallest character reaches). We use:
 *
 *     glyph_y = cell_top + ascender - bearing_y
 *
 *   Where:
 *     cell_top   = top of the cell in pixels (y argument)
 *     ascender   = font-wide value: pixels above baseline
 *     bearing_y  = glyph-specific: pixels above baseline
 *
 *   This places every glyph on a shared invisible baseline,
 *   so "A" and "p" align correctly even though "p" has a
 *   descender that hangs below the baseline.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ── font_init ──────────────────────────────────────────────── */

int font_init(Font *f, SDL_Renderer *renderer,
              const char *path, int size) {

    memset(f, 0, sizeof(Font));
    f->size = size;

    /* ── Step 1: Init FreeType library ── */
    if (FT_Init_FreeType(&f->library) != 0) {
        fprintf(stderr, "font_init: FT_Init_FreeType failed\n");
        return -1;
    }

    /* ── Step 2: Load the font file ── */
    if (FT_New_Face(f->library, path, 0, &f->face) != 0) {
        fprintf(stderr, "font_init: FT_New_Face failed: %s\n", path);
        FT_Done_FreeType(f->library);
        return -1;
    }

    /* ── Step 3: Set pixel size ── */
    /*
     * FT_Set_Pixel_Sizes(face, width, height)
     * Passing 0 for width tells FreeType to auto-calculate
     * width from the height, preserving the font's aspect ratio.
     */
    FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)size);

    /*
     * ── Step 4: Read font-wide metrics ──
     *
     * FreeType metrics are in 26.6 fixed-point format.
     * That means the value is in units of 1/64 pixel.
     * Shifting right by 6 (dividing by 64) converts to pixels.
     *
     * ascender  — distance from baseline to top of tallest glyph
     *             (positive, e.g. top of 'H' or 'd')
     * descender — distance from baseline to bottom of deepest glyph
     *             (negative in FreeType, e.g. bottom of 'p' or 'g')
     * height    — total line height including leading (line gap)
     */
    int ascender  = (int)(f->face->size->metrics.ascender  >> 6);
    int descender = (int)(f->face->size->metrics.descender >> 6);
    /* descender is negative — negate it to get the depth below baseline */
    if (descender < 0) descender = -descender;

    f->ascender = ascender;  /* store for use in font_draw_char */

    /* ── Step 5: Rasterise every printable ASCII character ── */
    for (int c = 32; c < GLYPH_COUNT; c++) {

        /*
         * FT_LOAD_RENDER — load the glyph outline AND rasterise
         * it into a bitmap in one step.
         * After this call, f->face->glyph->bitmap holds pixels.
         */
        if (FT_Load_Char(f->face, (FT_ULong)c, FT_LOAD_RENDER) != 0) {
            /* Skip characters that fail to load */
            continue;
        }

        FT_GlyphSlot slot = f->face->glyph;
        FT_Bitmap   *bmp  = &slot->bitmap;

        /*
         * Store glyph metrics.
         *
         * bearing_x — horizontal offset from the pen position
         *             to the left edge of the glyph bitmap.
         *             Positive = glyph starts to the right of pen.
         *
         * bearing_y — vertical offset from the baseline to the
         *             TOP of the glyph bitmap (positive = above
         *             baseline). Used for vertical placement.
         *
         * advance   — how far to move the pen after drawing.
         *             In 26.6 fixed-point — shift right 6 = pixels.
         */
        f->cache[c].bearing_x = slot->bitmap_left;
        f->cache[c].bearing_y = slot->bitmap_top;
        f->cache[c].advance   = (int)(slot->advance.x >> 6);
        f->cache[c].width     = (int)bmp->width;
        f->cache[c].height    = (int)bmp->rows;

        /* Characters like space have no visible pixels */
        if (bmp->width == 0 || bmp->rows == 0) continue;

        /*
         * Convert FreeType's grayscale bitmap to RGBA.
         *
         * FreeType gives: 1 byte per pixel, 0=transparent 255=opaque.
         * SDL2 needs:     4 bytes per pixel (R, G, B, A).
         *
         * We set RGB=white (255,255,255) and A=FreeType's value.
         * Later, SDL_SetTextureColorMod() multiplies the RGB by
         * whatever color we want — white*color = color. This lets
         * us draw the same cached texture in any color efficiently.
         */
        Uint8 *rgba = malloc((size_t)(bmp->width * bmp->rows * 4));
        if (!rgba) {
            fprintf(stderr, "font_init: out of memory for glyph %d\n", c);
            continue;
        }

        for (int i = 0; i < (int)(bmp->width * bmp->rows); i++) {
            rgba[i * 4 + 0] = 255;              /* R */
            rgba[i * 4 + 1] = 255;              /* G */
            rgba[i * 4 + 2] = 255;              /* B */
            rgba[i * 4 + 3] = bmp->buffer[i];   /* A */
        }

        /*
         * SDL_PIXELFORMAT_ABGR8888 — correct byte order on Linux.
         * SDL_TEXTUREACCESS_STATIC — uploaded once, read many times.
         */
        SDL_Texture *tex = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STATIC,
            (int)bmp->width,
            (int)bmp->rows
        );

        if (!tex) {
            free(rgba);
            continue;
        }

        /* Upload pixel data. pitch = bytes per row */
        SDL_UpdateTexture(tex, NULL, rgba,
                          (int)bmp->width * 4);

        /* Enable alpha blending so glyph edges are anti-aliased */
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

        free(rgba);
        f->cache[c].texture = tex;
    }

    /* ── Step 6: Compute cell dimensions ── */
    /*
     * cell_width  = advance of 'M' (widest typical character)
     * cell_height = total line height from FreeType metrics
     *               = ascender + descender (both positive here)
     *
     * Adding a small leading (extra spacing between lines)
     * improves readability. We add 2px.
     */
    f->cell_width  = f->cache['M'].advance;
    f->cell_height = ascender + descender + 2;

    /* Safety fallbacks */
    if (f->cell_width  <= 0) f->cell_width  = size / 2;
    if (f->cell_height <= 0) f->cell_height = size + 4;
    if (f->ascender    <= 0) f->ascender    = size;

    printf("Font loaded: %s @ %dpx  cell=%dx%d  ascender=%d\n",
           path, size, f->cell_width, f->cell_height, f->ascender);
    return 0;
}


/* ── font_draw_char ─────────────────────────────────────────── */

/*
 * Draws one character at cell position (x, y).
 * x, y = top-left corner of the CHARACTER CELL (not the glyph).
 *
 * Vertical placement formula:
 *
 *   glyph_y = y + (ascender - bearing_y)
 *
 * Explanation:
 *   - y is the top of the cell
 *   - ascender is how far the tallest character rises above baseline
 *   - so (y + ascender) is the pixel position of the baseline
 *   - bearing_y is how far THIS glyph rises above the baseline
 *   - subtracting bearing_y from the baseline gives the glyph top
 *
 * This formula works correctly for ALL characters:
 *   'H' — large bearing_y, sits near cell top
 *   'p' — bearing_y only covers part of height, descender hangs below
 *   '.' — small bearing_y, sits near the baseline
 */
void font_draw_char(Font *f, SDL_Renderer *renderer,
                    char c, int x, int y,
                    Uint8 r, Uint8 g, Uint8 b) {

    unsigned char uc = (unsigned char)c;
    if (uc < 32 || uc >= GLYPH_COUNT) return;

    Glyph *glyph = &f->cache[uc];
    if (!glyph->texture) return;

    /* Apply color: SDL multiplies texture RGB by (r,g,b) */
    SDL_SetTextureColorMod(glyph->texture, r, g, b);

    /*
     * Compute destination rectangle.
     *
     * dst.x = x + bearing_x
     *   bearing_x shifts right for glyphs with left padding
     *   (most monospace fonts have bearing_x = 0 or small positive)
     *
     * dst.y = y + (ascender - bearing_y)
     *   Places glyph on the correct baseline position
     */
    SDL_Rect dst = {
        x + glyph->bearing_x,
        y + (f->ascender - glyph->bearing_y),
        glyph->width,
        glyph->height
    };

    SDL_RenderCopy(renderer, glyph->texture, NULL, &dst);
}


/* ── font_draw_string ───────────────────────────────────────── */

/*
 * Draws a null-terminated string starting at (x, y).
 * Advances x by each character's advance width.
 */
void font_draw_string(Font *f, SDL_Renderer *renderer,
                      const char *str, int x, int y,
                      Uint8 r, Uint8 g, Uint8 b) {

    int cursor_x = x;

    while (*str != '\0') {
        unsigned char uc = (unsigned char)*str;

        font_draw_char(f, renderer, *str,
                       cursor_x, y, r, g, b);

        /* Advance by this glyph's width */
        if (uc >= 32 && uc < GLYPH_COUNT)
            cursor_x += f->cache[uc].advance;

        str++;
    }
}


/* ── font_destroy ───────────────────────────────────────────── */

void font_destroy(Font *f) {
    for (int c = 32; c < GLYPH_COUNT; c++) {
        if (f->cache[c].texture) {
            SDL_DestroyTexture(f->cache[c].texture);
            f->cache[c].texture = NULL;
        }
    }
    if (f->face)    FT_Done_Face(f->face);
    if (f->library) FT_Done_FreeType(f->library);
    printf("Font destroyed.\n");
}