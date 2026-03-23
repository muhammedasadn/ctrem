/*
 * pane.c — Recursive split-pane tree implementation.
 *
 * The pane tree works like this:
 *
 *   Single pane:           After Ctrl+Shift+Right (H split):
 *
 *   [LEAF A]               [SPLIT_H]
 *                          /        \
 *                     [LEAF A]   [LEAF B]
 *
 *   After splitting LEAF B vertically:
 *
 *   [SPLIT_H]
 *   /        \
 * [LEAF A]  [SPLIT_V]
 *           /        \
 *        [LEAF B]  [LEAF C]
 */

#define _GNU_SOURCE
#include "pane.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ── pane_create_leaf ───────────────────────────────────────── */

Pane *pane_create_leaf(int cols, int rows) {
    Pane *p = calloc(1, sizeof(Pane));
    if (!p) return NULL;

    p->type    = PANE_LEAF;
    p->ratio   = 0.5f;
    p->focused = 0;
    p->first   = NULL;
    p->second  = NULL;

    p->term = terminal_create(cols, rows);
    if (!p->term) {
        free(p);
        return NULL;
    }

    if (pty_init(&p->pty, cols, rows) != 0) {
        terminal_destroy(p->term);
        free(p);
        return NULL;
    }

    return p;
}


/* ── pane_split ─────────────────────────────────────────────── */

/*
 * Splits an existing LEAF pane into a SPLIT node.
 *
 * Before:  caller holds pointer → [LEAF A]
 * After:   caller holds pointer → [SPLIT]
 *                                 /      \
 *                            [LEAF A]  [LEAF B (new)]
 *
 * We allocate a new split node, move the existing leaf into
 * split->first, and create a fresh leaf as split->second.
 *
 * The caller MUST replace their pointer to the old leaf with
 * the returned split node pointer.
 */
Pane *pane_split(Pane *leaf, PaneType type, int cols, int rows) {
    if (!leaf || leaf->type != PANE_LEAF) return leaf;

    /* Create the new split node */
    Pane *split = calloc(1, sizeof(Pane));
    if (!split) return leaf;

    split->type   = type;
    split->ratio  = 0.5f;
    split->first  = leaf;
    split->focused = 0;

    /* Create the new leaf for the second half */
    split->second = pane_create_leaf(cols, rows);
    if (!split->second) {
        free(split);
        return leaf;
    }

    /* Focus moves to the new pane */
    leaf->focused         = 0;
    split->second->focused = 1;

    printf("Pane split: type=%s\n",
           type == PANE_SPLIT_H ? "horizontal" : "vertical");
    return split;
}


/* ── pane_layout ────────────────────────────────────────────── */

/*
 * Recursively computes the pixel rect for each pane.
 *
 * For a LEAF: rect = the passed-in area. Done.
 *
 * For a SPLIT_H (left|right):
 *   first_area.w  = area.w * ratio  - DIVIDER_SIZE/2
 *   second_area.x = area.x + first_area.w + DIVIDER_SIZE
 *   second_area.w = area.w - first_area.w - DIVIDER_SIZE
 *
 * For a SPLIT_V (top/bottom): same but on height.
 *
 * After computing the child rects, we recurse.
 * After this function, every leaf has a valid rect
 * and knows exactly where to render its terminal grid.
 */
void pane_layout(Pane *p, SDL_Rect area) {
    if (!p) return;

    p->rect = area;  /* Store our own area */

    if (p->type == PANE_LEAF) return;

    SDL_Rect first_rect  = area;
    SDL_Rect second_rect = area;

    if (p->type == PANE_SPLIT_H) {
        /* Horizontal: split width */
        int first_w = (int)(area.w * p->ratio) - DIVIDER_SIZE / 2;
        if (first_w < 10) first_w = 10;
        if (first_w > area.w - DIVIDER_SIZE - 10)
            first_w = area.w - DIVIDER_SIZE - 10;

        first_rect.w  = first_w;
        second_rect.x = area.x + first_w + DIVIDER_SIZE;
        second_rect.w = area.w - first_w - DIVIDER_SIZE;

    } else { /* PANE_SPLIT_V */
        /* Vertical: split height */
        int first_h = (int)(area.h * p->ratio) - DIVIDER_SIZE / 2;
        if (first_h < 10) first_h = 10;
        if (first_h > area.h - DIVIDER_SIZE - 10)
            first_h = area.h - DIVIDER_SIZE - 10;

        first_rect.h  = first_h;
        second_rect.y = area.y + first_h + DIVIDER_SIZE;
        second_rect.h = area.h - first_h - DIVIDER_SIZE;
    }

    /* Recurse into children */
    pane_layout(p->first,  first_rect);
    pane_layout(p->second, second_rect);
}


/* ── pane_get_focused ───────────────────────────────────────── */

/*
 * Walk the tree and return the focused leaf.
 * Returns the first leaf found if none is marked focused.
 */
Pane *pane_get_focused(Pane *p) {
    if (!p) return NULL;

    if (p->type == PANE_LEAF) {
        return p;  /* Return this leaf regardless of focus flag */
    }

    /* Check first subtree */
    Pane *f = pane_get_focused(p->first);
    if (f && f->focused) return f;

    /* Check second subtree */
    Pane *s = pane_get_focused(p->second);
    if (s && s->focused) return s;

    /* No focused pane found — return first leaf */
    return f ? f : s;
}


/* ── pane_collect_leaves ────────────────────────────────────── */

/* Helper: collect all leaf pointers into an array */
static int collect_leaves(Pane *p, Pane **out, int max) {
    if (!p) return 0;
    if (p->type == PANE_LEAF) {
        if (max > 0) { out[0] = p; return 1; }
        return 0;
    }
    int n  = collect_leaves(p->first,  out,       max);
    int m  = collect_leaves(p->second, out + n, max - n);
    return n + m;
}


/* ── pane_focus_next ────────────────────────────────────────── */

/*
 * Cycles focus to the next leaf in depth-first order.
 * Wraps from last back to first.
 */
void pane_focus_next(Pane *root) {
    Pane *leaves[64];
    int   count = collect_leaves(root, leaves, 64);
    if (count <= 1) return;

    /* Find which one is focused */
    int focused_idx = 0;
    for (int i = 0; i < count; i++) {
        if (leaves[i]->focused) {
            focused_idx = i;
            break;
        }
    }

    /* Unfocus current, focus next (with wraparound) */
    leaves[focused_idx]->focused = 0;
    leaves[(focused_idx + 1) % count]->focused = 1;
}


/* ── pane_find_at ───────────────────────────────────────────── */

/*
 * Returns the leaf pane whose rect contains point (x, y).
 * Used to focus a pane when the user clicks on it.
 */
Pane *pane_find_at(Pane *p, int x, int y) {
    if (!p) return NULL;

    /* Check if point is inside this pane's rect */
    if (x < p->rect.x || x >= p->rect.x + p->rect.w) return NULL;
    if (y < p->rect.y || y >= p->rect.y + p->rect.h) return NULL;

    if (p->type == PANE_LEAF) return p;

    /* Recurse into children */
    Pane *found = pane_find_at(p->first, x, y);
    if (found) return found;
    return pane_find_at(p->second, x, y);
}


/* ── pane_set_focus ─────────────────────────────────────────── */

/* Helper: clear focus on all leaves */
static void clear_all_focus(Pane *p) {
    if (!p) return;
    if (p->type == PANE_LEAF) { p->focused = 0; return; }
    clear_all_focus(p->first);
    clear_all_focus(p->second);
}

/* Set focus to a specific leaf */
void pane_set_focus(Pane *root, Pane *target) {
    clear_all_focus(root);
    if (target && target->type == PANE_LEAF) {
        target->focused = 1;
    }
}


/* ── pane_close_focused ─────────────────────────────────────── */

/*
 * Close the focused leaf pane.
 *
 * When a leaf is closed, its parent split node collapses —
 * the sibling takes over the split node's position in the tree.
 *
 * We use a recursive approach:
 *   - If root itself is the focused leaf → destroy and return NULL
 *   - If a child is the focused leaf     → replace split node
 *     with the other child
 *   - Otherwise recurse deeper
 *
 * The caller must update their root pointer to the return value.
 */
Pane *pane_close_focused(Pane *p) {
    if (!p) return NULL;

    if (p->type == PANE_LEAF) {
        if (p->focused) {
            pane_destroy(p);
            return NULL;
        }
        return p;
    }

    /* Check if first child is a focused leaf */
    if (p->first && p->first->type == PANE_LEAF
            && p->first->focused) {
        Pane *survivor = p->second;
        p->first->focused = 0;
        pane_destroy(p->first);
        free(p);  /* free the split node itself */
        /* Give focus to the survivor's first leaf */
        Pane *leaves[64];
        int n = collect_leaves(survivor, leaves, 64);
        if (n > 0) leaves[0]->focused = 1;
        return survivor;
    }

    /* Check if second child is a focused leaf */
    if (p->second && p->second->type == PANE_LEAF
            && p->second->focused) {
        Pane *survivor = p->first;
        p->second->focused = 0;
        pane_destroy(p->second);
        free(p);
        Pane *leaves[64];
        int n = collect_leaves(survivor, leaves, 64);
        if (n > 0) leaves[0]->focused = 1;
        return survivor;
    }

    /* Focused pane is deeper — recurse */
    p->first  = pane_close_focused(p->first);
    p->second = pane_close_focused(p->second);
    return p;
}


/* ── pane_draw_dividers ─────────────────────────────────────── */

/*
 * Draws the divider bars between split panes.
 * Focused pane gets a highlighted border.
 */
void pane_draw_dividers(Pane *p, SDL_Renderer *renderer) {
    if (!p || p->type == PANE_LEAF) {
        /* Draw focus highlight border on focused leaf */
        if (p && p->focused) {
            SDL_SetRenderDrawColor(renderer, 80, 140, 255, 180);
            SDL_RenderDrawRect(renderer, &p->rect);
        }
        return;
    }

    /* Draw the divider bar */
    SDL_Rect divider;
    if (p->type == PANE_SPLIT_H) {
        divider.x = p->first->rect.x + p->first->rect.w;
        divider.y = p->rect.y;
        divider.w = DIVIDER_SIZE;
        divider.h = p->rect.h;
    } else {
        divider.x = p->rect.x;
        divider.y = p->first->rect.y + p->first->rect.h;
        divider.w = p->rect.w;
        divider.h = DIVIDER_SIZE;
    }

    SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255);
    SDL_RenderFillRect(renderer, &divider);

    /* Recurse */
    pane_draw_dividers(p->first,  renderer);
    pane_draw_dividers(p->second, renderer);
}


/* ── pane_read_all ──────────────────────────────────────────── */

/*
 * Read PTY output for every leaf in the tree.
 * Background panes keep getting updated even when not focused.
 */
void pane_read_all(Pane *p, char *buf, int bufsize) {
    if (!p) return;

    if (p->type == PANE_LEAF) {
        int n = pty_read(&p->pty, buf, bufsize);
        if (n > 0) {
            terminal_process(p->term, buf, n);
        }
        return;
    }

    pane_read_all(p->first,  buf, bufsize);
    pane_read_all(p->second, buf, bufsize);
}


/* ── pane_destroy ───────────────────────────────────────────── */

/*
 * Recursively free the pane tree.
 * Post-order: free children before parent.
 */
void pane_destroy(Pane *p) {
    if (!p) return;

    if (p->type == PANE_LEAF) {
        pty_destroy(&p->pty);
        terminal_destroy(p->term);
        free(p);
        return;
    }

    pane_destroy(p->first);
    pane_destroy(p->second);
    free(p);
}