#ifndef ANSI_H
#define ANSI_H

/*
 * ansi.h — Terminal emulation: cell grid, ANSI/VT100 parser,
 *           scrollback buffer.
 *
 * This header defines every data type and function used by
 * the terminal emulation layer. It is included by ansi.c
 * (implementation) and main.c (rendering + input).
 */

#include <stdint.h>
#include <stdlib.h>

/* ── Constants ──────────────────────────────────────────────── */

/* Number of ASCII codepoints we handle (0–127) */
#define GLYPH_COUNT     128

/* Maximum parameters inside one CSI escape e.g. \x1b[1;32m = 2 */
#define MAX_PARAMS      16

/* Maximum lines kept in scrollback ring buffer */
#define SCROLLBACK_MAX  5000


/* ── Color ──────────────────────────────────────────────────── */

/*
 * Color — 24-bit RGB.
 * Every cell on the grid has its own fg and bg color,
 * so colored text is preserved in the scrollback too.
 */
typedef struct {
    uint8_t r, g, b;
} Color;

/*
 * ANSI_COLORS — the 16 standard terminal palette entries.
 * Indices 0–7 are normal colors, 8–15 are bright variants.
 * Programs select them with SGR codes 30–37 (fg) / 40–47 (bg)
 * and 90–97 (bright fg) / 100–107 (bright bg).
 */
static const Color ANSI_COLORS[16] = {
    {  0,   0,   0},  /*  0 Black          */
    {170,   0,   0},  /*  1 Red            */
    {  0, 170,   0},  /*  2 Green          */
    {170, 170,   0},  /*  3 Yellow         */
    {  0,   0, 170},  /*  4 Blue           */
    {170,   0, 170},  /*  5 Magenta        */
    {  0, 170, 170},  /*  6 Cyan           */
    {170, 170, 170},  /*  7 White          */
    { 85,  85,  85},  /*  8 Bright Black   */
    {255,  85,  85},  /*  9 Bright Red     */
    { 85, 255,  85},  /* 10 Bright Green   */
    {255, 255,  85},  /* 11 Bright Yellow  */
    { 85,  85, 255},  /* 12 Bright Blue    */
    {255,  85, 255},  /* 13 Bright Magenta */
    { 85, 255, 255},  /* 14 Bright Cyan    */
    {255, 255, 255},  /* 15 Bright White   */
};


/* ── Cell ───────────────────────────────────────────────────── */

/*
 * Cell — one character position on the terminal grid.
 *
 * The entire visible screen is a 2D array of Cells:
 *   cells[row][col]
 *
 * Each Cell stores the character, its colors, and text
 * attributes independently. This means every cell can have
 * a different color, which is how ANSI color output works.
 *
 * dirty: set to 1 when the cell changes. A future optimization
 * can skip redrawing clean cells. Currently we redraw all.
 */
typedef struct {
    char  ch;       /* Character to display (printable ASCII)  */
    Color fg;       /* Foreground (text) color                 */
    Color bg;       /* Background color                        */
    int   bold;     /* 1 = bold / bright weight                */
    int   dirty;    /* 1 = changed since last frame            */
} Cell;


/* ── Scrollback ─────────────────────────────────────────────── */

/*
 * ScrollbackLine — one saved line of terminal history.
 *
 * When a line scrolls off the top of the visible screen,
 * we deep-copy it into the scrollback ring buffer as a
 * ScrollbackLine. We store the full Cell array (not just
 * characters) so colors are preserved when you scroll back.
 *
 * cols is stored per-line because the terminal may have been
 * resized between when the line was captured and now.
 */
typedef struct {
    Cell *cells;   /* Heap-allocated array of 'cols' Cell structs */
    int   cols;    /* Number of columns when this line was saved  */
} ScrollbackLine;


/* ── Parser state ───────────────────────────────────────────── */

/*
 * ParserState — the three states of the ANSI escape parser.
 *
 * The parser is a simple finite state machine (FSM).
 * It reads one byte at a time and transitions between states:
 *
 *   NORMAL  → regular character, write to grid
 *           → \x1b received → switch to ESCAPE
 *
 *   ESCAPE  → '[' received → switch to CSI
 *           → anything else → back to NORMAL
 *
 *   CSI     → digit/semicolon → accumulate parameter bytes
 *           → letter (A–Z, a–z) → apply command, back to NORMAL
 *           → unexpected byte → abort, back to NORMAL
 */
typedef enum {
    STATE_NORMAL,   /* Reading regular printable characters     */
    STATE_ESCAPE,   /* Just received ESC byte (0x1b)            */
    STATE_CSI       /* Inside a CSI sequence, collecting params */
} ParserState;


/* ── Terminal ───────────────────────────────────────────────── */

/*
 * Terminal — the complete state of one terminal session.
 *
 * This struct owns:
 *   - The visible cell grid (cells[rows][cols])
 *   - The current cursor position
 *   - The current drawing attributes (color, bold)
 *   - The ANSI parser's internal state
 *   - The scrollback ring buffer
 *   - The current scroll position (viewport offset)
 *
 * One Terminal corresponds to one PTY / one shell session.
 * When we add tabs in Module 7, each tab will have its own
 * Terminal instance.
 */
typedef struct {

    /* ── Visible grid ── */
    Cell **cells;       /* 2D array: cells[row][col]            */
    int    cols;        /* Current terminal width in characters  */
    int    rows;        /* Current terminal height in characters */

    /* ── Cursor ── */
    int    cursor_col;  /* 0-based column position              */
    int    cursor_row;  /* 0-based row position                 */

    /* ── Current drawing attributes ── */
    Color  current_fg;  /* Foreground color for new characters  */
    Color  current_bg;  /* Background color for new characters  */
    int    bold;        /* 1 if bold mode is active             */

    /* ── ANSI parser state ── */
    ParserState  state;         /* Current parser FSM state     */
    char         params[64];    /* Raw parameter string buffer  */
    int          params_len;    /* Bytes written into params[]  */

    /* ── Scrollback ring buffer ── */
    ScrollbackLine scrollback[SCROLLBACK_MAX]; /* fixed-size ring */
    int            sb_head;     /* Next write index (0-based)   */
    int            sb_count;    /* Lines stored so far          */

    /* ── Viewport ── */
    int            scroll_offset; /* 0=live bottom, N=up N lines */

} Terminal;


/* ── Public API ─────────────────────────────────────────────── */

/*
 * terminal_create — allocate and initialize a Terminal.
 * Returns a heap pointer; caller must call terminal_destroy().
 */
Terminal *terminal_create(int cols, int rows);

/*
 * terminal_destroy — free all memory owned by the Terminal.
 * Always call this before exiting or closing a tab.
 */
void terminal_destroy(Terminal *t);

/*
 * terminal_process — feed raw PTY bytes into the parser.
 * Call this every frame after pty_read() returns data.
 * The parser updates the cell grid and cursor in place.
 */
void terminal_process(Terminal *t, const char *buf, int len);

/*
 * terminal_resize — rebuild the grid at a new size.
 * Call this when the SDL2 window is resized.
 * Also call pty_resize() afterwards to notify the shell.
 */
void terminal_resize(Terminal *t, int cols, int rows);

/*
 * scrollback_get — retrieve a stored line by recency index.
 * index 0 = most recently scrolled-off line.
 * index 1 = one before that. Returns NULL if out of range.
 */
ScrollbackLine *scrollback_get(Terminal *t, int index);

/*
 * terminal_get_display_row — returns the Cell array to render
 * for a given screen row, accounting for scroll_offset.
 *
 * When scroll_offset == 0: returns live cells[row].
 * When scrolled: returns the appropriate historical line.
 * Returns NULL if the row has no content (render as blank).
 *
 * Use this in the render loop instead of accessing
 * term->cells[row] directly.
 */
Cell *terminal_get_display_row(Terminal *t, int screen_row);


#endif /* ANSI_H */