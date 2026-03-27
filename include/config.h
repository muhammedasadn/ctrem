#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h — User configuration for cterm.
 *
 * Reads ~/.config/cterm/cterm.conf at startup.
 * Every setting has a sensible default so the file is optional.
 *
 * Config file format:
 *   key = value
 *   # lines starting with # are comments
 *   blank lines are ignored
 *
 * Example ~/.config/cterm/cterm.conf:
 *   font_path     = /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
 *   font_size     = 16
 *   fg_color      = 200 200 200
 *   bg_color      = 18 18 18
 *   window_width  = 900
 *   window_height = 560
 *   scrollback    = 5000
 */

/* Maximum path length for font_path */
#define CONFIG_PATH_MAX 512

/*
 * Config — all user-configurable settings.
 * Populated by config_load() at startup.
 * Access via the global g_config.
 */
typedef struct {

    /* Font */
    char font_path[CONFIG_PATH_MAX]; /* path to .ttf file      */
    int  font_size;                  /* pixels, e.g. 16        */

    /* Colors — stored as R G B (0-255 each) */
    int  fg_r, fg_g, fg_b;          /* default text color     */
    int  bg_r, bg_g, bg_b;          /* background color       */
    int  cursor_r, cursor_g, cursor_b; /* cursor color         */

    /* Window */
    int  window_width;               /* initial width  pixels  */
    int  window_height;              /* initial height pixels  */
    int  start_fullscreen;           /* 1 = start fullscreen   */

    /* Terminal */
    int  scrollback_lines;           /* max scrollback history */
    int  font_antialiasing;          /* 1 = smooth (default)   */

} Config;

/*
 * Global config instance.
 * Declared here, defined in config.c.
 * All modules access settings via g_config.font_size etc.
 */
extern Config g_config;

/*
 * config_load — read the config file and populate g_config.
 * Sets all fields to defaults first, then applies the file.
 * Safe to call even if the file doesn't exist.
 *
 * Call once at startup before any other module is initialized.
 */
void config_load(void);

/*
 * config_save_default — write a default config file if none exists.
 * Creates ~/.config/cterm/ directory if needed.
 * Call after config_load() so the user gets a template to edit.
 */
void config_save_default(void);

/*
 * config_print — dump current settings to stdout for debugging.
 */
void config_print(void);

#endif /* CONFIG_H */