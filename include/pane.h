#ifndef PANE_H
#define PANE_H

/*
 * pane.h — Split pane tree for cterm.
 *
 * A tab's screen area is divided into panes using a binary tree.
 * Each leaf node holds one Terminal + PTY (one shell session).
 * Each split node divides its area between two child panes.
 *
 * The tree is built by splitting leaf nodes. Closing a leaf
 * collapses its parent split and the sibling takes the space.
 */

#include "ansi.h"   /* Terminal, Cell */
#include "pty.h"    /* PTY            */
#include <SDL2/SDL.h>

/* Width of the divider bar between split panes in pixels */
#define DIVIDER_SIZE 4


/* ── Pane type ──────────────────────────────────────────────── */

typedef enum {
    PANE_LEAF,       /* Terminal + PTY live here, no children   */
    PANE_SPLIT_H,    /* Horizontal split: left  | right         */
    PANE_SPLIT_V     /* Vertical   split: top   / bottom        */
} PaneType;


/* ── Pane node ──────────────────────────────────────────────── */

/*
 * Pane — one node in the pane tree.
 *
 * PANE_LEAF:
 *   term    — the terminal grid and ANSI parser
 *   pty     — the shell process
 *   first   — NULL
 *   second  — NULL
 *   focused — 1 if this pane receives keyboard input
 *
 * PANE_SPLIT_H / PANE_SPLIT_V:
 *   first   — left child  (or top)
 *   second  — right child (or bottom)
 *   ratio   — fraction of space given to 'first' (0.1 – 0.9)
 *   term    — unused
 *   focused — unused (focus lives on leaves only)
 *
 * rect — pixel bounds computed each frame by pane_layout().
 *        Read this in the render loop to know where to draw.
 */
typedef struct Pane {
    PaneType       type;

    /* Leaf fields */
    Terminal      *term;
    PTY            pty;

    /* Split fields */
    struct Pane   *first;
    struct Pane   *second;
    float          ratio;     /* fraction of space for 'first' */

    /* Layout result — filled by pane_layout() */
    SDL_Rect       rect;      /* pixel bounds of this pane      */

    /* Focus flag (leaf only) */
    int            focused;   /* 1 = this leaf receives input   */
} Pane;


/* ── Public API ─────────────────────────────────────────────── */

/*
 * pane_create_leaf — allocate a new leaf with Terminal + PTY.
 * cols/rows = initial grid dimensions.
 * Returns heap pointer; caller must eventually call pane_destroy().
 */
Pane *pane_create_leaf(int cols, int rows);

/*
 * pane_split — split a leaf into a split node with two leaves.
 *
 * The original leaf becomes 'first'. A new leaf is 'second'.
 * type = PANE_SPLIT_H (left|right) or PANE_SPLIT_V (top/bottom).
 *
 * Returns the new split node. The caller MUST replace their
 * pointer to the old leaf with the returned pointer:
 *
 *   tab->root = pane_split(tab->root, PANE_SPLIT_H, cols, rows);
 */
Pane *pane_split(Pane *leaf, PaneType type, int cols, int rows);

/*
 * pane_layout — recursively compute pixel rects for all nodes.
 * Call once per frame with the total available screen area.
 * After this call, every leaf's rect field is valid.
 */
void pane_layout(Pane *p, SDL_Rect area);

/*
 * pane_get_focused — return the currently focused leaf.
 * If no leaf has focused=1, returns the first leaf found.
 * Never returns NULL as long as the tree has at least one leaf.
 */
Pane *pane_get_focused(Pane *p);

/*
 * pane_set_focus — clear focus on all leaves, then set focus
 * on 'target'. target must be a PANE_LEAF.
 * root = the root of the whole pane tree for this tab.
 */
void pane_set_focus(Pane *root, Pane *target);

/*
 * pane_focus_next — cycle focus to the next leaf in depth-first
 * order, wrapping from the last leaf back to the first.
 */
void pane_focus_next(Pane *root);

/*
 * pane_close_focused — destroy the focused leaf and collapse
 * its parent split node (sibling takes over the parent's space).
 *
 * Returns the new root of the tree (may differ from old root
 * when the focused leaf was the first or second child of root).
 * The caller must update their root pointer:
 *
 *   tab->root = pane_close_focused(tab->root);
 */
Pane *pane_close_focused(Pane *root);

/*
 * pane_find_at — return the leaf pane whose rect contains (x,y).
 * Used to switch focus when the user clicks inside the terminal
 * area. Returns NULL if no leaf contains the point.
 */
Pane *pane_find_at(Pane *p, int x, int y);

/*
 * pane_draw_dividers — draw the divider bars between split panes
 * and a highlight border around the focused leaf.
 * Call after rendering all leaf terminals each frame.
 */
void pane_draw_dividers(Pane *p, SDL_Renderer *renderer);

/*
 * pane_read_all — read PTY output for every leaf in the tree
 * and feed it into that leaf's terminal parser.
 * Call once per frame for every tab (not just the active one).
 */
void pane_read_all(Pane *p, char *buf, int bufsize);

/*
 * pane_destroy — recursively free the entire pane tree.
 * Kills each leaf's PTY process and frees its Terminal.
 * Always call this before removing a tab.
 */
void pane_destroy(Pane *p);


#endif /* PANE_H */