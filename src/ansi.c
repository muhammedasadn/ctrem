/*
 * ansi.c — ANSI/VT100 terminal emulator implementation.
 *
 * This file implements:
 *   - Terminal grid allocation and destruction
 *   - The 3-state ANSI escape sequence parser
 *   - SGR (color + bold) handling
 *   - Cursor movement commands
 *   - Screen and line erase commands
 *   - Scrollback ring buffer (push, get, display row)
 *   - Terminal resize
 *
 * The public API is declared in ansi.h.
 * Include ansi.h — do not include this file directly.
 */

#define _GNU_SOURCE

#include "ansi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ── Default colors ─────────────────────────────────────────── */

static const Color DEFAULT_FG = {200, 200, 200};
static const Color DEFAULT_BG = {  0,   0,   0};


/* ── terminal_create ────────────────────────────────────────── */

Terminal *terminal_create(int cols, int rows) {
    Terminal *t = calloc(1, sizeof(Terminal));
    if (!t) {
        fprintf(stderr, "terminal_create: out of memory\n");
        return NULL;
    }

    t->cols          = cols;
    t->rows          = rows;
    t->current_fg    = DEFAULT_FG;
    t->current_bg    = DEFAULT_BG;
    t->state         = STATE_NORMAL;
    t->scroll_offset = 0;
    t->sb_head       = 0;
    t->sb_count      = 0;

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
        for (int r = 0; r < t->rows; r++)
            free(t->cells[r]);
        free(t->cells);
    }

    for (int i = 0; i < SCROLLBACK_MAX; i++) {
        if (t->scrollback[i].cells)
            free(t->scrollback[i].cells);
    }

    free(t);
}


/* ── Internal helpers ───────────────────────────────────────── */

static void mark_all_dirty(Terminal *t) {
    for (int r = 0; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++)
            t->cells[r][c].dirty = 1;
}

/*
 * scrollback_push — save a copy of one row into the ring buffer.
 *
 * Called every time a line scrolls off the top of the visible
 * screen. We deep-copy the Cell array so colors are preserved.
 *
 * Ring buffer:
 *   sb_head  → next slot to write (wraps at SCROLLBACK_MAX)
 *   sb_count → total lines stored (capped at SCROLLBACK_MAX)
 */
static void scrollback_push(Terminal *t, Cell *row) {
    ScrollbackLine *slot = &t->scrollback[t->sb_head];

    if (slot->cells) {
        free(slot->cells);
        slot->cells = NULL;
    }

    slot->cells = malloc(t->cols * sizeof(Cell));
    if (!slot->cells) return;

    memcpy(slot->cells, row, t->cols * sizeof(Cell));
    slot->cols = t->cols;

    t->sb_head = (t->sb_head + 1) % SCROLLBACK_MAX;
    if (t->sb_count < SCROLLBACK_MAX) t->sb_count++;
}

/*
 * scroll_up — shift all rows up by one, push top into scrollback.
 *
 * We rotate the row pointer array rather than copying cell data.
 * The old top pointer is reused as the new blank bottom row.
 */
static void scroll_up(Terminal *t) {
    scrollback_push(t, t->cells[0]);

    Cell *top_row = t->cells[0];
    memmove(&t->cells[0], &t->cells[1],
            (t->rows - 1) * sizeof(Cell *));

    t->cells[t->rows - 1] = top_row;
    for (int c = 0; c < t->cols; c++) {
        t->cells[t->rows - 1][c].ch    = ' ';
        t->cells[t->rows - 1][c].fg    = DEFAULT_FG;
        t->cells[t->rows - 1][c].bg    = DEFAULT_BG;
        t->cells[t->rows - 1][c].bold  = 0;
        t->cells[t->rows - 1][c].dirty = 1;
    }

    /* Keep the user's scrolled view stable as new lines arrive */
    if (t->scroll_offset > 0) {
        t->scroll_offset++;
        if (t->scroll_offset > t->sb_count)
            t->scroll_offset = t->sb_count;
    }
}

/*
 * put_char — write one character at cursor and advance it.
 */
static void put_char(Terminal *t, char ch) {
    if (t->cursor_col >= t->cols) {
        t->cursor_col = 0;
        t->cursor_row++;
    }
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

    if (*count == 0) {
        out[0] = 0;
        *count  = 1;
    }
}


/* ── SGR — Select Graphic Rendition ────────────────────────── */

static void apply_sgr(Terminal *t, int *params, int count) {
    for (int i = 0; i < count; i++) {
        int p = params[i];

        if (p == 0) {
            t->current_fg = DEFAULT_FG;
            t->current_bg = DEFAULT_BG;
            t->bold       = 0;

        } else if (p == 1) {
            t->bold = 1;

        } else if (p == 2 || p == 22) {
            t->bold = 0;

        } else if (p >= 30 && p <= 37) {
            int idx = (p - 30) + (t->bold ? 8 : 0);
            t->current_fg = ANSI_COLORS[idx];

        } else if (p == 39) {
            t->current_fg = DEFAULT_FG;

        } else if (p >= 40 && p <= 47) {
            t->current_bg = ANSI_COLORS[p - 40];

        } else if (p == 49) {
            t->current_bg = DEFAULT_BG;

        } else if (p >= 90 && p <= 97) {
            t->current_fg = ANSI_COLORS[(p - 90) + 8];

        } else if (p >= 100 && p <= 107) {
            t->current_bg = ANSI_COLORS[(p - 100) + 8];

        } else if (p == 38) {
            if (i + 2 < count && params[i + 1] == 5) {
                int n = params[i + 2];
                if (n >= 0 && n < 16)
                    t->current_fg = ANSI_COLORS[n];
                i += 2;
            } else if (i + 4 < count && params[i + 1] == 2) {
                t->current_fg.r = (uint8_t)params[i + 2];
                t->current_fg.g = (uint8_t)params[i + 3];
                t->current_fg.b = (uint8_t)params[i + 4];
                i += 4;
            }

        } else if (p == 48) {
            if (i + 2 < count && params[i + 1] == 5) {
                int n = params[i + 2];
                if (n >= 0 && n < 16)
                    t->current_bg = ANSI_COLORS[n];
                i += 2;
            } else if (i + 4 < count && params[i + 1] == 2) {
                t->current_bg.r = (uint8_t)params[i + 2];
                t->current_bg.g = (uint8_t)params[i + 3];
                t->current_bg.b = (uint8_t)params[i + 4];
                i += 4;
            }
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

    switch (final) {

        case 'm':
            apply_sgr(t, params, count);
            break;

        case 'A': {
            int n = (p0 < 1) ? 1 : p0;
            t->cursor_row -= n;
            if (t->cursor_row < 0) t->cursor_row = 0;
            break;
        }
        case 'B': {
            int n = (p0 < 1) ? 1 : p0;
            t->cursor_row += n;
            if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
            break;
        }
        case 'C': {
            int n = (p0 < 1) ? 1 : p0;
            t->cursor_col += n;
            if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
            break;
        }
        case 'D': {
            int n = (p0 < 1) ? 1 : p0;
            t->cursor_col -= n;
            if (t->cursor_col < 0) t->cursor_col = 0;
            break;
        }
        case 'E': {
            int n = (p0 < 1) ? 1 : p0;
            t->cursor_row += n;
            t->cursor_col  = 0;
            if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
            break;
        }
        case 'F': {
            int n = (p0 < 1) ? 1 : p0;
            t->cursor_row -= n;
            t->cursor_col  = 0;
            if (t->cursor_row < 0) t->cursor_row = 0;
            break;
        }
        case 'G': {
            int col = (p0 < 1) ? 1 : p0;
            t->cursor_col = col - 1;
            if (t->cursor_col < 0)        t->cursor_col = 0;
            if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
            break;
        }
        case 'H':
        case 'f': {
            int row = (p0 < 1) ? 1 : p0;
            int col = (p1 < 1) ? 1 : p1;
            t->cursor_row = row - 1;
            t->cursor_col = col - 1;
            if (t->cursor_row < 0)        t->cursor_row = 0;
            if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
            if (t->cursor_col < 0)        t->cursor_col = 0;
            if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
            break;
        }
        case 'J': {
            if (p0 == 0) {
                for (int c = t->cursor_col; c < t->cols; c++) {
                    t->cells[t->cursor_row][c].ch    = ' ';
                    t->cells[t->cursor_row][c].dirty = 1;
                }
                for (int r = t->cursor_row + 1; r < t->rows; r++)
                    for (int c = 0; c < t->cols; c++) {
                        t->cells[r][c].ch    = ' ';
                        t->cells[r][c].dirty = 1;
                    }
            } else if (p0 == 1) {
                for (int r = 0; r < t->cursor_row; r++)
                    for (int c = 0; c < t->cols; c++) {
                        t->cells[r][c].ch    = ' ';
                        t->cells[r][c].dirty = 1;
                    }
                for (int c = 0; c <= t->cursor_col; c++) {
                    t->cells[t->cursor_row][c].ch    = ' ';
                    t->cells[t->cursor_row][c].dirty = 1;
                }
            } else if (p0 == 2 || p0 == 3) {
                for (int r = 0; r < t->rows; r++)
                    for (int c = 0; c < t->cols; c++) {
                        t->cells[r][c].ch    = ' ';
                        t->cells[r][c].dirty = 1;
                    }
                t->cursor_row = 0;
                t->cursor_col = 0;
            }
            mark_all_dirty(t);
            break;
        }
        case 'K': {
            if (p0 == 0) {
                for (int c = t->cursor_col; c < t->cols; c++) {
                    t->cells[t->cursor_row][c].ch    = ' ';
                    t->cells[t->cursor_row][c].dirty = 1;
                }
            } else if (p0 == 1) {
                for (int c = 0; c <= t->cursor_col; c++) {
                    t->cells[t->cursor_row][c].ch    = ' ';
                    t->cells[t->cursor_row][c].dirty = 1;
                }
            } else if (p0 == 2) {
                for (int c = 0; c < t->cols; c++) {
                    t->cells[t->cursor_row][c].ch    = ' ';
                    t->cells[t->cursor_row][c].dirty = 1;
                }
            }
            break;
        }
        case 'L': {
            int n = (p0 < 1) ? 1 : p0;
            for (int i = 0; i < n; i++) {
                Cell *bottom = t->cells[t->rows - 1];
                memmove(&t->cells[t->cursor_row + 1],
                        &t->cells[t->cursor_row],
                        (t->rows - t->cursor_row - 1) * sizeof(Cell *));
                t->cells[t->cursor_row] = bottom;
                for (int c = 0; c < t->cols; c++) {
                    t->cells[t->cursor_row][c].ch    = ' ';
                    t->cells[t->cursor_row][c].dirty = 1;
                }
            }
            mark_all_dirty(t);
            break;
        }
        case 'M': {
            int n = (p0 < 1) ? 1 : p0;
            for (int i = 0; i < n; i++) {
                Cell *top = t->cells[t->cursor_row];
                memmove(&t->cells[t->cursor_row],
                        &t->cells[t->cursor_row + 1],
                        (t->rows - t->cursor_row - 1) * sizeof(Cell *));
                t->cells[t->rows - 1] = top;
                for (int c = 0; c < t->cols; c++) {
                    t->cells[t->rows - 1][c].ch    = ' ';
                    t->cells[t->rows - 1][c].dirty = 1;
                }
            }
            mark_all_dirty(t);
            break;
        }
        case 'X': {
            int n = (p0 < 1) ? 1 : p0;
            for (int c = t->cursor_col;
                 c < t->cursor_col + n && c < t->cols; c++) {
                t->cells[t->cursor_row][c].ch    = ' ';
                t->cells[t->cursor_row][c].dirty = 1;
            }
            break;
        }
        case 'h':
        case 'l':
        case 'r':
        case 'n':
        case 'c':
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

            case STATE_NORMAL:
                if (c == 0x1b) {
                    t->state = STATE_ESCAPE;

                } else if (c == '\r') {
                    t->cursor_col = 0;

                } else if (c == '\n') {
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

                } else if (c == 0x07 || c == 0x0e || c == 0x0f) {
                    /* BEL, SO, SI — ignore */

                } else if (c >= 32 && c < 127) {
                    put_char(t, (char)c);
                }
                break;

            case STATE_ESCAPE:
                if (c == '[') {
                    t->state      = STATE_CSI;
                    t->params_len = 0;
                    memset(t->params, 0, sizeof(t->params));

                } else if (c == 'c') {
                    for (int r = 0; r < t->rows; r++)
                        for (int col = 0; col < t->cols; col++) {
                            t->cells[r][col].ch    = ' ';
                            t->cells[r][col].fg    = DEFAULT_FG;
                            t->cells[r][col].bg    = DEFAULT_BG;
                            t->cells[r][col].dirty = 1;
                        }
                    t->cursor_row = 0;
                    t->cursor_col = 0;
                    t->current_fg = DEFAULT_FG;
                    t->current_bg = DEFAULT_BG;
                    t->bold       = 0;
                    t->state      = STATE_NORMAL;

                } else if (c == 'M') {
                    if (t->cursor_row > 0) t->cursor_row--;
                    t->state = STATE_NORMAL;

                } else {
                    t->state = STATE_NORMAL;
                }
                break;

            case STATE_CSI:
                if ((c >= '0' && c <= '9') || c == ';' || c == '?'
                        || c == '!' || c == '"' || c == '$') {
                    if (t->params_len < (int)sizeof(t->params) - 1) {
                        t->params[t->params_len++] = (char)c;
                        t->params[t->params_len]   = '\0';
                    }
                } else if (c >= 0x40 && c <= 0x7e) {
                    apply_csi(t, (char)c);
                    t->state = STATE_NORMAL;
                } else {
                    t->state = STATE_NORMAL;
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

    if (t->cursor_row >= rows) t->cursor_row = rows - 1;
    if (t->cursor_col >= cols) t->cursor_col = cols - 1;
    if (t->cursor_row < 0)    t->cursor_row = 0;
    if (t->cursor_col < 0)    t->cursor_col = 0;
}


/* ── Scrollback public API ──────────────────────────────────── */

/*
 * scrollback_get — retrieve a stored line by recency index.
 *
 * index 0 = most recently pushed (newest history line).
 * index sb_count-1 = oldest stored line.
 *
 * sb_head points to the NEXT write slot, so most recent
 * entry is at (sb_head - 1) with wraparound.
 */
ScrollbackLine *scrollback_get(Terminal *t, int index) {
    if (index < 0 || index >= t->sb_count) return NULL;
    int pos = (t->sb_head - 1 - index + SCROLLBACK_MAX) % SCROLLBACK_MAX;
    return &t->scrollback[pos];
}

/*
 * terminal_get_display_row — resolve which Cell array to render.
 *
 * MENTAL MODEL — think of one long continuous tape:
 *
 *  index:  0        1  ...  sb_count-1 | sb_count  ...  sb_count+rows-1
 *          ^oldest history             ^           live screen          ^
 *                                      |
 *                              boundary between
 *                              scrollback and live
 *
 * scroll_offset=0 → viewport shows [sb_count .. sb_count+rows-1]  (live)
 * scroll_offset=N → viewport shows [sb_count-N .. sb_count-N+rows-1]
 *
 * For each screen_row (0=top of window):
 *
 *   total      = sb_count + rows
 *   view_bottom = total - scroll_offset
 *   view_top    = view_bottom - rows
 *   line_index  = view_top + screen_row
 *
 *   line_index < sb_count  → scrollback
 *     sb_index = (sb_count - 1) - line_index   (newest=0 mapping)
 *
 *   line_index >= sb_count → live screen
 *     live_row = line_index - sb_count
 */
Cell *terminal_get_display_row(Terminal *t, int screen_row) {

    /* Fast path — live view, no math needed */
    if (t->scroll_offset == 0) {
        return t->cells[screen_row];
    }

    int total       = t->sb_count + t->rows;
    int view_bottom = total - t->scroll_offset;
    int view_top    = view_bottom - t->rows;
    int line_index  = view_top + screen_row;

    /* Completely out of range — render blank */
    if (line_index < 0 || line_index >= total) {
        return NULL;
    }

    if (line_index < t->sb_count) {
        /*
         * In scrollback history.
         * line_index 0 = oldest stored line → sb_index = sb_count-1
         * line_index sb_count-1 = newest  → sb_index = 0
         */
        int sb_index = (t->sb_count - 1) - line_index;
        ScrollbackLine *sl = scrollback_get(t, sb_index);
        return sl ? sl->cells : NULL;
    } else {
        /* In live screen */
        int live_row = line_index - t->sb_count;
        if (live_row >= 0 && live_row < t->rows)
            return t->cells[live_row];
        return NULL;
    }
}