/*
 * ansi.c — Complete ANSI/VT100 terminal emulator.
 *
 * All command-line bugs fixed in this version:
 *
 *  [1] Readline history garbled (Up/Down arrows):
 *      - clear_cell() now resets fg+bg+bold, not just ch
 *      - EL/ED use clear_cell() everywhere
 *      - DCH (P) and ICH (@) implemented for Delete/insert
 *
 *  [2] Path/prompt duplication:
 *      - \n resets cursor_col=0 (matches xterm behavior)
 *
 *  [3] Cursor position wrong after commands:
 *      - CHA (G) correctly maps 1-based param to 0-based col
 *      - CUP (H/f) clamps to valid range
 *
 *  [4] Garbled output from btop/vim/htop:
 *      - OSC sequences (ESC ]) now parsed and silently ignored
 *        instead of leaking their bytes as printable characters
 *      - DECSC (7) / DECRC (8) save/restore cursor implemented
 *      - Alternate screen sequences (?1049h/l) acknowledged
 *
 *  [5] CSI parser corruption:
 *      - Accepts '>' ' ' '\'' as intermediate bytes
 *      - Accepts DCS (ESC P) and PM/APC (ESC ^ ESC _) — skips
 *        their content until ST (ESC \)
 *
 *  [6] Optimizations:
 *      - Dirty-cell tracking: only changed cells set dirty=1
 *      - scroll_up reuses pointer rotation (no memcpy of data)
 *      - clear_cell bounds-checked once, inlined for hot paths
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "ansi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Defaults ───────────────────────────────────────────────── */
static const Color DEFAULT_FG = {200, 200, 200};
static const Color DEFAULT_BG = {  0,   0,   0};

/* ── terminal_create ────────────────────────────────────────── */
Terminal *terminal_create(int cols, int rows) {
    Terminal *t = calloc(1, sizeof(Terminal));
    if (!t) return NULL;

    t->cols          = cols;
    t->rows          = rows;
    t->current_fg    = DEFAULT_FG;
    t->current_bg    = DEFAULT_BG;
    t->state         = STATE_NORMAL;
    t->scroll_offset = 0;
    t->sb_head       = 0;
    t->sb_count      = 0;
    t->alt_screen    = 0;
    t->saved_col     = 0;
    t->saved_row     = 0;

    t->cells = calloc(rows, sizeof(Cell *));
    if (!t->cells) { free(t); return NULL; }

    for (int r = 0; r < rows; r++) {
        t->cells[r] = calloc(cols, sizeof(Cell));
        if (!t->cells[r]) { terminal_destroy(t); return NULL; }
        for (int c = 0; c < cols; c++) {
            t->cells[r][c].ch    = ' ';
            t->cells[r][c].fg    = DEFAULT_FG;
            t->cells[r][c].bg    = DEFAULT_BG;
            t->cells[r][c].dirty = 1;
        }
    }
    return t;
}

/* ── terminal_destroy ───────────────────────────────────────── */
void terminal_destroy(Terminal *t) {
    if (!t) return;
    if (t->cells) {
        for (int r = 0; r < t->rows; r++) free(t->cells[r]);
        free(t->cells);
    }
    for (int i = 0; i < SCROLLBACK_MAX; i++)
        if (t->scrollback[i].cells)
            free(t->scrollback[i].cells);
    free(t);
}

/* ── Internal: clear_cell ───────────────────────────────────── */
/*
 * Resets a cell to blank with default colors.
 *
 * CRITICAL: must reset bg, not just ch.
 * If a cell had a colored background (e.g. green from a
 * prompt) and we only clear ch, the renderer still draws
 * the old background color. Old text "shines through".
 */
static inline void clear_cell(Terminal *t, int row, int col) {
    if ((unsigned)row >= (unsigned)t->rows) return;
    if ((unsigned)col >= (unsigned)t->cols) return;
    Cell *cell = &t->cells[row][col];
    cell->ch    = ' ';
    cell->fg    = DEFAULT_FG;
    cell->bg    = DEFAULT_BG;
    cell->bold  = 0;
    cell->dirty = 1;
}

static void mark_all_dirty(Terminal *t) {
    for (int r = 0; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++)
            t->cells[r][c].dirty = 1;
}

/* ── Internal: scrollback_push ─────────────────────────────── */
static void scrollback_push(Terminal *t, Cell *row) {
    ScrollbackLine *slot = &t->scrollback[t->sb_head];
    if (slot->cells) { free(slot->cells); slot->cells = NULL; }

    slot->cells = malloc(t->cols * sizeof(Cell));
    if (!slot->cells) return;
    memcpy(slot->cells, row, t->cols * sizeof(Cell));
    slot->cols = t->cols;

    t->sb_head = (t->sb_head + 1) % SCROLLBACK_MAX;
    if (t->sb_count < SCROLLBACK_MAX) t->sb_count++;
}

/* ── Internal: scroll_up ────────────────────────────────────── */
static void scroll_up(Terminal *t) {
    /* Save the top row into scrollback history */
    scrollback_push(t, t->cells[0]);

    /* Rotate pointers — O(rows) pointer moves, zero data copy */
    Cell *top = t->cells[0];
    memmove(&t->cells[0], &t->cells[1],
            (t->rows - 1) * sizeof(Cell *));
    t->cells[t->rows - 1] = top;

    /* Clear the new bottom row */
    for (int c = 0; c < t->cols; c++)
        clear_cell(t, t->rows - 1, c);

    /* Keep scrolled viewport stable when new lines arrive */
    if (t->scroll_offset > 0) {
        t->scroll_offset++;
        if (t->scroll_offset > t->sb_count)
            t->scroll_offset = t->sb_count;
    }
}

/* ── Internal: put_char ─────────────────────────────────────── */
static void put_char(Terminal *t, char ch) {
    /* Wrap at right edge */
    if (t->cursor_col >= t->cols) {
        t->cursor_col = 0;
        t->cursor_row++;
    }
    /* Scroll at bottom edge */
    if (t->cursor_row >= t->rows) {
        scroll_up(t);
        t->cursor_row = t->rows - 1;
    }
    Cell *cell  = &t->cells[t->cursor_row][t->cursor_col];
    cell->ch    = ch;
    cell->fg    = t->current_fg;
    cell->bg    = t->current_bg;
    cell->bold  = t->bold;
    cell->dirty = 1;
    t->cursor_col++;
}

/* ── Internal: clamp helpers ────────────────────────────────── */
static inline int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── CSI parameter parsing ──────────────────────────────────── */
static void parse_params(const char *raw, int *out, int *count) {
    *count = 0;
    const char *p = raw;
    while (*p && *count < MAX_PARAMS) {
        char *end;
        out[(*count)++] = (int)strtol(p, &end, 10);
        if (*end == ';') end++;
        if (end == p) break;
        p = end;
    }
    if (*count == 0) { out[0] = 0; *count = 1; }
}

/* ── SGR ────────────────────────────────────────────────────── */
static void apply_sgr(Terminal *t, int *params, int count) {
    for (int i = 0; i < count; i++) {
        int p = params[i];
        switch (p) {
            case 0:
                t->current_fg = DEFAULT_FG;
                t->current_bg = DEFAULT_BG;
                t->bold       = 0;
                break;
            case 1:  t->bold = 1; break;
            case 2: case 22: t->bold = 0; break;
            case 39: t->current_fg = DEFAULT_FG; break;
            case 49: t->current_bg = DEFAULT_BG; break;
            default:
                if (p >= 30 && p <= 37) {
                    t->current_fg = ANSI_COLORS[(p-30) + (t->bold ? 8:0)];
                } else if (p >= 40 && p <= 47) {
                    t->current_bg = ANSI_COLORS[p-40];
                } else if (p >= 90 && p <= 97) {
                    t->current_fg = ANSI_COLORS[(p-90)+8];
                } else if (p >= 100 && p <= 107) {
                    t->current_bg = ANSI_COLORS[(p-100)+8];
                } else if (p == 38) {
                    if (i+2 < count && params[i+1] == 5) {
                        int n = params[i+2];
                        if (n >= 0 && n < 16)
                            t->current_fg = ANSI_COLORS[n];
                        i += 2;
                    } else if (i+4 < count && params[i+1] == 2) {
                        t->current_fg.r = (uint8_t)params[i+2];
                        t->current_fg.g = (uint8_t)params[i+3];
                        t->current_fg.b = (uint8_t)params[i+4];
                        i += 4;
                    }
                } else if (p == 48) {
                    if (i+2 < count && params[i+1] == 5) {
                        int n = params[i+2];
                        if (n >= 0 && n < 16)
                            t->current_bg = ANSI_COLORS[n];
                        i += 2;
                    } else if (i+4 < count && params[i+1] == 2) {
                        t->current_bg.r = (uint8_t)params[i+2];
                        t->current_bg.g = (uint8_t)params[i+3];
                        t->current_bg.b = (uint8_t)params[i+4];
                        i += 4;
                    }
                }
                break;
        }
    }
}

/* ── CSI dispatch ───────────────────────────────────────────── */
static void apply_csi(Terminal *t, char final) {
    int params[MAX_PARAMS];
    int count = 0;
    parse_params(t->params, params, &count);

    int p0 = params[0];
    int p1 = (count > 1) ? params[1] : 1;
    if (p1 < 1) p1 = 1;

    int cr = t->cursor_row;
    int cc = t->cursor_col;

    switch (final) {

        case 'm': apply_sgr(t, params, count); break;

        /* Cursor movement */
        case 'A':
            t->cursor_row = clamp(cr - ((p0<1)?1:p0), 0, t->rows-1);
            break;
        case 'B':
            t->cursor_row = clamp(cr + ((p0<1)?1:p0), 0, t->rows-1);
            break;
        case 'C':
            t->cursor_col = clamp(cc + ((p0<1)?1:p0), 0, t->cols-1);
            break;
        case 'D':
            t->cursor_col = clamp(cc - ((p0<1)?1:p0), 0, t->cols-1);
            break;
        case 'E':
            t->cursor_row = clamp(cr + ((p0<1)?1:p0), 0, t->rows-1);
            t->cursor_col = 0;
            break;
        case 'F':
            t->cursor_row = clamp(cr - ((p0<1)?1:p0), 0, t->rows-1);
            t->cursor_col = 0;
            break;

        case 'G':
            /*
             * CHA — cursor horizontal absolute (1-based).
             * readline sends \x1b[G (p0=0) or \x1b[1G (p0=1)
             * to jump to column 0 before redrawing a line.
             * Both must map to cursor_col = 0.
             */
            t->cursor_col = clamp(((p0<1)?1:p0) - 1, 0, t->cols-1);
            break;

        case 'H': case 'f':
            /* CUP — row;col both 1-based */
            t->cursor_row = clamp(((p0<1)?1:p0) - 1, 0, t->rows-1);
            t->cursor_col = clamp(((p1<1)?1:p1) - 1, 0, t->cols-1);
            break;

        case 'd':
            /* VPA — vertical position absolute (1-based row) */
            t->cursor_row = clamp(((p0<1)?1:p0) - 1, 0, t->rows-1);
            break;

        /* Erase in display */
        case 'J':
            if (p0 == 0) {
                for (int c = cc; c < t->cols; c++)
                    clear_cell(t, cr, c);
                for (int r = cr+1; r < t->rows; r++)
                    for (int c = 0; c < t->cols; c++)
                        clear_cell(t, r, c);
            } else if (p0 == 1) {
                for (int r = 0; r < cr; r++)
                    for (int c = 0; c < t->cols; c++)
                        clear_cell(t, r, c);
                for (int c = 0; c <= cc; c++)
                    clear_cell(t, cr, c);
            } else if (p0 == 2 || p0 == 3) {
                for (int r = 0; r < t->rows; r++)
                    for (int c = 0; c < t->cols; c++)
                        clear_cell(t, r, c);
                t->cursor_row = 0;
                t->cursor_col = 0;
            }
            mark_all_dirty(t);
            break;

        /* Erase in line */
        case 'K':
            /*
             * This is the core of the readline history fix.
             * readline sequence for history redraw:
             *   \r       → cursor_col = 0
             *   \x1b[K   → EL0: clear col 0..end (whole line)
             *   text     → print new content
             *
             * clear_cell() resets bg so colored prompt text
             * from the previous command is truly erased.
             */
            if (p0 == 0) {
                for (int c = cc; c < t->cols; c++)
                    clear_cell(t, cr, c);
            } else if (p0 == 1) {
                for (int c = 0; c <= cc; c++)
                    clear_cell(t, cr, c);
            } else if (p0 == 2) {
                for (int c = 0; c < t->cols; c++)
                    clear_cell(t, cr, c);
            }
            break;

        /* Insert / delete lines */
        case 'L': {
            int n = (p0<1)?1:p0;
            for (int i = 0; i < n; i++) {
                Cell *bottom = t->cells[t->rows-1];
                memmove(&t->cells[cr+1], &t->cells[cr],
                        (t->rows-cr-1)*sizeof(Cell*));
                t->cells[cr] = bottom;
                for (int c = 0; c < t->cols; c++)
                    clear_cell(t, cr, c);
            }
            mark_all_dirty(t);
            break;
        }
        case 'M': {
            int n = (p0<1)?1:p0;
            for (int i = 0; i < n; i++) {
                Cell *top = t->cells[cr];
                memmove(&t->cells[cr], &t->cells[cr+1],
                        (t->rows-cr-1)*sizeof(Cell*));
                t->cells[t->rows-1] = top;
                for (int c = 0; c < t->cols; c++)
                    clear_cell(t, t->rows-1, c);
            }
            mark_all_dirty(t);
            break;
        }

        /* Erase / insert / delete characters */
        case 'X': {
            int n = (p0<1)?1:p0;
            for (int c = cc; c < cc+n && c < t->cols; c++)
                clear_cell(t, cr, c);
            break;
        }

        case 'P':
            /*
             * DCH — delete N chars at cursor (shift left).
             * Used by readline when you press the Delete key.
             */
        {
            int n = (p0<1)?1:p0;
            for (int c = cc; c < t->cols; c++) {
                int src = c + n;
                if (src < t->cols) {
                    t->cells[cr][c] = t->cells[cr][src];
                    t->cells[cr][c].dirty = 1;
                } else {
                    clear_cell(t, cr, c);
                }
            }
            break;
        }

        case '@':
            /*
             * ICH — insert N blank chars at cursor (shift right).
             * Used by readline when you type in the middle of a line.
             */
        {
            int n = (p0<1)?1:p0;
            for (int c = t->cols-1; c >= cc+n; c--) {
                t->cells[cr][c] = t->cells[cr][c-n];
                t->cells[cr][c].dirty = 1;
            }
            for (int c = cc; c < cc+n && c < t->cols; c++)
                clear_cell(t, cr, c);
            break;
        }

        /* Scroll up / down */
        case 'S': {
            int n = (p0<1)?1:p0;
            for (int i = 0; i < n; i++) scroll_up(t);
            break;
        }
        case 'T': {
            /* Scroll down — insert blank line at top */
            int n = (p0<1)?1:p0;
            for (int i = 0; i < n; i++) {
                Cell *bottom = t->cells[t->rows-1];
                memmove(&t->cells[1], &t->cells[0],
                        (t->rows-1)*sizeof(Cell*));
                t->cells[0] = bottom;
                for (int c = 0; c < t->cols; c++)
                    clear_cell(t, 0, c);
            }
            mark_all_dirty(t);
            break;
        }

        /* Mode set / reset */
        case 'h':
        case 'l': {
            /*
             * Handle the most important mode flags.
             * ?25h/l = show/hide cursor (we always show it)
             * ?1049h/l = alternate screen (acknowledge only)
             * ?2004h/l = bracketed paste (acknowledge only)
             */
            /* All mode changes silently acknowledged */
            break;
        }

        /* Cursor save / restore (CSI versions) */
        case 's':
            t->saved_col = t->cursor_col;
            t->saved_row = t->cursor_row;
            break;
        case 'u':
            t->cursor_col = clamp(t->saved_col, 0, t->cols-1);
            t->cursor_row = clamp(t->saved_row, 0, t->rows-1);
            break;

        /* Repeat last char */
        case 'b':
            /* REP — not critical, ignore */
            break;

        /* Status / device queries — ignore */
        case 'n': case 'c': case 'r':
            break;

        default:
            break;
    }
}

/* ── terminal_process ───────────────────────────────────────── */
void terminal_process(Terminal *t, const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];

        switch (t->state) {

            /* ══ NORMAL ══════════════════════════════════════ */
            case STATE_NORMAL:

                if (c == 0x1b) {
                    t->state = STATE_ESCAPE;

                } else if (c == '\r') {
                    /*
                     * CR — go to column 0.
                     * readline uses \r to go back to line start
                     * before erasing and redrawing the command.
                     */
                    t->cursor_col = 0;

                } else if (c == '\n') {
                    /*
                     * LF — next row AND reset col=0.
                     *
                     * Always reset col on \n to match xterm.
                     * Fixes prompt duplication for programs that
                     * send bare \n without a preceding \r.
                     */
                    t->cursor_col = 0;
                    t->cursor_row++;
                    if (t->cursor_row >= t->rows) {
                        scroll_up(t);
                        t->cursor_row = t->rows - 1;
                    }

                } else if (c == '\b') {
                    if (t->cursor_col > 0) t->cursor_col--;

                } else if (c == '\t') {
                    t->cursor_col = (t->cursor_col + 8) & ~7;
                    if (t->cursor_col >= t->cols)
                        t->cursor_col = t->cols - 1;

                } else if (c == 0x07 || c == 0x0e || c == 0x0f ||
                           c == 0x00 || c == 0x7f || c == 0x05) {
                    /* BEL SO SI NUL DEL ENQ — all ignored */

                } else if (c >= 0x20 && c < 0x7f) {
                    put_char(t, (char)c);

                } else if (c >= 0xa0) {
                    put_char(t, '?');   /* non-ASCII placeholder */
                }
                /* 0x80-0x9f: C1 controls — skip */
                break;

            /* ══ ESCAPE ══════════════════════════════════════ */
            case STATE_ESCAPE:

                if (c == '[') {
                    t->state      = STATE_CSI;
                    t->params_len = 0;
                    memset(t->params, 0, sizeof(t->params));

                } else if (c == ']') {
                    /*
                     * OSC — Operating System Command.
                     * bash uses this to set the terminal title:
                     *   ESC ] 0 ; title ST
                     * Without catching this, the title bytes leak
                     * as printable characters — garbling the screen.
                     */
                    t->state   = STATE_OSC;
                    t->osc_len = 0;
                    memset(t->osc_buf, 0, sizeof(t->osc_buf));

                } else if (c == 'P' || c == '^' || c == '_') {
                    /*
                     * DCS / PM / APC — device control / private /
                     * application program command strings.
                     * We skip their content until ST (ESC \).
                     * Reuse OSC state for simplicity.
                     */
                    t->state   = STATE_OSC;
                    t->osc_len = 0;

                } else if (c == 'c') {
                    /* RIS — full reset */
                    for (int r = 0; r < t->rows; r++)
                        for (int col = 0; col < t->cols; col++)
                            clear_cell(t, r, col);
                    t->cursor_row = 0; t->cursor_col = 0;
                    t->current_fg = DEFAULT_FG;
                    t->current_bg = DEFAULT_BG;
                    t->bold       = 0;
                    t->state      = STATE_NORMAL;

                } else if (c == 'M') {
                    /* RI — reverse index */
                    if (t->cursor_row > 0) t->cursor_row--;
                    t->state = STATE_NORMAL;

                } else if (c == '7') {
                    /* DECSC — save cursor */
                    t->saved_col = t->cursor_col;
                    t->saved_row = t->cursor_row;
                    t->state = STATE_NORMAL;

                } else if (c == '8') {
                    /* DECRC — restore cursor */
                    t->cursor_col = clamp(t->saved_col, 0, t->cols-1);
                    t->cursor_row = clamp(t->saved_row, 0, t->rows-1);
                    t->state = STATE_NORMAL;

                } else if (c == '=' || c == '>') {
                    /* DECPAM/DECPNM — keypad mode, ignore */
                    t->state = STATE_NORMAL;

                } else if (c == '(' || c == ')' ||
                           c == '*' || c == '+') {
                    /* Character set designation, ignore */
                    t->state = STATE_NORMAL;

                } else if (c == '\\') {
                    /* ST — string terminator (bare), ignore */
                    t->state = STATE_NORMAL;

                } else {
                    t->state = STATE_NORMAL;
                }
                break;

            /* ══ CSI ═════════════════════════════════════════ */
            case STATE_CSI:

                if ((c >= '0' && c <= '9') ||
                    c == ';' || c == '?' || c == '!' ||
                    c == '"' || c == '$' || c == '>' ||
                    c == ' ' || c == '\'') {
                    if (t->params_len < (int)sizeof(t->params)-1) {
                        t->params[t->params_len++] = (char)c;
                        t->params[t->params_len]   = '\0';
                    }
                } else if (c >= 0x40 && c <= 0x7e) {
                    apply_csi(t, (char)c);
                    t->state = STATE_NORMAL;
                } else if (c == 0x1b) {
                    /* ESC inside CSI — abort current, start fresh */
                    t->state = STATE_ESCAPE;
                } else {
                    t->state = STATE_NORMAL;
                }
                break;

            /* ══ OSC ═════════════════════════════════════════ */
            case STATE_OSC:
                /*
                 * Accumulate OSC content until ST (ESC \) or BEL (0x07).
                 * We silently ignore the content — we just need to
                 * consume all the bytes so they don't appear on screen.
                 *
                 * Common OSC sequences bash sends:
                 *   ESC]0;title BEL  — set window title
                 *   ESC]7;uri BEL    — set working directory
                 *   ESC]133;A BEL    — shell integration marks
                 */
                if (c == 0x07) {
                    /* BEL terminates OSC */
                    t->state = STATE_NORMAL;
                } else if (c == 0x1b) {
                    /* ESC — may be start of ST (ESC \) */
                    /* We just go back to ESCAPE state */
                    t->state = STATE_ESCAPE;
                } else {
                    /* Accumulate (but don't overflow) */
                    if (t->osc_len < (int)sizeof(t->osc_buf)-1) {
                        t->osc_buf[t->osc_len++] = (char)c;
                    }
                }
                break;
        }
    }
}

/* ── terminal_resize ────────────────────────────────────────── */
void terminal_resize(Terminal *t, int cols, int rows) {
    if (t->cells) {
        for (int r = 0; r < t->rows; r++) free(t->cells[r]);
        free(t->cells);
        t->cells = NULL;
    }
    t->cols = cols;
    t->rows = rows;

    t->cells = calloc(rows, sizeof(Cell *));
    if (!t->cells) return;

    for (int r = 0; r < rows; r++) {
        t->cells[r] = calloc(cols, sizeof(Cell));
        if (!t->cells[r]) return;
        for (int c = 0; c < cols; c++) {
            t->cells[r][c].ch    = ' ';
            t->cells[r][c].fg    = DEFAULT_FG;
            t->cells[r][c].bg    = DEFAULT_BG;
            t->cells[r][c].dirty = 1;
        }
    }

    t->cursor_row = clamp(t->cursor_row, 0, rows-1);
    t->cursor_col = clamp(t->cursor_col, 0, cols-1);
}

/* ── Scrollback public API ──────────────────────────────────── */
ScrollbackLine *scrollback_get(Terminal *t, int index) {
    if (index < 0 || index >= t->sb_count) return NULL;
    int pos = (t->sb_head - 1 - index + SCROLLBACK_MAX)
              % SCROLLBACK_MAX;
    return &t->scrollback[pos];
}

Cell *terminal_get_display_row(Terminal *t, int screen_row) {
    if (t->scroll_offset == 0)
        return t->cells[screen_row];

    int total       = t->sb_count + t->rows;
    int view_bottom = total - t->scroll_offset;
    int view_top    = view_bottom - t->rows;
    int line_index  = view_top + screen_row;

    if (line_index < 0 || line_index >= total)
        return NULL;

    if (line_index < t->sb_count) {
        int sb_idx = (t->sb_count - 1) - line_index;
        ScrollbackLine *sl = scrollback_get(t, sb_idx);
        return sl ? sl->cells : NULL;
    }

    int live = line_index - t->sb_count;
    return (live >= 0 && live < t->rows) ? t->cells[live] : NULL;
}