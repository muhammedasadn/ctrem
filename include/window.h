#ifndef WINDOW_H
#define WINDOW_H

/*
 * window.h — SDL2 window management.
 *
 * Supports:
 *   - Resizable window (user can drag edges)
 *   - F11 fullscreen toggle
 *   - Maximize / minimize via window manager
 *   - GPU-accelerated renderer with vsync
 */

#include <SDL2/SDL.h>

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    int           width;
    int           height;
    int           running;
    int           fullscreen;   /* 1 = currently fullscreen */
} Window;

int  window_init(Window *w, int width, int height);
void window_toggle_fullscreen(Window *w);
void window_render_begin(Window *w);
void window_render_end(Window *w);
void window_destroy(Window *w);

#endif /* WINDOW_H */