#include <stdio.h>
#include "window.h"
#include "font.h"

int main(void) {
    Window win;
    Font   font;

    if (window_init(&win, 800, 500) != 0) {
        return 1;
    }

    /* Load font from assets folder at 18px size */
    if (font_init(&font, win.renderer, "../assets/font.ttf", 18) != 0) {
        printf("Failed to load font.\n");
        window_destroy(&win);
        return 1;
    }

    while (win.running) {
        window_handle_events(&win);
        window_render_begin(&win);

        /* Draw some test strings at different positions and colors */
        font_draw_string(&font, win.renderer,
                         "cterm v0.1 - terminal emulator",
                         10, 10, 220, 220, 220);  /* light gray */

        font_draw_string(&font, win.renderer,
                         "user@machine:~$",
                         10, 40, 80, 200, 120);   /* green prompt */

        font_draw_string(&font, win.renderer,
                         "Hello, World!",
                         10, 70, 255, 255, 255);  /* white */

        font_draw_string(&font, win.renderer,
                         "ANSI colors coming in Module 5...",
                         10, 100, 100, 180, 255); /* blue */

        window_render_end(&win);
    }

    font_destroy(&font);
    window_destroy(&win);
    return 0;
}