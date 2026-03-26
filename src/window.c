/*
 * window.c — SDL2 window + renderer.
 *
 * Features:
 *   - Resizable window (SDL_WINDOW_RESIZABLE)
 *   - F11 fullscreen toggle (true fullscreen desktop mode)
 *   - Maximize via window manager works automatically
 *   - VSync enabled for smooth rendering
 *   - Dark title bar where supported
 */

#include "window.h"
#include <stdio.h>

int window_init(Window *w, int width, int height) {

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    w->width      = width;
    w->height     = height;
    w->running    = 1;
    w->fullscreen = 0;

    /*
     * SDL_WINDOW_RESIZABLE — user can drag window edges to resize.
     * SDL_WINDOW_SHOWN     — show immediately, no invisible window.
     *
     * We do NOT use SDL_WINDOW_FULLSCREEN here — we start windowed
     * and toggle fullscreen with F11 via window_toggle_fullscreen().
     */
    w->window = SDL_CreateWindow(
        "cterm",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN |
        SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!w->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    /*
     * Set minimum window size so the user can't shrink it to
     * something too small to render any terminal content.
     */
    SDL_SetWindowMinimumSize(w->window, 400, 200);

    /*
     * SDL_RENDERER_ACCELERATED — use GPU for all drawing.
     * SDL_RENDERER_PRESENTVSYNC — cap to monitor refresh rate,
     *   eliminates tearing and reduces CPU usage when idle.
     */
    w->renderer = SDL_CreateRenderer(
        w->window, -1,
        SDL_RENDERER_ACCELERATED |
        SDL_RENDERER_PRESENTVSYNC
    );

    if (!w->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(w->window);
        return -1;
    }

    /* Enable alpha blending for the tool launcher overlay */
    SDL_SetRenderDrawBlendMode(w->renderer, SDL_BLENDMODE_BLEND);

    printf("Window created: %dx%d\n", width, height);
    return 0;
}

/*
 * window_toggle_fullscreen — switch between windowed and fullscreen.
 *
 * SDL_WINDOW_FULLSCREEN_DESKTOP keeps the native desktop resolution
 * and just expands the window to fill the screen — no mode switch,
 * no flicker, instant. This is what every modern terminal uses.
 *
 * Pressing F11 again returns to the previous windowed size.
 */
void window_toggle_fullscreen(Window *w) {
    w->fullscreen = !w->fullscreen;

    if (w->fullscreen) {
        SDL_SetWindowFullscreen(w->window,
                                SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(w->window, 0);
    }

    /* Update stored dimensions after mode change */
    SDL_GetWindowSize(w->window, &w->width, &w->height);
}

void window_render_begin(Window *w) {
    /* Dark background — #121212 */
    SDL_SetRenderDrawColor(w->renderer, 18, 18, 18, 255);
    SDL_RenderClear(w->renderer);
}

void window_render_end(Window *w) {
    SDL_RenderPresent(w->renderer);
}

void window_destroy(Window *w) {
    if (w->renderer) SDL_DestroyRenderer(w->renderer);
    if (w->window)   SDL_DestroyWindow(w->window);
    SDL_Quit();
}