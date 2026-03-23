#include "font.h"
#include <stdio.h>
#include <string.h>

/*
 * font_init — loads a .ttf file and pre-renders all printable
 * ASCII characters into GPU textures.
 *
 * Parameters:
 *   f        — pointer to our Font struct
 *   renderer — the SDL2 renderer (needed to create textures)
 *   path     — file path to the .ttf font file
 *   size     — font size in pixels (e.g. 16)
 */
int font_init(Font *f, SDL_Renderer *renderer,
              const char *path, int size) {

    /* Zero out the struct so all pointers start as NULL.
     * memset fills a block of memory with a value.
     * sizeof(Font) = total bytes the Font struct occupies. */
    memset(f, 0, sizeof(Font));
    f->size = size;

    /* Step 1: Initialize the FreeType library */
    if (FT_Init_FreeType(&f->library) != 0) {
        printf("FreeType init failed\n");
        return -1;
    }

    /* Step 2: Load the font file into a "face"
     * A face = one font + one style (e.g. Regular, Bold)
     * The 0 at the end = use the first face in the file */
    if (FT_New_Face(f->library, path, 0, &f->face) != 0) {
        printf("Failed to load font: %s\n", path);
        return -1;
    }

    /* Step 3: Set the font size.
     * FT_Set_Pixel_Sizes(face, width, height)
     * 0 for width = auto-calculate from height */
    FT_Set_Pixel_Sizes(f->face, 0, size);

    /* Step 4: Pre-render all printable ASCII characters.
     * ASCII 32 = space, 127 = DEL (we stop before that).
     * We loop through every printable character and cache it. */
    for (int c = 32; c < GLYPH_COUNT; c++) {

        /* FT_LOAD_RENDER = load the glyph AND rasterize it.
         * After this call, f->face->glyph->bitmap holds pixels. */
        if (FT_Load_Char(f->face, c, FT_LOAD_RENDER) != 0) {
            printf("Failed to load glyph '%c'\n", c);
            continue;  /* Skip this character, don't crash */
        }

        FT_GlyphSlot slot = f->face->glyph;
        FT_Bitmap   *bmp  = &slot->bitmap;

        /* Store the positioning metrics */
        f->cache[c].bearing_x = slot->bitmap_left;
        f->cache[c].bearing_y = slot->bitmap_top;
        f->cache[c].advance   = slot->advance.x >> 6; /* advance is in 1/64 pixels, shift right 6 = divide by 64 */
        f->cache[c].width     = bmp->width;
        f->cache[c].height    = bmp->rows;

        /* Some characters (like space) have no visible pixels.
         * Skip creating a texture for them. */
        if (bmp->width == 0 || bmp->rows == 0) {
            continue;
        }

        /*
         * FreeType gives us a grayscale bitmap: one byte per pixel,
         * 0 = transparent, 255 = fully opaque.
         * SDL2 needs RGBA: 4 bytes per pixel (red, green, blue, alpha).
         * We convert: set RGB to white (255,255,255), alpha = FreeType byte.
         * This lets us color the glyph later by modulating the texture.
         */
        Uint8 *rgba = malloc(bmp->width * bmp->rows * 4);
        if (!rgba) {
            printf("Out of memory\n");
            return -1;
        }

        for (int i = 0; i < (int)(bmp->width * bmp->rows); i++) {
            rgba[i * 4 + 0] = 255;        /* Red   */
            rgba[i * 4 + 1] = 255;        /* Green */
            rgba[i * 4 + 2] = 255;        /* Blue  */
            rgba[i * 4 + 3] = bmp->buffer[i]; /* Alpha from FreeType */
        }

        /* Create an SDL2 texture from our RGBA pixel data.
         * SDL_PIXELFORMAT_RGBA8888 = 4 bytes per pixel, RGBA order.
         * SDL_TEXTUREACCESS_STATIC = uploaded once, read many times. */
        SDL_Texture *tex = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_STATIC,
            bmp->width,
            bmp->rows
        );

        /* Upload our pixel data into the texture.
         * pitch = bytes per row = width * 4 bytes per pixel */
        SDL_UpdateTexture(tex, NULL, rgba, bmp->width * 4);

        /* Enable alpha blending so transparent pixels show the background */
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

        free(rgba); /* We don't need the CPU copy anymore */

        f->cache[c].texture = tex;
    }

    /*
     * Determine cell size — how much space one character takes.
     * We measure the 'M' character because it's typically the
     * widest character in monospace fonts (hence "em" unit in CSS).
     */
    f->cell_width  = f->cache['M'].advance;
    f->cell_height = size + (size / 4); /* add 25% for line spacing */

    printf("Font loaded: %s @ %dpx, cell=%dx%d\n",
           path, size, f->cell_width, f->cell_height);
    return 0;
}

/*
 * font_draw_char — draws a single character at position (x, y).
 * x, y is the TOP-LEFT of the character cell.
 * r, g, b controls the text color.
 */
void font_draw_char(Font *f, SDL_Renderer *renderer,
                    char c, int x, int y,
                    Uint8 r, Uint8 g, Uint8 b) {

    /* Cast to unsigned to use as array index safely */
    unsigned char uc = (unsigned char)c;

    /* Bounds check — only handle printable ASCII */
    if (uc < 32 || uc >= GLYPH_COUNT) return;

    Glyph *g_entry = &f->cache[uc];

    /* Space and other zero-bitmap chars have no texture */
    if (g_entry->texture == NULL) return;

    /* Colorize the texture.
     * SDL_SetTextureColorMod multiplies the texture's RGB by r,g,b.
     * Since our texture is white (255,255,255), multiplying by any
     * color gives us that color. */
    SDL_SetTextureColorMod(g_entry->texture, r, g, b);

    /*
     * Position the glyph correctly using bearing metrics.
     * bearing_x shifts right (for characters with left padding).
     * bearing_y shifts up from baseline (font ascender).
     * We use cell_height as baseline reference so all chars sit
     * on the same invisible line.
     */
    SDL_Rect dst = {
        x + g_entry->bearing_x,
        y + (f->cell_height - g_entry->bearing_y) - (f->cell_height / 4),
        g_entry->width,
        g_entry->height
    };

    SDL_RenderCopy(renderer, g_entry->texture, NULL, &dst);
}

/*
 * font_draw_string — draws a full string starting at (x, y).
 * Loops through each character, draws it, then advances x.
 */
void font_draw_string(Font *f, SDL_Renderer *renderer,
                      const char *str, int x, int y,
                      Uint8 r, Uint8 g, Uint8 b) {

    int cursor_x = x;

    /* Loop until we hit the null terminator '\0'
     * In C, strings end with a zero byte. The loop condition
     * *str != '\0' is true while there are characters left. */
    while (*str != '\0') {
        font_draw_char(f, renderer, *str, cursor_x, y, r, g, b);

        /* Advance cursor by this character's width */
        unsigned char uc = (unsigned char)*str;
        if (uc >= 32 && uc < GLYPH_COUNT) {
            cursor_x += f->cache[uc].advance;
        }

        str++; /* Move to next character in the string */
    }
}

/*
 * font_destroy — free all GPU textures and FreeType resources.
 */
void font_destroy(Font *f) {
    for (int c = 32; c < GLYPH_COUNT; c++) {
        if (f->cache[c].texture) {
            SDL_DestroyTexture(f->cache[c].texture);
        }
    }
    FT_Done_Face(f->face);
    FT_Done_FreeType(f->library);
    printf("Font destroyed.\n");
}