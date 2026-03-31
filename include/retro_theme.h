#ifndef RETRO_THEME_H
#define RETRO_THEME_H

/*
 * retro_theme.h — GRiD OS inspired color palette.
 *
 * Reference: GRiD Systems OS (1987) screenshot.
 * Colors shift from yellow (warm) to green (phosphor CRT).
 *
 * Usage:  SDL_SetRenderDrawColor(r, RT_BG_R, RT_BG_G, RT_BG_B, 255);
 */

/* ── Background — near-black with faint green tint ── */
#define RT_BG_R     6
#define RT_BG_G     10
#define RT_BG_B     6

/* ── Primary text — warm yellow-green (phosphor) ── */
#define RT_TEXT_R   195
#define RT_TEXT_G   210
#define RT_TEXT_B   55

/* ── Dim text — darker olive, for inactive/secondary ── */
#define RT_DIM_R    90
#define RT_DIM_G    105
#define RT_DIM_B    25

/* ── Bright green — selected row background ── */
#define RT_SEL_BG_R   28
#define RT_SEL_BG_G   160
#define RT_SEL_BG_B   28

/* ── Selected row text — black on bright green ── */
#define RT_SEL_FG_R   0
#define RT_SEL_FG_G   0
#define RT_SEL_FG_B   0

/* ── Border / grid lines — mid green ── */
#define RT_BORDER_R  55
#define RT_BORDER_G  140
#define RT_BORDER_B  55

/* ── Status panel background — slightly lighter than BG ── */
#define RT_PANEL_R   10
#define RT_PANEL_G   16
#define RT_PANEL_B   10

/* ── Active tab / header accent ── */
#define RT_ACCENT_R  160
#define RT_ACCENT_G  220
#define RT_ACCENT_B  60

/* ── Input box (like "GRID 80" field in the screenshot) ── */
#define RT_INPUT_BG_R   20
#define RT_INPUT_BG_G   80
#define RT_INPUT_BG_B   20

/* ── Cursor color ── */
#define RT_CURSOR_R  200
#define RT_CURSOR_G  220
#define RT_CURSOR_B  80

/* ── Header line (date/time bar) ── */
#define RT_HEADER_R  14
#define RT_HEADER_G  22
#define RT_HEADER_B  14

/* ── Warning / alert — amber yellow ── */
#define RT_WARN_R   230
#define RT_WARN_G   180
#define RT_WARN_B    30

#endif /* RETRO_THEME_H */