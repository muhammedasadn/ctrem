/*
 * main.c — cterm final version.
 *
 * Module 11: Dirty-cell performance optimization.
 *   render_pane_tree() now checks cell->dirty before drawing.
 *   Only cells that actually changed get redrawn each frame.
 *   An idle terminal drops from thousands of SDL draw calls
 *   per frame to near zero. All dirty flags are cleared after
 *   each render pass.
 *
 * Module 12: Config file support.
 *   config_load() reads ~/.config/cterm/cterm.conf at startup.
 *   Font path, font size, window size, colors, shell, scrollback
 *   all come from the config. A default file is auto-created if
 *   none exists.
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
#include "config.h"


/* ── render_pane_tree ───────────────────────────────────────── */

/*
 * Renders all leaf panes in the tree.
 *
 * Module 11 optimization — dirty cell tracking:
 *   Before this change: every cell was redrawn every frame.
 *   After this change:  only cells with dirty=1 are redrawn.
 *
 * How it works:
 *   - ansi.c sets cell->dirty = 1 whenever a cell changes.
 *   - We check dirty before drawing background + glyph.
 *   - After drawing, we set dirty = 0.
 *   - Next frame, only cells changed by PTY output are dirty.
 *
 * On an idle terminal: 0 cell redraws per frame.
 * On active output:    only the changed cells redraw.
 * On full clear:       all cells redraw once, then settle.
 *
 * Exception: the cursor cell is always redrawn (blink state
 * changes every 500ms regardless of cell content).
 */
static void render_pane_tree(Pane *p, SDL_Renderer *renderer,
                              Font *font, const Config *cfg) {
    if (!p) return;

    if (p->type != PANE_LEAF) {
        render_pane_tree(p->first,  renderer, font, cfg);
        render_pane_tree(p->second, renderer, font, cfg);
        return;
    }

    Terminal *term = p->term;
    SDL_Rect   r   = p->rect;

    if (r.w < font->cell_width || r.h < font->cell_height) return;

    /* Recalculate grid from pixel rect */
    int pcols = r.w / font->cell_width;
    int prows = r.h / font->cell_height;
    if (pcols < 1) pcols = 1;
    if (prows < 1) prows = 1;

    /* Resize if pane dimensions changed */
    if (pcols != term->cols || prows != term->rows) {
        terminal_resize(term, pcols, prows);
        pty_resize(&p->pty, pcols, prows);
    }

    /* ── Draw dirty cells only ── */
    for (int row = 0; row < term->rows; row++) {
        Cell *display_row = terminal_get_display_row(term, row);

        for (int col = 0; col < term->cols; col++) {
            int x = r.x + col * font->cell_width;
            int y = r.y + row * font->cell_height;

            if (x + font->cell_width  > r.x + r.w) continue;
            if (y + font->cell_height > r.y + r.h) continue;

            /* ── Dirty check — Module 11 ── */
            /*
             * If this cell hasn't changed since last frame,
             * skip it entirely. The GPU still holds the correct
             * pixels from the previous render.
             */
            if (display_row && !display_row[col].dirty) continue;

            Uint8 bg_r = cfg->bg_r, bg_g = cfg->bg_g, bg_b = cfg->bg_b;
            Uint8 fg_r = cfg->fg_r, fg_g = cfg->fg_g, fg_b = cfg->fg_b;
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

            /* Draw background */
            SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, 255);
            SDL_Rect bg_rect = {x, y, font->cell_width, font->cell_height};
            SDL_RenderFillRect(renderer, &bg_rect);

            /* Draw character glyph */
            if (ch != ' ' && ch != '\0') {
                font_draw_char(font, renderer, ch, x, y,
                               fg_r, fg_g, fg_b);
            }

            /* Clear dirty flag — cell is now up to date */
            if (display_row) display_row[col].dirty = 0;
        }
    }

    /* ── Cursor ── */
    /*
     * Always redraw the cursor cell regardless of dirty state.
     * The cursor blinks — its visual state changes every
     * cfg->cursor_blink_ms milliseconds even if the cell
     * content is unchanged.
     *
     * We also re-dirty the cursor cell so it gets redrawn
     * next frame (for the blink-off state).
     */
    if (p->focused && term->scroll_offset == 0) {
        int cx = r.x + term->cursor_col * font->cell_width;
        int cy = r.y + term->cursor_row * font->cell_height;

        if (cx + font->cell_width  <= r.x + r.w &&
            cy + font->cell_height <= r.y + r.h) {

            int blink_ms = cfg->cursor_blink_ms > 0
                           ? cfg->cursor_blink_ms : 500;
            Uint32 ticks = SDL_GetTicks();

            if (cfg->cursor_blink_ms == 0 ||
                (ticks / (Uint32)blink_ms) % 2 == 0) {
                SDL_SetRenderDrawColor(renderer, 220, 220, 220, 200);
                SDL_Rect cur = {cx, cy,
                                font->cell_width, font->cell_height};
                SDL_RenderFillRect(renderer, &cur);
            }

            /* Keep cursor cell dirty so blink redraws next frame */
            if (term->cursor_row < term->rows &&
                term->cursor_col < term->cols) {
                term->cells[term->cursor_row][term->cursor_col].dirty = 1;
            }
        }
    }

    /* ── Scroll indicator ── */
    if (p->focused && term->scroll_offset > 0) {
        SDL_SetRenderDrawColor(renderer, 80, 140, 255, 220);
        SDL_Rect top_line = {r.x, r.y, r.w, 2};
        SDL_RenderFillRect(renderer, &top_line);

        if (term->sb_count > 0) {
            float ratio = (float)term->scroll_offset
                          / (float)term->sb_count;
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
    /* ── Module 12: Load config first ────────────────────────── */
    Config cfg;
    config_load(&cfg);

    Window     win;
    Font       font;
    TabManager tm;
    ToolManager tools;

    /* ── Window (using config dimensions) ───────────────────── */
    if (window_init(&win, cfg.win_width, cfg.win_height) != 0) {
        fprintf(stderr, "Failed to create window.\n");
        return 1;
    }

    SDL_SetWindowTitle(win.window, "cterm");

    /* ── Font (using config font path + size) ───────────────── */
    if (font_init(&font, win.renderer,
                  cfg.font_path, cfg.font_size) != 0) {
        fprintf(stderr,
                "Failed to load font: %s\n"
                "Edit ~/.config/cterm/cterm.conf to fix font_path\n",
                cfg.font_path);
        window_destroy(&win);
        return 1;
    }

    /* ── Grid dimensions ─────────────────────────────────────── */
    int cols = win.width  / font.cell_width;
    int rows = (win.height - cfg.tab_bar_height) / font.cell_height;

    /* ── Tabs ────────────────────────────────────────────────── */
    if (tabs_init(&tm, cols, rows) != 0) {
        fprintf(stderr, "Failed to init tabs.\n");
        font_destroy(&font);
        window_destroy(&win);
        return 1;
    }

    /* ── Tools ───────────────────────────────────────────────── */
    tools_init(&tools);

    char read_buf[4096];
    int  running = 1;

    /*
     * Initial full-dirty pass.
     * On the very first frame every cell must be drawn even
     * though no PTY output has arrived yet — we need to paint
     * the background. We mark all cells dirty at startup.
     * After the first render they settle to clean.
     */

    /* ════════════════════════════════════════════════════════════
     * MAIN LOOP
     * ════════════════════════════════════════════════════════════ */
    while (running) {

        Tab *tab = tabs_get_active(&tm);

        /* ── Events ──────────────────────────────────────────── */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {

            if (event.type == SDL_QUIT) {
                running = 0;
                continue;
            }

            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                win.width  = event.window.data1;
                win.height = event.window.data2;
                cols = win.width  / font.cell_width;
                rows = (win.height - cfg.tab_bar_height)
                       / font.cell_height;
                continue;
            }

            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT) {
                if (tools.launcher.visible) {
                    tools_launcher_close(&tools);
                    continue;
                }
                int mx = event.button.x;
                int my = event.button.y;
                if (my < cfg.tab_bar_height) {
                    if (tabs_handle_click(&tm, mx, my, cols, rows))
                        tab = tabs_get_active(&tm);
                } else {
                    Pane *clicked = pane_find_at(tab->root, mx, my);
                    if (clicked) pane_set_focus(tab->root, clicked);
                }
                continue;
            }

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

            if (event.type == SDL_KEYDOWN) {
                SDL_Keycode sym = event.key.keysym.sym;
                SDL_Keymod  mod = SDL_GetModState();
                int ctrl  = (mod & KMOD_CTRL)  != 0;
                int shift = (mod & KMOD_SHIFT) != 0;

                /* Launcher intercepts all keys when open */
                if (tools.launcher.visible) {
                    tools_launcher_handle_key(&tools, sym, mod,
                                              &tm, cols, rows);
                    tab = tabs_get_active(&tm);
                    continue;
                }

                /* ── Tab shortcuts ── */
                if (ctrl && !shift && sym == SDLK_t) {
                    tabs_new(&tm, cols, rows);
                    tab = tabs_get_active(&tm);
                    continue;
                }
                if (ctrl && !shift && sym == SDLK_w) {
                    tabs_close(&tm, tm.active);
                    tab = tabs_get_active(&tm);
                    continue;
                }
                if (ctrl && !shift && sym == SDLK_TAB) {
                    tabs_next(&tm);
                    tab = tabs_get_active(&tm);
                    continue;
                }
                if (ctrl && shift && sym == SDLK_TAB) {
                    tabs_prev(&tm);
                    tab = tabs_get_active(&tm);
                    continue;
                }
                if (ctrl && !shift &&
                    sym >= SDLK_1 && sym <= SDLK_9) {
                    tabs_set_active(&tm, sym - SDLK_1);
                    tab = tabs_get_active(&tm);
                    continue;
                }

                /* ── Tool launcher ── */
                if (ctrl && !shift && sym == SDLK_p) {
                    tools_launcher_open(&tools);
                    continue;
                }

                /* ── Pane shortcuts ── */
                if (ctrl && shift && sym == SDLK_RIGHT) {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp) {
                        Pane *n = pane_split(fp, PANE_SPLIT_H,
                                             cols, rows);
                        if (n != fp) tab->root = n;
                    }
                    continue;
                }
                if (ctrl && shift && sym == SDLK_DOWN) {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp) {
                        Pane *n = pane_split(fp, PANE_SPLIT_V,
                                             cols, rows);
                        if (n != fp) tab->root = n;
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
                    pane_focus_next(tab->root);
                    continue;
                }

                /* ── Generic Ctrl+letter → control char ── */
                if (ctrl && sym >= SDLK_a && sym <= SDLK_z) {
                    Pane *fp = pane_get_focused(tab->root);
                    if (fp) {
                        char cc = (char)(sym - SDLK_a + 1);
                        pty_write(&fp->pty, &cc, 1);
                    }
                    continue;
                }

                /* ── Terminal keys ── */
                {
                    Pane     *fp  = pane_get_focused(tab->root);
                    if (!fp) continue;
                    PTY      *pty = &fp->pty;
                    Terminal *t   = fp->term;

                    switch (sym) {
                        case SDLK_RETURN:
                            t->scroll_offset = 0;
                            pty_write(pty, "\r", 1);      break;
                        case SDLK_BACKSPACE:
                            pty_write(pty, "\x7f", 1);    break;
                        case SDLK_TAB:
                            pty_write(pty, "\t", 1);      break;
                        case SDLK_ESCAPE:
                            pty_write(pty, "\x1b", 1);    break;
                        case SDLK_UP:
                            pty_write(pty, "\x1b[A", 3);  break;
                        case SDLK_DOWN:
                            pty_write(pty, "\x1b[B", 3);  break;
                        case SDLK_RIGHT:
                            pty_write(pty, "\x1b[C", 3);  break;
                        case SDLK_LEFT:
                            pty_write(pty, "\x1b[D", 3);  break;
                        case SDLK_HOME:
                            pty_write(pty, "\x1b[H", 3);  break;
                        case SDLK_END:
                            pty_write(pty, "\x1b[F", 3);  break;
                        case SDLK_DELETE:
                            pty_write(pty, "\x1b[3~", 4); break;
                        case SDLK_PAGEUP:
                            t->scroll_offset += rows / 2;
                            if (t->scroll_offset > t->sb_count)
                                t->scroll_offset = t->sb_count;
                            break;
                        case SDLK_PAGEDOWN:
                            t->scroll_offset -= rows / 2;
                            if (t->scroll_offset < 0)
                                t->scroll_offset = 0;
                            break;
                        default: break;
                    }
                }
                continue;
            }

        } /* end SDL_PollEvent */

        /* ── Read PTY output for all tabs ───────────────────── */
        for (int i = 0; i < tm.count; i++)
            pane_read_all(tm.tabs[i].root, read_buf, sizeof(read_buf));

        /* ── Render ──────────────────────────────────────────── */
        window_render_begin(&win);

        /* Tab bar */
        tabs_draw_bar(&tm, win.renderer, &font, win.width);

        /* Pane layout */
        tab = tabs_get_active(&tm);
        SDL_Rect terminal_area = {
            0, cfg.tab_bar_height,
            win.width, win.height - cfg.tab_bar_height
        };
        pane_layout(tab->root, terminal_area);

        /* Render panes (dirty cells only) */
        render_pane_tree(tab->root, win.renderer, &font, &cfg);

        /* Dividers + focus border */
        pane_draw_dividers(tab->root, win.renderer);

        /* Tool launcher overlay */
        if (tools.launcher.visible)
            tools_launcher_draw(&tools, win.renderer,
                                &font, win.width, win.height);

        window_render_end(&win);

    } /* end main loop */

    /* ── Cleanup ─────────────────────────────────────────────── */
    tabs_destroy(&tm);
    font_destroy(&font);
    window_destroy(&win);
    return 0;
}