#include "window.h"
#include <stdio.h>

/*
 * window_init — creates the SDL2 window and renderer
 *
 * Parameters:
 *   w      — pointer to our Window struct (we fill it in here)
 *   width  — window width in pixels
 *   height — window height in pixels
 *
 * Returns 0 on success, -1 on failure.
 * Returning an int for success/failure is a C convention.
 */
int window_init(Window *w, int width, int height) {

    /* SDL_Init starts the subsystems we need.
     * SDL_INIT_VIDEO = window, rendering, input events
     * Returns non-zero on failure. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Store width and height in our struct */
    w->width  = width;
    w->height = height;
    w->running = 1;  /* 1 means "keep running" */

    /* Create the OS window.
     * SDL_WINDOWPOS_CENTERED = center it on screen
     * SDL_WINDOW_SHOWN       = make it visible immediately
     * SDL_WINDOW_RESIZABLE   = user can resize it */
    w->window = SDL_CreateWindow(
        "cterm",                  /* Title bar text      */
        SDL_WINDOWPOS_CENTERED,   /* X position          */
        SDL_WINDOWPOS_CENTERED,   /* Y position          */
        width,                    /* Width in pixels     */
        height,                   /* Height in pixels    */
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (w->window == NULL) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Create the renderer — this is what draws to the window.
     * -1 = use the first available driver (usually your GPU)
     * SDL_RENDERER_ACCELERATED = use GPU acceleration      */
    w->renderer = SDL_CreateRenderer(
        w->window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (w->renderer == NULL) {
        printf("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    printf("Window created: %dx%d\n", width, height);
    return 0;  /* Success */
}

/*
 * window_handle_events — check if anything happened
 * (key press, mouse click, close button, etc.)
 * Call this once per frame inside your loop.
 */
void window_handle_events(Window *w) {
    SDL_Event event;

    /* SDL_PollEvent fills 'event' with the next event.
     * Returns 1 if there was an event, 0 if the queue is empty.
     * The while loop drains all pending events each frame. */
    while (SDL_PollEvent(&event)) {

        /* SDL_QUIT fires when the user clicks the X button */
        if (event.type == SDL_QUIT) {
            w->running = 0;  /* Tell the loop to stop */
        }

        /* Escape key also quits — good for development */
        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                w->running = 0;
            }
        }
    }
}

/*
 * window_render_begin — called at the START of each frame.
 * Clears the screen to a dark background color.
 * RGB(18, 18, 18) is a near-black, easy on the eyes.
 */
void window_render_begin(Window *w) {
    SDL_SetRenderDrawColor(w->renderer, 18, 18, 18, 255);
    SDL_RenderClear(w->renderer);
}

/*
 * window_render_end — called at the END of each frame.
 * "Presents" the frame — swaps the back buffer to screen.
 * This is double buffering: you draw on a hidden surface,
 * then flip it to the screen all at once (no flickering).
 */
void window_render_end(Window *w) {
    SDL_RenderPresent(w->renderer);
}

/*
 * window_destroy — clean up everything.
 * Always free resources in reverse order of creation.
 */
void window_destroy(Window *w) {
    if (w->renderer) SDL_DestroyRenderer(w->renderer);
    if (w->window)   SDL_DestroyWindow(w->window);
    SDL_Quit();
    printf("Window destroyed.\n");
}