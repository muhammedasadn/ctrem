#ifndef ANSI_H
#define ANSI_H

#include <stdint.h>
#include <stdlib.h>

/* ── Constants ──────────────────────────────────────────────── */
#define GLYPH_COUNT    128
#define MAX_PARAMS     16
#define SCROLLBACK_MAX 5000

/* ── Color ──────────────────────────────────────────────────── */
typedef struct { uint8_t r, g, b; } Color;

static const Color ANSI_COLORS[16] = {
    {  0,   0,   0}, {170,   0,   0}, {  0, 170,   0}, {170, 170,   0},
    {  0,   0, 170}, {170,   0, 170}, {  0, 170, 170}, {170, 170, 170},
    { 85,  85,  85}, {255,  85,  85}, { 85, 255,  85}, {255, 255,  85},
    { 85,  85, 255}, {255,  85, 255}, { 85, 255, 255}, {255, 255, 255},
};

/* ── Cell ───────────────────────────────────────────────────── */
/*
 * Module 11 optimization: dirty flag.
 * dirty=1 means this cell changed since last frame.
 * The renderer skips cells where dirty=0 — no SDL draw calls,
 * no GPU work. On an idle terminal this saves ~95% of draw calls.
 *
 * After rendering a cell, set dirty=0.
 * The terminal parser sets dirty=1 whenever it modifies a cell.
 */
typedef struct {
    char  ch;
    Color fg;
    Color bg;
    int   bold;
    int   dirty;   /* 1 = changed, needs redraw this frame */
} Cell;

/* ── ScrollbackLine ─────────────────────────────────────────── */
typedef struct {
    Cell *cells;
    int   cols;
} ScrollbackLine;

/* ── ParserState ────────────────────────────────────────────── */
typedef enum {
    STATE_NORMAL,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_OSC    /* Operating System Command (title, integration) */
} ParserState;

/* ── Terminal ───────────────────────────────────────────────── */
typedef struct {
    Cell         **cells;
    int            cols;
    int            rows;
    int            cursor_col;
    int            cursor_row;
    int            saved_col;
    int            saved_row;
    Color          current_fg;
    Color          current_bg;
    int            bold;
    ParserState    state;
    char           params[128];
    int            params_len;
    char           osc_buf[256];
    int            osc_len;
    ScrollbackLine scrollback[SCROLLBACK_MAX];
    int            sb_head;
    int            sb_count;
    int            scroll_offset;
    int            alt_screen;

    /* Module 11: frame dirty flag
     * Set to 1 by terminal_process() when any cell changed.
     * The render loop uses this to skip full redraws when
     * the terminal content hasn't changed at all this frame.
     * Reset to 0 after each render pass. */
    int            frame_dirty;
} Terminal;

/* ── Public API ─────────────────────────────────────────────── */
Terminal       *terminal_create(int cols, int rows);
void            terminal_destroy(Terminal *t);
void            terminal_process(Terminal *t, const char *buf, int len);
void            terminal_resize(Terminal *t, int cols, int rows);
ScrollbackLine *scrollback_get(Terminal *t, int index);
Cell           *terminal_get_display_row(Terminal *t, int screen_row);

#endif /* ANSI_H */