/*
 * main.c — cterm entry point.
 *
 * Features added:
 *   F11          — toggle fullscreen
 *   Alt+Enter    — toggle fullscreen (alternative)
 *   Window resize — drag edges or maximize via window manager
 *   All panes resize automatically when window changes size
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "window.h"
#include "font.h"
#include "tabs.h"
#include "pane.h"
#include "ansi.h"
#include "tools.h"


/* ── render_pane_tree ───────────────────────────────────────── */

static void render_pane_tree(Pane *p, SDL_Renderer *renderer,
                              Font *font) {
    if (!p) return;

    if (p->type != PANE_LEAF) {
        render_pane_tree(p->first,  renderer, font);
        render_pane_tree(p->second, renderer, font);
        return;
    }

    Terminal *term = p->term;
    SDL_Rect   r   = p->rect;

    if (r.w < font->cell_width || r.h < font->cell_height) return;

    int pcols = r.w / font->cell_width;
    int prows = r.h / font->cell_height;
    if (pcols < 1) pcols = 1;
    if (prows < 1) prows = 1;

    if (pcols != term->cols || prows != term->rows) {
        terminal_resize(term, pcols, prows);
        pty_resize(&p->pty, pcols, prows);
    }

    for (int row = 0; row < term->rows; row++) {
        Cell *display_row = terminal_get_display_row(term, row);

        for (int col = 0; col < term->cols; col++) {
            int x = r.x + col * font->cell_width;
            int y = r.y + row * font->cell_height;

            if (x + font->cell_width  > r.x + r.w) continue;
            if (y + font->cell_height > r.y + r.h) continue;

            Uint8 bg_r=0, bg_g=0, bg_b=0;
            Uint8 fg_r=200, fg_g=200, fg_b=200;
            char ch = ' ';

            if (display_row) {
                ch   = display_row[col].ch;
                fg_r = display_row[col].fg.r;
                fg_g = display_row[col].fg.g;
                fg_b = display_row[col].fg.b;
                bg_r = display_row[col].bg.r;
                bg_g = display_row[col].bg.g;
                bg_b = display_row[col].bg.b;
            }

            SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, 255);
            SDL_Rect bg_rect = {x, y, font->cell_width, font->cell_height};
            SDL_RenderFillRect(renderer, &bg_rect);

            if (ch != ' ' && ch != '\0')
                font_draw_char(font, renderer, ch, x, y, fg_r, fg_g, fg_b);
        }
    }

    /* Blinking cursor — focused pane, live view */
    if (p->focused && term->scroll_offset == 0) {
        Uint32 ticks = SDL_GetTicks();
        if ((ticks / 500) % 2 == 0) {
            SDL_SetRenderDrawColor(renderer, 220, 220, 220, 200);
            SDL_Rect cur = {
                r.x + term->cursor_col * font->cell_width,
                r.y + term->cursor_row * font->cell_height,
                font->cell_width, font->cell_height
            };
            SDL_RenderFillRect(renderer, &cur);
        }
    }

    /* Scroll indicator — focused pane, scrolled view */
    if (p->focused && term->scroll_offset > 0) {
        SDL_SetRenderDrawColor(renderer, 80, 140, 255, 220);
        SDL_Rect top_line = {r.x, r.y, r.w, 2};
        SDL_RenderFillRect(renderer, &top_line);

        if (term->sb_count > 0) {
            float ratio = (float)term->scroll_offset / (float)term->sb_count;
            int bar_h   = r.h / 8;
            int bar_y   = r.y + (int)((r.h - bar_h) * (1.0f - ratio));
            SDL_SetRenderDrawColor(renderer, 80, 140, 255, 160);
            SDL_Rect bar = {r.x + r.w - 4, bar_y, 4, bar_h};
            SDL_RenderFillRect(renderer, &bar);
        }
    }
}


/* ════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════ */

int main(void) {
    Window     win;
    Font       font;
    TabManager tm;
    ToolManager tools;

    /* ── Window ── */
    if (window_init(&win, 900, 560) != 0) {
        fprintf(stderr, "Failed to create window.\n");
        return 1;
    }

    /* ── Font ── */
    if (font_init(&font, win.renderer, "../assets/font.ttf", 16) != 0) {
        fprintf(stderr, "Failed to load font.\n");
        window_destroy(&win);
        return 1;
    }

    /* ── Grid size ── */
    int cols = win.width  / font.cell_width;
    int rows = (win.height - TAB_BAR_HEIGHT) / font.cell_height;

    /* ── Tabs ── */
    if (tabs_init(&tm, cols, rows) != 0) {
        fprintf(stderr, "Failed to init tabs.\n");
        font_destroy(&font);
        window_destroy(&win);
        return 1;
    }

    /* ── Tools ── */
    tools_init(&tools);

    char read_buf[4096];
    int  running = 1;

    /* ════════════════════════════════════════════════════════
     * MAIN LOOP
     * ════════════════════════════════════════════════════════ */
    while (running) {

        Tab *tab = tabs_get_active(&tm);

        /* ── Events ── */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {

            /* Quit */
            if (event.type == SDL_QUIT) {
                running = 0; continue;
            }

            /* Window resize / maximize / restore */
            if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GetWindowSize(win.window, &win.width, &win.height);
                    cols = win.width  / font.cell_width;
                    rows = (win.height - TAB_BAR_HEIGHT) / font.cell_height;
                    if (cols < 1) cols = 1;
                    if (rows < 1) rows = 1;
                    /* Pane terminals resize automatically in render loop */
                }
                continue;
            }

            /* Mouse click */
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT) {
                if (tools.launcher.visible) {
                    tools_launcher_close(&tools); continue;
                }
                int mx = event.button.x, my = event.button.y;
                if (my < TAB_BAR_HEIGHT) {
                    if (tabs_handle_click(&tm, mx, my, cols, rows))
                        tab = tabs_get_active(&tm);
                } else {
                    Pane *clicked = pane_find_at(tab->root, mx, my);
                    if (clicked) pane_set_focus(tab->root, clicked);
                }
                continue;
            }

            /* Mouse wheel — scrollback */
            if (event.type == SDL_MOUSEWHEEL) {
                if (!tools.launcher.visible) {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp && fp->term) {
                        if (event.wheel.y > 0) {
                            fp->term->scroll_offset += 3;
                            if (fp->term->scroll_offset > fp->term->sb_count)
                                fp->term->scroll_offset = fp->term->sb_count;
                        } else if (event.wheel.y < 0) {
                            fp->term->scroll_offset -= 3;
                            if (fp->term->scroll_offset < 0)
                                fp->term->scroll_offset = 0;
                        }
                    }
                }
                continue;
            }

            /* Text input */
            if (event.type == SDL_TEXTINPUT) {
                if (tools.launcher.visible) {
                    tools_launcher_handle_text(&tools, event.text.text);
                } else {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp) {
                        fp->term->scroll_offset = 0;
                        pty_write(&fp->pty, event.text.text,
                                  strlen(event.text.text));
                    }
                }
                continue;
            }

            /* Key down */
            if (event.type == SDL_KEYDOWN) {
                SDL_Keycode sym = event.key.keysym.sym;
                SDL_Keymod  mod = SDL_GetModState();
                int ctrl  = (mod & KMOD_CTRL)  != 0;
                int shift = (mod & KMOD_SHIFT) != 0;
                int alt   = (mod & KMOD_ALT)   != 0;

                /* Launcher intercepts all keys when open */
                if (tools.launcher.visible) {
                    tools_launcher_handle_key(&tools, sym, mod,
                                              &tm, cols, rows);
                    tab = tabs_get_active(&tm);
                    continue;
                }

                /* ── F11 or Alt+Enter — fullscreen toggle ── */
                if (sym == SDLK_F11 ||
                    (alt && sym == SDLK_RETURN)) {
                    window_toggle_fullscreen(&win);
                    SDL_GetWindowSize(win.window,
                                      &win.width, &win.height);
                    cols = win.width / font.cell_width;
                    rows = (win.height - TAB_BAR_HEIGHT)
                           / font.cell_height;
                    continue;
                }

                /* ── Tab shortcuts ── */
                if (ctrl && !shift && sym == SDLK_t) {
                    tabs_new(&tm, cols, rows);
                    tab = tabs_get_active(&tm); continue;
                }
                if (ctrl && !shift && sym == SDLK_w) {
                    tabs_close(&tm, tm.active);
                    tab = tabs_get_active(&tm); continue;
                }
                if (ctrl && !shift && sym == SDLK_TAB) {
                    tabs_next(&tm); tab = tabs_get_active(&tm); continue;
                }
                if (ctrl && shift && sym == SDLK_TAB) {
                    tabs_prev(&tm); tab = tabs_get_active(&tm); continue;
                }
                if (ctrl && !shift && sym >= SDLK_1 && sym <= SDLK_9) {
                    tabs_set_active(&tm, sym - SDLK_1);
                    tab = tabs_get_active(&tm); continue;
                }

                /* ── Tool launcher ── */
                if (ctrl && !shift && sym == SDLK_p) {
                    tools_launcher_open(&tools); continue;
                }

                /* ── Pane shortcuts ── */
                if (ctrl && shift && sym == SDLK_RIGHT) {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp) {
                        Pane *nw = pane_split(fp, PANE_SPLIT_H, cols, rows);
                        if (nw != fp) tab->root = nw;
                    }
                    continue;
                }
                if (ctrl && shift && sym == SDLK_DOWN) {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp) {
                        Pane *nw = pane_split(fp, PANE_SPLIT_V, cols, rows);
                        if (nw != fp) tab->root = nw;
                    }
                    continue;
                }
                if (ctrl && shift && sym == SDLK_w) {
                    tab->root = pane_close_focused(tab->root);
                    if (!tab->root) {
                        tabs_close(&tm, tm.active);
                        tab = tabs_get_active(&tm);
                    }
                    continue;
                }
                if (ctrl && shift && sym == SDLK_f) {
                    pane_focus_next(tab->root); continue;
                }

                /* ── Generic Ctrl+letter → control byte ── */
                if (ctrl && sym >= SDLK_a && sym <= SDLK_z) {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp) {
                        char ctrl_char = (char)(sym - SDLK_a + 1);
                        pty_write(&fp->pty, &ctrl_char, 1);
                    }
                    continue;
                }

                /* ── Terminal keys ── */
                {
                    Pane *fp = pane_get_focused(tab->root);
                    if (!fp) continue;
                    PTY      *pty     = &fp->pty;
                    Terminal *term_fp = fp->term;

                    switch (sym) {
                        case SDLK_RETURN:
                            term_fp->scroll_offset = 0;
                            pty_write(pty, "\r", 1); break;
                        case SDLK_BACKSPACE:
                            pty_write(pty, "\x7f", 1); break;
                        case SDLK_TAB:
                            pty_write(pty, "\t", 1); break;
                        case SDLK_ESCAPE:
                            pty_write(pty, "\x1b", 1); break;
                        case SDLK_UP:
                            pty_write(pty, "\x1b[A", 3); break;
                        case SDLK_DOWN:
                            pty_write(pty, "\x1b[B", 3); break;
                        case SDLK_RIGHT:
                            pty_write(pty, "\x1b[C", 3); break;
                        case SDLK_LEFT:
                            pty_write(pty, "\x1b[D", 3); break;
                        case SDLK_HOME:
                            pty_write(pty, "\x1b[H", 3); break;
                        case SDLK_END:
                            pty_write(pty, "\x1b[F", 3); break;
                        case SDLK_DELETE:
                            pty_write(pty, "\x1b[3~", 4); break;
                        case SDLK_PAGEUP:
                            term_fp->scroll_offset += rows / 2;
                            if (term_fp->scroll_offset > term_fp->sb_count)
                                term_fp->scroll_offset = term_fp->sb_count;
                            break;
                        case SDLK_PAGEDOWN:
                            term_fp->scroll_offset -= rows / 2;
                            if (term_fp->scroll_offset < 0)
                                term_fp->scroll_offset = 0;
                            break;
                        default: break;
                    }
                }
                continue;
            }

        } /* end SDL_PollEvent */


        /* ── Read PTY output ── */
        for (int i = 0; i < tm.count; i++)
            pane_read_all(tm.tabs[i].root, read_buf, sizeof(read_buf));


        /* ── Render ── */
        window_render_begin(&win);

        /* Tab bar */
        tabs_draw_bar(&tm, win.renderer, &font, win.width);

        /* Pane layout + render */
        tab = tabs_get_active(&tm);
        SDL_Rect terminal_area = {
            0, TAB_BAR_HEIGHT,
            win.width, win.height - TAB_BAR_HEIGHT
        };
        pane_layout(tab->root, terminal_area);
        render_pane_tree(tab->root, win.renderer, &font);
        pane_draw_dividers(tab->root, win.renderer);

        /* Tool launcher overlay */
        if (tools.launcher.visible)
            tools_launcher_draw(&tools, win.renderer,
                                &font, win.width, win.height);

        window_render_end(&win);

    } /* end main loop */

    tabs_destroy(&tm);
    font_destroy(&font);
    window_destroy(&win);
    return 0;
}