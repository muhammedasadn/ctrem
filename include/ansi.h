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
typedef struct {
    char  ch;
    Color fg;
    Color bg;
    int   bold;
    int   dirty;
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
    STATE_OSC       /* Operating System Command — e.g. title set */
} ParserState;

/* ── Terminal ───────────────────────────────────────────────── */
typedef struct {
    Cell         **cells;
    int            cols;
    int            rows;
    int            cursor_col;
    int            cursor_row;

    /* Saved cursor (DECSC/DECRC) */
    int            saved_col;
    int            saved_row;

    Color          current_fg;
    Color          current_bg;
    int            bold;

    /* Parser */
    ParserState    state;
    char           params[128];
    int            params_len;

    /* OSC accumulator */
    char           osc_buf[256];
    int            osc_len;

    /* Scrollback */
    ScrollbackLine scrollback[SCROLLBACK_MAX];
    int            sb_head;
    int            sb_count;
    int            scroll_offset;

    /* Alternate screen — simple flag for now */
    int            alt_screen;
} Terminal;

/* ── Public API ─────────────────────────────────────────────── */
Terminal       *terminal_create(int cols, int rows);
void            terminal_destroy(Terminal *t);
void            terminal_process(Terminal *t, const char *buf, int len);
void            terminal_resize(Terminal *t, int cols, int rows);
ScrollbackLine *scrollback_get(Terminal *t, int index);
Cell           *terminal_get_display_row(Terminal *t, int screen_row);

#endif /* ANSI_H */