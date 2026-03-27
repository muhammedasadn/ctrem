/*
 * ansi.c — ANSI/VT100 terminal emulator — final complete version.
 *
 * All bugs fixed:
 *   [1] Blank screen — OSC state fixed, never leaks bytes
 *   [2] Path duplication — \n resets cursor_col=0
 *   [3] History garbled — clear_cell() resets bg+fg+bold
 *   [4] Delete key — DCH (case 'P') implemented
 *   [5] Mid-line typing — ICH (case '@') implemented
 *   [6] NUL/DEL bytes — explicitly ignored
 *   [7] CSI parser — accepts '>' and ' ' intermediate bytes
 *   [8] DECSC/DECRC — cursor save/restore implemented
 *
 * Module 11 optimization:
 *   frame_dirty flag on Terminal struct.
 *   Set to 1 whenever terminal_process() modifies any cell.
 *   The render loop uses this to skip full redraws on idle frames.
 *   clear_cell() and put_char() both set cell->dirty=1 so the
 *   per-cell dirty check in main.c works correctly.
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
    t->frame_dirty   = 1;   /* force full draw on first frame */

    t->cells = calloc(rows, sizeof(Cell *));
    if (!t->cells) { free(t); return NULL; }

    for (int r = 0; r < rows; r++) {
        t->cells[r] = calloc(cols, sizeof(Cell));
        if (!t->cells[r]) { terminal_destroy(t); return NULL; }
        for (int c = 0; c < cols; c++) {
            t->cells[r][c].ch    = ' ';
            t->cells[r][c].fg    = DEFAULT_FG;
            t->cells[r][c].bg    = DEFAULT_BG;
            t->cells[r][c].bold  = 0;
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


/* ── Internal helpers ───────────────────────────────────────── */

/*
 * clear_cell — set one cell to blank with DEFAULT colors.
 *
 * MUST reset bg as well as ch.
 * If only ch=' ' but bg stays colored, the renderer draws
 * the old background — old text "shines through" the erase.
 * This was the root cause of the readline history redraw bug.
 *
 * Sets frame_dirty so the render loop knows something changed.
 */
static inline void clear_cell(Terminal *t, int row, int col) {
    if ((unsigned)row >= (unsigned)t->rows) return;
    if ((unsigned)col >= (unsigned)t->cols) return;
    Cell *cell  = &t->cells[row][col];
    cell->ch    = ' ';
    cell->fg    = DEFAULT_FG;
    cell->bg    = DEFAULT_BG;
    cell->bold  = 0;
    cell->dirty = 1;
    t->frame_dirty = 1;
}

static void mark_all_dirty(Terminal *t) {
    for (int r = 0; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++)
            t->cells[r][c].dirty = 1;
    t->frame_dirty = 1;
}

static inline int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Scrollback ─────────────────────────────────────────────── */

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

static void scroll_up(Terminal *t) {
    /* Save top row into scrollback before losing it */
    scrollback_push(t, t->cells[0]);

    /* Rotate pointers — O(rows) moves, zero data copy */
    Cell *top = t->cells[0];
    memmove(&t->cells[0], &t->cells[1],
            (t->rows - 1) * sizeof(Cell *));
    t->cells[t->rows - 1] = top;

    /* Clear new bottom row */
    for (int c = 0; c < t->cols; c++)
        clear_cell(t, t->rows - 1, c);

    /* Keep scrolled viewport stable as new lines arrive */
    if (t->scroll_offset > 0) {
        t->scroll_offset++;
        if (t->scroll_offset > t->sb_count)
            t->scroll_offset = t->sb_count;
    }

    t->frame_dirty = 1;
}

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
    t->frame_dirty = 1;

    t->cursor_col++;
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


/* ── SGR — Select Graphic Rendition ────────────────────────── */

static void apply_sgr(Terminal *t, int *params, int count) {
    for (int i = 0; i < count; i++) {
        int p = params[i];
        switch (p) {
            case 0:
                t->current_fg = DEFAULT_FG;
                t->current_bg = DEFAULT_BG;
                t->bold = 0;
                break;
            case 1:  t->bold = 1; break;
            case 2: case 22: t->bold = 0; break;
            case 39: t->current_fg = DEFAULT_FG; break;
            case 49: t->current_bg = DEFAULT_BG; break;
            default:
                if (p >= 30 && p <= 37)
                    t->current_fg = ANSI_COLORS[(p-30) + (t->bold ? 8:0)];
                else if (p >= 40 && p <= 47)
                    t->current_bg = ANSI_COLORS[p-40];
                else if (p >= 90 && p <= 97)
                    t->current_fg = ANSI_COLORS[(p-90)+8];
                else if (p >= 100 && p <= 107)
                    t->current_bg = ANSI_COLORS[(p-100)+8];
                else if (p == 38) {
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


/* ── CSI command dispatch ───────────────────────────────────── */

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

        /* ── SGR ── */
        case 'm':
            apply_sgr(t, params, count);
            break;

        /* ── Cursor movement ── */
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
             * readline sends \x1b[G or \x1b[1G to jump to col 0.
             * Both p0=0 and p0=1 must map to cursor_col=0.
             */
            t->cursor_col = clamp(((p0<1)?1:p0) - 1, 0, t->cols-1);
            break;

        case 'H': case 'f':
            /* CUP — cursor position row;col (1-based) */
            t->cursor_row = clamp(((p0<1)?1:p0) - 1, 0, t->rows-1);
            t->cursor_col = clamp(((p1<1)?1:p1) - 1, 0, t->cols-1);
            break;

        case 'd':
            /* VPA — vertical position absolute (1-based row) */
            t->cursor_row = clamp(((p0<1)?1:p0) - 1, 0, t->rows-1);
            break;

        /* ── Erase in display ── */
        case 'J': {
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
        }

        /* ── Erase in line ── */
        case 'K':
            /*
             * EL — erase in line. Core of readline history fix.
             *
             * readline Up/Down arrow redraw sequence:
             *   \r       → cursor_col = 0
             *   \x1b[K   → EL0: erase col 0..end (whole line)
             *   text     → print new command on clean line
             *
             * clear_cell() resets bg so colored prompt backgrounds
             * are truly erased — not just hidden by a space char.
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

        /* ── Insert / delete lines ── */
        case 'L': {
            int n = (p0<1)?1:p0;
            for (int i = 0; i < n; i++) {
                Cell *bot = t->cells[t->rows-1];
                memmove(&t->cells[cr+1], &t->cells[cr],
                        (t->rows-cr-1)*sizeof(Cell*));
                t->cells[cr] = bot;
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

        /* ── Erase / insert / delete characters ── */
        case 'X': {
            int n = (p0<1)?1:p0;
            for (int c = cc; c < cc+n && c < t->cols; c++)
                clear_cell(t, cr, c);
            break;
        }

        case 'P':
            /*
             * DCH — delete N characters at cursor (shift left).
             * Used by readline when you press the Delete key.
             *
             * Example N=1, cursor at col 3:
             *   Before: a b c [X] e f g _
             *   After:  a b c  e  f g _ _
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
            t->frame_dirty = 1;
            break;
        }

        case '@':
            /*
             * ICH — insert N blank chars at cursor (shift right).
             * Used by readline when typing mid-line.
             *
             * Example N=1, cursor at col 3:
             *   Before: a b c [X] e f g h
             *   After:  a b c  _  X e f g
             */
        {
            int n = (p0<1)?1:p0;
            for (int c = t->cols-1; c >= cc+n; c--) {
                t->cells[cr][c] = t->cells[cr][c-n];
                t->cells[cr][c].dirty = 1;
            }
            for (int c = cc; c < cc+n && c < t->cols; c++)
                clear_cell(t, cr, c);
            t->frame_dirty = 1;
            break;
        }

        /* ── Scroll up / down ── */
        case 'S': {
            int n = (p0<1)?1:p0;
            for (int i = 0; i < n; i++) scroll_up(t);
            break;
        }
        case 'T': {
            int n = (p0<1)?1:p0;
            for (int i = 0; i < n; i++) {
                Cell *bot = t->cells[t->rows-1];
                memmove(&t->cells[1], &t->cells[0],
                        (t->rows-1)*sizeof(Cell*));
                t->cells[0] = bot;
                for (int c = 0; c < t->cols; c++)
                    clear_cell(t, 0, c);
            }
            mark_all_dirty(t);
            break;
        }

        /* ── Cursor save / restore (CSI versions) ── */
        case 's':
            t->saved_col = t->cursor_col;
            t->saved_row = t->cursor_row;
            break;
        case 'u':
            t->cursor_col = clamp(t->saved_col, 0, t->cols-1);
            t->cursor_row = clamp(t->saved_row, 0, t->rows-1);
            break;

        /* ── Mode / status — all silently acknowledged ── */
        case 'h': case 'l': case 'r':
        case 'n': case 'c': case 'b':
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
                     * readline uses \r before redrawing the command line.
                     */
                    t->cursor_col = 0;

                } else if (c == '\n') {
                    /*
                     * LF — advance row AND reset col=0.
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
                    t->frame_dirty = 1;

                } else if (c == '\b') {
                    /* BS — backspace */
                    if (t->cursor_col > 0) t->cursor_col--;

                } else if (c == '\t') {
                    /* HT — tab to next 8-column boundary */
                    t->cursor_col = (t->cursor_col + 8) & ~7;
                    if (t->cursor_col >= t->cols)
                        t->cursor_col = t->cols - 1;

                } else if (c == 0x07 || c == 0x0e || c == 0x0f ||
                           c == 0x00 || c == 0x7f || c == 0x05) {
                    /* BEL SO SI NUL DEL ENQ — all ignored */

                } else if (c >= 0x20 && c < 0x7f) {
                    /* Printable ASCII */
                    put_char(t, (char)c);

                } else if (c >= 0xa0) {
                    /* High-byte — draw placeholder */
                    put_char(t, '?');
                }
                /* 0x80–0x9f: C1 controls — ignored */
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
                     * bash uses this constantly:
                     *   ESC ] 0 ; title BEL  — set window title
                     *   ESC ] 7 ; uri BEL    — set working directory
                     *   ESC ] 133 ; A BEL    — shell integration
                     *
                     * Every byte of the OSC content must be consumed.
                     * If any byte leaks into STATE_NORMAL it gets
                     * printed as a character — causing garbled output
                     * or a blank screen.
                     */
                    t->state   = STATE_OSC;
                    t->osc_len = 0;
                    memset(t->osc_buf, 0, sizeof(t->osc_buf));

                } else if (c == 'P' || c == '^' || c == '_') {
                    /* DCS / PM / APC — treat same as OSC */
                    t->state   = STATE_OSC;
                    t->osc_len = 0;

                } else if (c == 'c') {
                    /* RIS — full terminal reset */
                    for (int r = 0; r < t->rows; r++)
                        for (int col = 0; col < t->cols; col++)
                            clear_cell(t, r, col);
                    t->cursor_row = 0; t->cursor_col = 0;
                    t->current_fg = DEFAULT_FG;
                    t->current_bg = DEFAULT_BG;
                    t->bold = 0;
                    t->state = STATE_NORMAL;

                } else if (c == 'M') {
                    /* RI — reverse index (scroll down one line) */
                    if (t->cursor_row > 0) t->cursor_row--;
                    t->state = STATE_NORMAL;

                } else if (c == '7') {
                    /* DECSC — save cursor position */
                    t->saved_col = t->cursor_col;
                    t->saved_row = t->cursor_row;
                    t->state = STATE_NORMAL;

                } else if (c == '8') {
                    /* DECRC — restore cursor position */
                    t->cursor_col = clamp(t->saved_col, 0, t->cols-1);
                    t->cursor_row = clamp(t->saved_row, 0, t->rows-1);
                    t->state = STATE_NORMAL;

                } else if (c == '=' || c == '>') {
                    /* DECPAM / DECPNM — keypad mode, ignore */
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
                    /* Parameter or intermediate byte */
                    if (t->params_len < (int)sizeof(t->params)-1) {
                        t->params[t->params_len++] = (char)c;
                        t->params[t->params_len]   = '\0';
                    }
                } else if (c >= 0x40 && c <= 0x7e) {
                    /* Final byte — dispatch command */
                    apply_csi(t, (char)c);
                    t->state = STATE_NORMAL;
                } else if (c == 0x1b) {
                    /* ESC inside CSI — abort, start fresh */
                    t->state = STATE_ESCAPE;
                } else {
                    /* Unexpected byte — abort */
                    t->state = STATE_NORMAL;
                }
                break;

            /* ══ OSC ═════════════════════════════════════════ */
            case STATE_OSC:
                /*
                 * Consume OSC content until:
                 *   BEL (0x07) — shell uses this as OSC terminator
                 *   ESC (0x1b) — start of ST = ESC \
                 *
                 * NEVER let OSC bytes reach STATE_NORMAL.
                 * This was the root cause of the blank screen bug:
                 * bash's title-setting OSC sequence filled cells
                 * with the title string before the prompt rendered.
                 */
                if (c == 0x07) {
                    /* BEL terminates OSC — back to normal */
                    t->state = STATE_NORMAL;
                } else if (c == 0x1b) {
                    /*
                     * ESC — start of ST (ESC \).
                     * Transition to ESCAPE; the following '\' will
                     * be handled there as a bare ST terminator.
                     */
                    t->state = STATE_ESCAPE;
                }
                /* All other bytes silently consumed */
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

    t->cursor_row  = clamp(t->cursor_row,  0, rows-1);
    t->cursor_col  = clamp(t->cursor_col,  0, cols-1);
    t->frame_dirty = 1;
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