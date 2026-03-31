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
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* for access() */
#include <limits.h>

/* Helper function to try loading a font from a path */
static int try_load_font(FT_Library library, FT_Face *face, 
                         const char *path, int size) {
    if (!path) return -1;
    
    /* Check if file exists and is readable */
    if (access(path, R_OK) != 0) return -1;
    
    /* Try to load the font */
    if (FT_New_Face(library, path, 0, face) == 0) {
        printf("font_init: Loaded font from: %s\n", path);
        return 0;
    }
    return -1;
}

static int try_load_relative_to_exe(FT_Library library, FT_Face *face,
                                    const char *path, int size) {
    if (!path || path[0] == '\0' || path[0] == '/') {
        return -1;
    }

#ifdef CTERM_LINUX
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0 || len >= (ssize_t)sizeof(exe_path) - 1) {
        return -1;
    }
    exe_path[len] = '\0';

    char *slash = strrchr(exe_path, '/');
    if (!slash) {
        return -1;
    }
    *slash = '\0';

    char resolved[PATH_MAX];
    if (snprintf(resolved, sizeof(resolved), "%s/%s", exe_path, path)
        >= (int)sizeof(resolved)) {
        return -1;
    }

    return try_load_font(library, face, resolved, size);
#else
    (void)library;
    (void)face;
    (void)path;
    (void)size;
    return -1;
#endif
}


/* ── font_init ──────────────────────────────────────────────── */

int font_init(Font *f, SDL_Renderer *renderer,
              const char *config_path, int size) {

    memset(f, 0, sizeof(Font));
    f->size = size;

    /* ── Step 1: Init FreeType library ── */
    if (FT_Init_FreeType(&f->library) != 0) {
        fprintf(stderr, "font_init: FT_Init_FreeType failed\n");
        return -1;
    }

    /* ── Step 2: Try to load font from multiple locations ── */
    const char *env_font = getenv("CTERM_FONT");
    const char* font_paths[] = {
        /* 1. Config file path */
        config_path,
        
        /* 2. AppImage internal path */
        "/usr/share/cterm/font.ttf",
        
        /* 3. Common system font locations */
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/truetype/fira-code/FiraCode-Regular.ttf",
        "assets/font.ttf",
        "../assets/font.ttf",
        
        NULL
    };
    
    int loaded = 0;
    if (env_font && try_load_font(f->library, &f->face, env_font, size) == 0) {
        loaded = 1;
    }

    for (int i = 0; font_paths[i] != NULL; i++) {
        if (loaded) {
            break;
        }

        if (try_load_font(f->library, &f->face, font_paths[i], size) == 0) {
            loaded = 1;
            break;
        }

        if (try_load_relative_to_exe(f->library, &f->face,
                                     font_paths[i], size) == 0) {
            loaded = 1;
            break;
        }
    }
    
    if (!loaded) {
        fprintf(stderr, "font_init: Failed to load any font!\n");
        fprintf(stderr, "Tried paths:\n");
        if (env_font) {
            fprintf(stderr, "  - %s\n", env_font);
        }
        for (int i = 0; font_paths[i] != NULL; i++) {
            fprintf(stderr, "  - %s\n", font_paths[i]);
        }
        FT_Done_FreeType(f->library);
        return -1;
    }

    /* ── Step 3: Set pixel size ── */
    FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)size);

    /*
     * ── Step 4: Read font-wide metrics ──
     */
    int ascender  = (int)(f->face->size->metrics.ascender  >> 6);
    int descender = (int)(f->face->size->metrics.descender >> 6);
    if (descender < 0) descender = -descender;

    f->ascender = ascender;

    /* ── Step 5: Rasterise every printable ASCII character ── */
    for (int c = 32; c < GLYPH_COUNT; c++) {
        if (FT_Load_Char(f->face, (FT_ULong)c, FT_LOAD_RENDER) != 0) {
            continue;
        }

        FT_GlyphSlot slot = f->face->glyph;
        FT_Bitmap   *bmp  = &slot->bitmap;

        f->cache[c].bearing_x = slot->bitmap_left;
        f->cache[c].bearing_y = slot->bitmap_top;
        f->cache[c].advance   = (int)(slot->advance.x >> 6);
        f->cache[c].width     = (int)bmp->width;
        f->cache[c].height    = (int)bmp->rows;

        if (bmp->width == 0 || bmp->rows == 0) continue;

        Uint8 *rgba = malloc((size_t)(bmp->width * bmp->rows * 4));
        if (!rgba) {
            fprintf(stderr, "font_init: out of memory for glyph %d\n", c);
            continue;
        }

        for (int i = 0; i < (int)(bmp->width * bmp->rows); i++) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = bmp->buffer[i];
        }

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

        SDL_UpdateTexture(tex, NULL, rgba, (int)bmp->width * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        free(rgba);
        f->cache[c].texture = tex;
    }

    /* ── Step 6: Compute cell dimensions ── */
    f->cell_width  = f->cache['M'].advance;
    f->cell_height = ascender + descender + 2;

    if (f->cell_width  <= 0) f->cell_width  = size / 2;
    if (f->cell_height <= 0) f->cell_height = size + 4;
    if (f->ascender    <= 0) f->ascender    = size;

    printf("Font loaded successfully! cell=%dx%d ascender=%d\n",
           f->cell_width, f->cell_height, f->ascender);
    return 0;
}


/* ── font_draw_char ─────────────────────────────────────────── */

void font_draw_char(Font *f, SDL_Renderer *renderer,
                    char c, int x, int y,
                    Uint8 r, Uint8 g, Uint8 b) {

    unsigned char uc = (unsigned char)c;
    if (uc < 32 || uc >= GLYPH_COUNT) return;

    Glyph *glyph = &f->cache[uc];
    if (!glyph->texture) return;

    SDL_SetTextureColorMod(glyph->texture, r, g, b);

    SDL_Rect dst = {
        x + glyph->bearing_x,
        y + (f->ascender - glyph->bearing_y),
        glyph->width,
        glyph->height
    };

    SDL_RenderCopy(renderer, glyph->texture, NULL, &dst);
}


/* ── font_draw_string ───────────────────────────────────────── */

void font_draw_string(Font *f, SDL_Renderer *renderer,
                      const char *str, int x, int y,
                      Uint8 r, Uint8 g, Uint8 b) {

    int cursor_x = x;

    while (*str != '\0') {
        unsigned char uc = (unsigned char)*str;

        font_draw_char(f, renderer, *str,
                       cursor_x, y, r, g, b);

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
