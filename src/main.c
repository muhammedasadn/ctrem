#include <stdio.h>
#include <string.h>
#include "window.h"
#include "font.h"
#include "pty.h"

/* How many characters fit on screen */
#define COLS 80
#define ROWS 24

/* Simple screen buffer — a 2D grid of characters */
static char screen[ROWS][COLS + 1];
static int  cursor_row = 0;
static int  cursor_col = 0;

/*
 * handle_output — takes raw bytes from bash and puts them
 * into our screen buffer. Very simplified for now —
 * the full ANSI parser comes in Module 5.
 */
void handle_output(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        char c = buf[i];

        if (c == '\n') {
            /* Newline: move to next row */
            cursor_row++;
            cursor_col = 0;
            if (cursor_row >= ROWS) {
                /* Scroll: shift all rows up by one */
                memmove(screen[0], screen[1], (ROWS - 1) * (COLS + 1));
                memset(screen[ROWS - 1], 0, COLS + 1);
                cursor_row = ROWS - 1;
            }
        } else if (c == '\r') {
            /* Carriage return: go to start of current line */
            cursor_col = 0;
        } else if (c == '\b') {
            /* Backspace: move cursor left */
            if (cursor_col > 0) cursor_col--;
        } else if (c >= 32 && c < 127) {
            /* Printable character: put it in the buffer */
            if (cursor_col < COLS) {
                screen[cursor_row][cursor_col] = c;
                cursor_col++;
            }
        }
        /* For now, skip ANSI escape sequences (Module 5) */
    }
}

int main(void) {
    Window win;
    Font   font;
    PTY    pty;

    /* Initialize window */
    if (window_init(&win, 800, 500) != 0) return 1;

    /* Initialize font */
    if (font_init(&font, win.renderer, "../assets/font.ttf", 16) != 0) {
        window_destroy(&win);
        return 1;
    }

    /* Initialize PTY — calculate cols/rows from font cell size */
    int cols = win.width  / font.cell_width;
    int rows = win.height / font.cell_height;

    if (pty_init(&pty, cols, rows) != 0) {
        font_destroy(&font);
        window_destroy(&win);
        return 1;
    }

    /* Clear screen buffer */
    memset(screen, 0, sizeof(screen));

    char read_buf[4096];

    while (win.running) {
        /* 1. Handle SDL2 window events */
        window_handle_events(&win);

        /* 2. Handle keyboard input — send keypresses to bash */
        /* We hook into SDL's event system directly here */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                win.running = 0;
            }
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    win.running = 0;
                }
            }
            if (event.type == SDL_TEXTINPUT) {
                /*
                 * SDL_TEXTINPUT fires for regular printable characters.
                 * event.text.text is a small string with the typed char.
                 * We write it directly to the PTY master → bash receives it.
                 */
                pty_write(&pty, event.text.text,
                          strlen(event.text.text));
            }
            if (event.type == SDL_KEYDOWN) {
                /*
                 * Special keys need manual translation.
                 * bash expects specific byte sequences for Enter,
                 * Backspace, arrow keys, etc.
                 */
                switch (event.key.keysym.sym) {
                    case SDLK_RETURN:
                        pty_write(&pty, "\r", 1);  /* Enter = carriage return */
                        break;
                    case SDLK_BACKSPACE:
                        pty_write(&pty, "\x7f", 1); /* Backspace = DEL byte */
                        break;
                    case SDLK_UP:
                        pty_write(&pty, "\x1b[A", 3); /* ESC [ A */
                        break;
                    case SDLK_DOWN:
                        pty_write(&pty, "\x1b[B", 3); /* ESC [ B */
                        break;
                    case SDLK_RIGHT:
                        pty_write(&pty, "\x1b[C", 3); /* ESC [ C */
                        break;
                    case SDLK_LEFT:
                        pty_write(&pty, "\x1b[D", 3); /* ESC [ D */
                        break;
                }
            }
        }

        /* 3. Read whatever bash has written back to us */
        int n = pty_read(&pty, read_buf, sizeof(read_buf));
        if (n > 0) {
            handle_output(read_buf, n);
        }

        /* 4. Render the screen buffer */
        window_render_begin(&win);

        for (int row = 0; row < rows && row < ROWS; row++) {
            for (int col = 0; col < cols && col < COLS; col++) {
                char c = screen[row][col];
                if (c == 0) continue;  /* Empty cell — skip */

                int x = col * font.cell_width;
                int y = row * font.cell_height;

                font_draw_char(&font, win.renderer, c,
                               x, y, 200, 200, 200); /* gray text */
            }
        }

        /* Draw a simple block cursor */
        {
            SDL_Rect cursor_rect = {
                cursor_col * font.cell_width,
                cursor_row * font.cell_height,
                font.cell_width,
                font.cell_height
            };
            SDL_SetRenderDrawColor(win.renderer, 200, 200, 200, 180);
            SDL_RenderFillRect(win.renderer, &cursor_rect);
        }

        window_render_end(&win);
    }

    pty_destroy(&pty);
    font_destroy(&font);
    window_destroy(&win);
    return 0;
}