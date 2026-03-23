/*
 * main.c — cterm entry point.
 *
 * Wires together all modules:
 *   window  → SDL2 window + GPU renderer
 *   font    → FreeType glyph cache
 *   pty     → shell process via pseudo-terminal
 *   term    → ANSI/VT100 parser + cell grid + scrollback
 *
 * Main loop every frame:
 *   1. Poll SDL2 events  (keyboard, mouse, resize, quit)
 *   2. Read PTY output   (bash → parser → cell grid)
 *   3. Render cell grid  (background rects + glyphs)
 *   4. Draw cursor       (blinks, hidden when scrolled)
 *   5. Draw scroll bar   (thin blue line when in history)
 */

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "window.h"
#include "font.h"
#include "pty.h"
#include "ansi.h"


int main(void) {
    Window   win;
    Font     font;
    PTY      pty;
    Terminal *term;

    /* ── Initialize window ───────────────────────────────────── */
    if (window_init(&win, 800, 500) != 0) {
        fprintf(stderr, "Failed to create window.\n");
        return 1;
    }

    /* ── Initialize font ─────────────────────────────────────── */
    if (font_init(&font, win.renderer, "../assets/font.ttf", 16) != 0) {
        fprintf(stderr, "Failed to load font.\n");
        window_destroy(&win);
        return 1;
    }

    /* ── Calculate grid dimensions from font cell size ───────── */
    /*
     * TAB_BAR_HEIGHT reserves space at the top for the tab bar
     * we will add in Module 7. For now it is 0.
     * Subtract it from usable height when calculating rows.
     */
    int TAB_BAR_HEIGHT = 0;
    int cols = win.width  / font.cell_width;
    int rows = (win.height - TAB_BAR_HEIGHT) / font.cell_height;

    /* ── Initialize terminal state ───────────────────────────── */
    term = terminal_create(cols, rows);
    if (!term) {
        fprintf(stderr, "Failed to create terminal.\n");
        font_destroy(&font);
        window_destroy(&win);
        return 1;
    }

    /* ── Initialize PTY — spawns bash ────────────────────────── */
    if (pty_init(&pty, cols, rows) != 0) {
        fprintf(stderr, "Failed to create PTY.\n");
        terminal_destroy(term);
        font_destroy(&font);
        window_destroy(&win);
        return 1;
    }

    char read_buf[4096];
    int  running = 1;

    /* ════════════════════════════════════════════════════════════
     * MAIN LOOP
     * Runs every frame until the user closes the window.
     * ════════════════════════════════════════════════════════════ */
    while (running) {

        /* ── 1. EVENT HANDLING ───────────────────────────────── */
        /*
         * ONE event loop handles everything.
         * Never split events across window_handle_events() AND
         * a separate SDL_PollEvent() loop — SDL's queue is drained
         * on the first call, so the second loop would see nothing.
         */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {

            /* ── Quit ── */
            if (event.type == SDL_QUIT) {
                running = 0;
            }

            /* ── Window resize ── */
            if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    win.width  = event.window.data1;
                    win.height = event.window.data2;
                    int new_cols = win.width  / font.cell_width;
                    int new_rows = (win.height - TAB_BAR_HEIGHT)
                                   / font.cell_height;
                    terminal_resize(term, new_cols, new_rows);
                    pty_resize(&pty, new_cols, new_rows);
                }
            }

            /* ── Mouse wheel — scrollback navigation ── */
            if (event.type == SDL_MOUSEWHEEL) {
                if (event.wheel.y > 0) {
                    /* Wheel UP → scroll into history */
                    term->scroll_offset += 3;
                    if (term->scroll_offset > term->sb_count)
                        term->scroll_offset = term->sb_count;
                } else if (event.wheel.y < 0) {
                    /* Wheel DOWN → back toward live view */
                    term->scroll_offset -= 3;
                    if (term->scroll_offset < 0)
                        term->scroll_offset = 0;
                }
            }

            /* ── Text input (printable characters) ── */
            /*
             * SDL fires SDL_TEXTINPUT for regular typed characters.
             * event.text.text is a small UTF-8 string (usually 1 char).
             * Typing anything snaps back to live view.
             */
            if (event.type == SDL_TEXTINPUT) {
                if (term->scroll_offset > 0)
                    term->scroll_offset = 0;
                pty_write(&pty, event.text.text,
                          strlen(event.text.text));
            }

            /* ── Special keys ── */
            if (event.type == SDL_KEYDOWN) {
                SDL_Keycode sym = event.key.keysym.sym;
                SDL_Keymod  mod = SDL_GetModState();

                /*
                 * Ctrl+letter combinations.
                 * In terminals, Ctrl+A sends byte 0x01, Ctrl+B sends
                 * 0x02, etc. This is calculated by subtracting 'a'
                 * from the key symbol and adding 1.
                 * Ctrl+C (0x03) is the most important — it sends
                 * SIGINT to the foreground process.
                 */
                if (mod & KMOD_CTRL) {
                    if (sym >= SDLK_a && sym <= SDLK_z) {
                        char ctrl_char = (char)(sym - SDLK_a + 1);
                        pty_write(&pty, &ctrl_char, 1);
                    }
                }

                switch (sym) {

                    /* Enter — carriage return, snap to live view */
                    case SDLK_RETURN:
                        term->scroll_offset = 0;
                        pty_write(&pty, "\r", 1);
                        break;

                    /* Backspace — DEL byte (0x7f) */
                    case SDLK_BACKSPACE:
                        pty_write(&pty, "\x7f", 1);
                        break;

                    case SDLK_TAB:
                        pty_write(&pty, "\t", 1);
                        break;

                    case SDLK_ESCAPE:
                        pty_write(&pty, "\x1b", 1);
                        break;

                    /*
                     * Arrow keys — VT100 cursor sequences.
                     * \x1b[A = up, B = down, C = right, D = left.
                     * bash's readline uses these for command history
                     * and cursor movement on the command line.
                     */
                    case SDLK_UP:
                        pty_write(&pty, "\x1b[A", 3);
                        break;
                    case SDLK_DOWN:
                        pty_write(&pty, "\x1b[B", 3);
                        break;
                    case SDLK_RIGHT:
                        pty_write(&pty, "\x1b[C", 3);
                        break;
                    case SDLK_LEFT:
                        pty_write(&pty, "\x1b[D", 3);
                        break;

                    case SDLK_HOME:
                        pty_write(&pty, "\x1b[H", 3);
                        break;
                    case SDLK_END:
                        pty_write(&pty, "\x1b[F", 3);
                        break;

                    /* Delete key — VT sequence \x1b[3~ */
                    case SDLK_DELETE:
                        pty_write(&pty, "\x1b[3~", 4);
                        break;

                    /*
                     * Page Up / Page Down — fast scrollback navigation.
                     * Jumps half a screen at a time, same as most
                     * terminal emulators.
                     */
                    case SDLK_PAGEUP:
                        term->scroll_offset += term->rows / 2;
                        if (term->scroll_offset > term->sb_count)
                            term->scroll_offset = term->sb_count;
                        break;

                    case SDLK_PAGEDOWN:
                        term->scroll_offset -= term->rows / 2;
                        if (term->scroll_offset < 0)
                            term->scroll_offset = 0;
                        break;

                    default:
                        break;
                }
            }

        } /* end SDL_PollEvent loop */


        /* ── 2. READ PTY OUTPUT ──────────────────────────────── */
        /*
         * pty_read() is non-blocking (we set O_NONBLOCK in pty_init).
         * It returns > 0 if bash wrote something, 0 or -1 if nothing
         * is available right now. We feed whatever we get to the
         * ANSI parser which updates the cell grid in place.
         */
        int n = pty_read(&pty, read_buf, sizeof(read_buf));
        if (n > 0) {
            terminal_process(term, read_buf, n);
        }


        /* ── 3. RENDER ───────────────────────────────────────── */
        window_render_begin(&win);

        for (int row = 0; row < term->rows; row++) {

            /*
             * terminal_get_display_row() returns the correct Cell
             * array for this row — either live screen data or a
             * historical line from the scrollback buffer, depending
             * on term->scroll_offset.
             */
            Cell *display_row = terminal_get_display_row(term, row);

            for (int col = 0; col < term->cols; col++) {
                int x = col * font.cell_width;
                int y = row * font.cell_height + TAB_BAR_HEIGHT;

                /* Default colors for empty / out-of-range rows */
                Uint8 bg_r = 0,   bg_g = 0,   bg_b = 0;
                Uint8 fg_r = 200, fg_g = 200, fg_b = 200;
                char  ch   = ' ';

                if (display_row) {
                    ch   = display_row[col].ch;
                    fg_r = display_row[col].fg.r;
                    fg_g = display_row[col].fg.g;
                    fg_b = display_row[col].fg.b;
                    bg_r = display_row[col].bg.r;
                    bg_g = display_row[col].bg.g;
                    bg_b = display_row[col].bg.b;
                }

                /* Draw background rectangle for this cell */
                SDL_SetRenderDrawColor(win.renderer,
                                       bg_r, bg_g, bg_b, 255);
                SDL_Rect bg_rect = {x, y,
                                    font.cell_width, font.cell_height};
                SDL_RenderFillRect(win.renderer, &bg_rect);

                /* Draw the character glyph (skip spaces) */
                if (ch != ' ' && ch != '\0') {
                    font_draw_char(&font, win.renderer,
                                   ch, x, y,
                                   fg_r, fg_g, fg_b);
                }
            }
        }


        /* ── 4. CURSOR ───────────────────────────────────────── */
        /*
         * Only draw the cursor when in live view (not scrolled back).
         * The cursor blinks every 500ms using SDL_GetTicks().
         * SDL_GetTicks() returns milliseconds since SDL was started.
         * Dividing by 500 and checking even/odd gives a 1Hz blink.
         */
        if (term->scroll_offset == 0) {
            Uint32 ticks = SDL_GetTicks();
            if ((ticks / 500) % 2 == 0) {
                SDL_SetRenderDrawColor(win.renderer, 220, 220, 220, 200);
                SDL_Rect cur = {
                    term->cursor_col * font.cell_width,
                    term->cursor_row * font.cell_height + TAB_BAR_HEIGHT,
                    font.cell_width,
                    font.cell_height
                };
                SDL_RenderFillRect(win.renderer, &cur);
            }
        }


        /* ── 5. SCROLL INDICATOR ─────────────────────────────── */
        /*
         * When scrolled into history, draw a thin blue line at the
         * top of the window so the user knows they are not in live
         * view. This matches the behaviour of many terminal emulators.
         *
         * Also draw a small scroll position indicator on the right
         * edge — a proportional bar showing where in history we are.
         */
        if (term->scroll_offset > 0) {

            /* Top blue line */
            SDL_SetRenderDrawColor(win.renderer, 80, 140, 255, 220);
            SDL_Rect top_line = {0, TAB_BAR_HEIGHT, win.width, 2};
            SDL_RenderFillRect(win.renderer, &top_line);

            /* Right-edge scroll position bar */
            if (term->sb_count > 0) {
                float ratio  = (float)term->scroll_offset
                               / (float)term->sb_count;
                int bar_h    = win.height / 8;   /* bar height */
                int bar_y    = (int)((win.height - bar_h) * (1.0f - ratio));
                SDL_SetRenderDrawColor(win.renderer, 80, 140, 255, 160);
                SDL_Rect bar = {win.width - 4, bar_y, 4, bar_h};
                SDL_RenderFillRect(win.renderer, &bar);
            }
        }

        window_render_end(&win);

    } /* end main loop */


    /* ── Cleanup — always free in reverse order of creation ──── */
    pty_destroy(&pty);
    terminal_destroy(term);
    font_destroy(&font);
    window_destroy(&win);

    return 0;
}