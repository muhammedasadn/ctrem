/*
 * tools.c — Built-in tool launcher for cterm.
 *
 * Provides a Ctrl+P overlay that lets users search and launch
 * pre-registered tools (btop, vim, nmap, etc.) in a new tab.
 *
 * Architecture:
 *   ToolManager  — owns the tool registry and launcher state
 *   ToolDef      — one registered tool (name, command, args)
 *   ToolLauncher — overlay UI state (visible, search, selected)
 *
 * Tools run through the SAME PTY+ANSI pipeline as bash.
 * We open a new tab (which spawns bash), then send the tool
 * command to bash as if the user typed it. This means every
 * tool gets full ANSI color support and correct terminal size
 * automatically — no special handling needed.
 *
 * Platform note:
 *   On Linux  → sends command string to bash via PTY write.
 *   On Windows → currently opens a new tab with cmd.exe.
 *                Tool launching via cmd.exe is a future addition.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "tools.h"
#include "tabs.h"
#include "pane.h"
#include "font.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* ── Internal: register_tool ────────────────────────────────── */

/*
 * Convenience helper to register one tool into the registry.
 * Keeps tools_init() readable — one call per tool.
 *
 * Parameters:
 *   tm      — the ToolManager to register into
 *   name    — short display name shown in the launcher list
 *   desc    — one-line description shown next to the name
 *   command — the executable name or full path
 *   new_tab — 1 = always open in a new tab
 *   ...     — extra string arguments, terminated by NULL
 *
 * args[0] is always set to command (argv[0] convention).
 * Up to MAX_TOOL_ARGS-2 extra args can follow.
 */
static void register_tool(ToolManager *tm,
                           const char *name,
                           const char *desc,
                           const char *command,
                           int new_tab, ...) {
    if (tm->count >= MAX_TOOLS) {
        fprintf(stderr, "register_tool: MAX_TOOLS reached\n");
        return;
    }

    ToolDef *t = &tm->tools[tm->count];
    memset(t, 0, sizeof(ToolDef));

    strncpy(t->name,    name,    sizeof(t->name)    - 1);
    strncpy(t->desc,    desc,    sizeof(t->desc)    - 1);
    strncpy(t->command, command, sizeof(t->command) - 1);
    t->new_tab = new_tab;

    /* args[0] = program name (standard C argv convention) */
    t->args[0] = t->command;

    /* Collect variadic extra arguments */
    va_list vargs;
    va_start(vargs, new_tab);
    int arg_idx = 1;
    while (arg_idx < MAX_TOOL_ARGS - 1) {
        char *arg = va_arg(vargs, char *);
        if (arg == NULL) break;
        /*
         * We store arg pointers. For static string literals
         * this is safe — they live for the program lifetime.
         * For dynamic strings, the caller must ensure lifetime.
         */
        t->args[arg_idx++] = arg;
    }
    va_end(vargs);
    t->args[arg_idx] = NULL;  /* NULL-terminate the args array */

    tm->count++;
}


/* ── tools_init ─────────────────────────────────────────────── */

/*
 * Register all built-in tools at startup.
 *
 * To add your own tool, call register_tool() with:
 *   - A short name (shown in launcher list)
 *   - A description (shown to the right of the name)
 *   - The command to run (program name or full path)
 *   - 1 for new_tab (almost always 1)
 *   - Any extra arguments, followed by NULL
 *
 * Example — add a custom script:
 *   register_tool(tm, "myscript", "Run my custom script",
 *                 "/home/user/scripts/myscript.sh", 1, NULL);
 */
void tools_init(ToolManager *tm) {
    memset(tm, 0, sizeof(ToolManager));

    /* ── System monitors ── */
    register_tool(tm, "btop",
        "Interactive system monitor (CPU, RAM, network, disk)",
        "btop", 1, NULL);

    register_tool(tm, "htop",
        "Interactive process viewer and manager",
        "htop", 1, NULL);

    register_tool(tm, "top",
        "Classic Unix process monitor",
        "top", 1, NULL);

    register_tool(tm, "iotop",
        "Monitor disk I/O by process (requires root)",
        "iotop", 1, NULL);

    register_tool(tm, "iftop",
        "Monitor network bandwidth by connection",
        "iftop", 1, NULL);

    /* ── Text editors ── */
    register_tool(tm, "vim",
        "Vi IMproved — powerful modal text editor",
        "vim", 1, NULL);

    register_tool(tm, "nano",
        "Simple beginner-friendly terminal text editor",
        "nano", 1, NULL);

    register_tool(tm, "micro",
        "Modern terminal text editor with mouse support",
        "micro", 1, NULL);

    /* ── Network tools ── */
    register_tool(tm, "nmap",
        "Network exploration and security port scanner",
        "nmap", 1, "--help", NULL);

    register_tool(tm, "netstat",
        "Display network connections and routing tables",
        "netstat", 1, "-tulpn", NULL);

    register_tool(tm, "ss",
        "Socket statistics — faster modern netstat",
        "ss", 1, "-tulpn", NULL);

    register_tool(tm, "curl",
        "Transfer data with URLs (HTTP, FTP, etc.)",
        "curl", 1, "--help", NULL);

    register_tool(tm, "wget",
        "Non-interactive network file downloader",
        "wget", 1, "--help", NULL);

    /* ── Security tools ── */
    register_tool(tm, "reaver",
        "WPS brute force attack tool (requires root + monitor mode)",
        "reaver", 1, "--help", NULL);

    register_tool(tm, "aircrack-ng",
        "802.11 WEP/WPA/WPA2 cracking suite",
        "aircrack-ng", 1, "--help", NULL);

    register_tool(tm, "wireshark",
        "Network protocol analyzer (GTK — opens in new window)",
        "wireshark", 1, NULL);

    /* ── File managers ── */
    register_tool(tm, "ranger",
        "Terminal file manager with vim keybindings",
        "ranger", 1, NULL);

    register_tool(tm, "mc",
        "Midnight Commander — orthodox dual-pane file manager",
        "mc", 1, NULL);

    register_tool(tm, "ncdu",
        "NCurses disk usage analyzer — find large files fast",
        "ncdu", 1, NULL);

    /* ── Programming REPLs ── */
    register_tool(tm, "python3",
        "Python 3 interactive interpreter (REPL)",
        "python3", 1, NULL);

    register_tool(tm, "node",
        "Node.js JavaScript runtime (REPL)",
        "node", 1, NULL);

    register_tool(tm, "lua",
        "Lua 5.x interactive interpreter",
        "lua", 1, NULL);

    register_tool(tm, "gdb",
        "GNU Debugger — debug C/C++ programs",
        "gdb", 1, NULL);

    /* ── Git ── */
    register_tool(tm, "lazygit",
        "Terminal UI for git commands",
        "lazygit", 1, NULL);

    register_tool(tm, "tig",
        "Text-mode interface for git repository browsing",
        "tig", 1, NULL);

    /* ── Shells ── */
    register_tool(tm, "bash",
        "New bash shell session",
        "/bin/bash", 1, NULL);

    register_tool(tm, "zsh",
        "Z shell — extended bash with plugins support",
        "zsh", 1, NULL);

    register_tool(tm, "fish",
        "Friendly interactive shell with autosuggestions",
        "fish", 1, NULL);

    printf("ToolManager: %d tools registered.\n", tm->count);
}


/* ── tools_launcher_open / close ────────────────────────────── */

void tools_launcher_open(ToolManager *tm) {
    ToolLauncher *l     = &tm->launcher;
    l->visible          = 1;
    l->selected         = 0;
    l->search[0]        = '\0';
    l->search_len       = 0;
    printf("Tool launcher opened.\n");
}

void tools_launcher_close(ToolManager *tm) {
    tm->launcher.visible = 0;
}


/* ── tools_launch ───────────────────────────────────────────── */

/*
 * Launch a tool in a new tab.
 *
 * Strategy:
 *   1. Open a new tab → this creates a bash shell via PTY.
 *   2. Wait 150ms for bash to start and print its prompt.
 *   3. Send the tool command string followed by \r (Enter).
 *   4. bash executes the command — the tool takes over the PTY.
 *   5. Update the tab title to show the tool name.
 *
 * This approach works for any tool without modifying pty.c.
 * The tool runs inside bash's process group and gets correct
 * terminal signals (Ctrl+C, Ctrl+Z, window resize, etc.).
 */
void tools_launch(ToolDef *tool, void *tabmgr_ptr,
                  int cols, int rows) {
    TabManager *tm = (TabManager *)tabmgr_ptr;

    /* Open a new tab with a fresh bash session */
    if (tabs_new(tm, cols, rows) != 0) {
        fprintf(stderr, "tools_launch: failed to open new tab\n");
        return;
    }

#ifdef CTERM_LINUX
    /*
     * Give bash time to initialise before we send the command.
     * Without this delay, our write arrives before bash has
     * set up its readline handler and the command gets mangled.
     * 150ms is generous — 50ms usually works too.
     */
    usleep(150000);   /* 150 milliseconds */

    Tab  *tab = tabs_get_active(tm);
    Pane *fp  = pane_get_focused(tab->root);

    if (!fp) {
        fprintf(stderr, "tools_launch: no focused pane\n");
        return;
    }

    /*
     * Build the full command string.
     *
     * Format: "command arg1 arg2 arg3\r"
     * The \r is a carriage return — bash treats it as Enter.
     *
     * We skip args[0] (it's the program name, not an argument
     * we want to pass on the command line).
     */
    char cmd[512] = {0};
    strncat(cmd, tool->command,
            sizeof(cmd) - strlen(cmd) - 1);

    for (int i = 1;
         i < MAX_TOOL_ARGS && tool->args[i] != NULL;
         i++) {
        strncat(cmd, " ",
                sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, tool->args[i],
                sizeof(cmd) - strlen(cmd) - 1);
    }

    /* Terminate with carriage return to execute */
    strncat(cmd, "\r", sizeof(cmd) - strlen(cmd) - 1);

    /* Send the command to bash */
    pty_write(&fp->pty, cmd, (int)strlen(cmd));

    /* Update the tab title so the user knows what's running */
    tab = tabs_get_active(tm);
    snprintf(tab->title, sizeof(tab->title), "%s", tool->name);

    printf("tools_launch: launched '%s' in new tab\n",
           tool->name);

#else
    /*
     * Windows: tab opened with cmd.exe shell.
     * Direct tool launching via cmd.exe will be added
     * in a future update. For now the tab opens a shell.
     */
    (void)tool;
    printf("tools_launch: Windows tool launch not yet implemented\n");
#endif
}


/* ── Filtering helpers ──────────────────────────────────────── */

/*
 * tool_matches — case-insensitive substring search.
 *
 * Returns 1 if 'search' appears in the tool's name OR desc.
 * Returns 1 always when search is empty (show all tools).
 *
 * We implement a simple linear scan rather than pulling in
 * a regex library — fast enough for MAX_TOOLS entries.
 */
static int tool_matches(const ToolDef *tool, const char *search) {
    if (!search || search[0] == '\0') return 1;

    /*
     * Helper: case-insensitive substring search in 'haystack'.
     * Returns 1 if 'needle' is found anywhere in 'haystack'.
     */
    #define ISTR_CONTAINS(haystack, needle)  ({          \
        int _found = 0;                                  \
        const char *_h = (haystack);                     \
        const char *_n = (needle);                       \
        for (int _i = 0; _h[_i] && !_found; _i++) {     \
            int _match = 1;                              \
            for (int _j = 0; _n[_j] && _match; _j++) {  \
                char _hc = _h[_i+_j];                    \
                char _nc = _n[_j];                       \
                if (_hc>='A'&&_hc<='Z') _hc += 32;      \
                if (_nc>='A'&&_nc<='Z') _nc += 32;       \
                if (_hc != _nc) _match = 0;              \
            }                                            \
            if (_match && _n[0]) _found = 1;             \
        }                                                \
        _found;                                          \
    })

    return ISTR_CONTAINS(tool->name, search) ||
           ISTR_CONTAINS(tool->desc, search);

    #undef ISTR_CONTAINS
}

/*
 * get_filtered — fill out_indices[] with indices of tools that
 * match the search string. Returns count of matches.
 */
static int get_filtered(ToolManager *tm, const char *search,
                        int *out_indices, int max) {
    int count = 0;
    for (int i = 0; i < tm->count && count < max; i++) {
        if (tool_matches(&tm->tools[i], search)) {
            out_indices[count++] = i;
        }
    }
    return count;
}


/* ── tools_launcher_handle_key ──────────────────────────────── */

/*
 * Handles a keypress while the launcher overlay is visible.
 *
 * Returns 1 = key was consumed (don't pass to shell).
 * Returns 0 = key not handled (shouldn't happen while open).
 *
 * Key actions:
 *   Escape     → close launcher, return to shell
 *   Enter      → launch selected tool
 *   Up         → move selection up (with wraparound)
 *   Down       → move selection down (with wraparound)
 *   Backspace  → delete last character of search
 */
int tools_launcher_handle_key(ToolManager *tm, SDL_Keycode sym,
                               SDL_Keymod mod, void *tabmgr_ptr,
                               int cols, int rows) {
    (void)mod;
    ToolLauncher *l = &tm->launcher;
    if (!l->visible) return 0;

    /* Build current filtered list to know bounds */
    int indices[MAX_TOOLS];
    int count = get_filtered(tm, l->search, indices, MAX_TOOLS);

    /* Clamp selection into valid range */
    if (count == 0)          l->selected = 0;
    else if (l->selected >= count) l->selected = count - 1;
    else if (l->selected < 0)      l->selected = 0;

    switch (sym) {

        case SDLK_ESCAPE:
            /* Close without launching */
            tools_launcher_close(tm);
            return 1;

        case SDLK_RETURN: {
            /* Launch the currently selected tool */
            if (count > 0 && l->selected < count) {
                ToolDef *tool = &tm->tools[indices[l->selected]];
                tools_launcher_close(tm);
                tools_launch(tool, tabmgr_ptr, cols, rows);
            }
            return 1;
        }

        case SDLK_UP:
            /* Move selection up, wrap from first to last */
            if (count > 0) {
                l->selected--;
                if (l->selected < 0) l->selected = count - 1;
            }
            return 1;

        case SDLK_DOWN:
            /* Move selection down, wrap from last to first */
            if (count > 0) {
                l->selected++;
                if (l->selected >= count) l->selected = 0;
            }
            return 1;

        case SDLK_BACKSPACE:
            /* Delete last character from search string */
            if (l->search_len > 0) {
                l->search[--l->search_len] = '\0';
                l->selected = 0;  /* reset selection on change */
            }
            return 1;

        case SDLK_HOME:
            /* Jump to first result */
            l->selected = 0;
            return 1;

        case SDLK_END:
            /* Jump to last result */
            if (count > 0) l->selected = count - 1;
            return 1;

        default:
            return 1;  /* consume all keys while launcher is open */
    }
}


/* ── tools_launcher_handle_text ─────────────────────────────── */

/*
 * Called from the SDL_TEXTINPUT handler when the launcher is open.
 * Appends the typed character to the search string and resets
 * the selection to the top of the filtered list.
 */
void tools_launcher_handle_text(ToolManager *tm, const char *text) {
    ToolLauncher *l = &tm->launcher;
    if (!l->visible) return;

    int len = (int)strlen(text);
    if (len <= 0) return;

    if (l->search_len + len < (int)sizeof(l->search) - 1) {
        strncat(l->search, text,
                sizeof(l->search) - (size_t)l->search_len - 1);
        l->search_len += len;
        l->selected    = 0;   /* reset to top on new search char */
    }
}


/* ── tools_launcher_draw ────────────────────────────────────── */

/*
 * Renders the tool launcher overlay centered on the window.
 *
 * Visual layout:
 *
 *   ╔══════════════════════════════════════╗
 *   ║  >  search text               cursor ║   search bar
 *   ╠══════════════════════════════════════╣
 *   ║  btop    system monitor              ║   selected row
 *   ║  htop    process viewer              ║
 *   ║  vim     text editor                 ║
 *   ║  nano    simple editor               ║
 *   ╠══════════════════════════════════════╣
 *   ║  Enter=launch  Esc=close  ↑↓=select  ║   footer hint
 *   ╚══════════════════════════════════════╝
 *
 * Rendering order (back to front):
 *   1. Full-window semi-transparent dark backdrop
 *   2. Overlay panel background
 *   3. Overlay border
 *   4. Search bar + prompt glyph + text
 *   5. Cursor blink in search bar
 *   6. Separator line
 *   7. Tool rows (highlighted row for selection)
 *   8. "No results" text if nothing matches
 *   9. Footer hint text
 */
void tools_launcher_draw(ToolManager *tm, SDL_Renderer *renderer,
                         void *font_ptr, int win_w, int win_h) {
    ToolLauncher *l = &tm->launcher;
    if (!l->visible) return;

    Font *font = (Font *)font_ptr;

    /* Build filtered list */
    int indices[MAX_TOOLS];
    int count = get_filtered(tm, l->search, indices, MAX_TOOLS);

    /* Clamp selection */
    if (l->selected < 0)           l->selected = 0;
    if (count > 0 && l->selected >= count)
                                   l->selected = count - 1;

    /* ── Layout constants ── */
    int overlay_w  = 560;
    int row_h      = font->cell_height + 8;
    int rows_shown = (count < 10) ? count : 10;  /* max 10 visible */
    int search_h   = font->cell_height + 14;
    int footer_h   = font->cell_height + 8;
    int overlay_h  = search_h + 2
                     + rows_shown * row_h
                     + footer_h + 8;

    /* Centre the overlay horizontally, place 1/4 from top */
    int ox = (win_w - overlay_w) / 2;
    int oy = (win_h - overlay_h) / 4;
    if (ox < 10) ox = 10;
    if (oy < 10) oy = 10;

    /* ── 1. Dark backdrop over entire window ── */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    SDL_Rect backdrop = {0, 0, win_w, win_h};
    SDL_RenderFillRect(renderer, &backdrop);

    /* ── 2. Overlay panel background ── */
    SDL_SetRenderDrawColor(renderer, 26, 26, 32, 255);
    SDL_Rect panel = {ox, oy, overlay_w, overlay_h};
    SDL_RenderFillRect(renderer, &panel);

    /* ── 3. Overlay border ── */
    SDL_SetRenderDrawColor(renderer, 70, 130, 240, 255);
    SDL_RenderDrawRect(renderer, &panel);

    /* Inner border (double-line effect) */
    SDL_SetRenderDrawColor(renderer, 40, 80, 160, 120);
    SDL_Rect inner = {ox + 1, oy + 1, overlay_w - 2, overlay_h - 2};
    SDL_RenderDrawRect(renderer, &inner);

    /* ── 4. Search bar background ── */
    SDL_SetRenderDrawColor(renderer, 18, 18, 24, 255);
    SDL_Rect search_bg = {ox + 2, oy + 2,
                           overlay_w - 4, search_h - 2};
    SDL_RenderFillRect(renderer, &search_bg);

    /* Search prompt ">" glyph */
    int prompt_x = ox + 10;
    int prompt_y = oy + (search_h - font->cell_height) / 2;
    font_draw_char(font, renderer, '>',
                   prompt_x, prompt_y,
                   70, 140, 255);

    /* Search text */
    int text_x = prompt_x + font->cell_width + 6;
    int text_y = prompt_y;

    if (l->search_len > 0) {
        font_draw_string(font, renderer, l->search,
                         text_x, text_y,
                         220, 220, 220);
    } else {
        font_draw_string(font, renderer,
                         "type to search...",
                         text_x, text_y,
                         65, 65, 75);
    }

    /* ── 5. Cursor blink in search bar ── */
    Uint32 ticks = SDL_GetTicks();
    if ((ticks / 530) % 2 == 0) {
        int cur_x = text_x + l->search_len * font->cell_width;
        SDL_SetRenderDrawColor(renderer, 80, 160, 255, 220);
        SDL_Rect cur_rect = {cur_x, text_y, 2, font->cell_height};
        SDL_RenderFillRect(renderer, &cur_rect);
    }

    /* ── 6. Separator below search bar ── */
    int sep_y = oy + search_h;
    SDL_SetRenderDrawColor(renderer, 55, 65, 100, 255);
    SDL_RenderDrawLine(renderer, ox + 1, sep_y,
                       ox + overlay_w - 2, sep_y);
    SDL_SetRenderDrawColor(renderer, 30, 35, 55, 255);
    SDL_RenderDrawLine(renderer, ox + 1, sep_y + 1,
                       ox + overlay_w - 2, sep_y + 1);

    /* ── 7. Tool rows ── */
    int rows_y = sep_y + 2;

    if (count == 0) {
        /* ── 8. No results ── */
        const char *msg = "no tools match your search";
        int msg_x = ox + (overlay_w
                    - (int)strlen(msg) * font->cell_width) / 2;
        font_draw_string(font, renderer, msg,
                         msg_x, rows_y + 10,
                         85, 85, 95);
    }

    for (int i = 0; i < rows_shown; i++) {
        int tool_idx = indices[i];
        ToolDef *tool = &tm->tools[tool_idx];
        int row_y   = rows_y + i * row_h;
        int is_sel  = (i == l->selected);

        /* Row highlight */
        if (is_sel) {
            SDL_SetRenderDrawColor(renderer, 40, 70, 130, 255);
            SDL_Rect sel_bg = {ox + 2, row_y,
                                overlay_w - 4, row_h};
            SDL_RenderFillRect(renderer, &sel_bg);

            /* Left accent bar on selected row */
            SDL_SetRenderDrawColor(renderer, 80, 160, 255, 255);
            SDL_Rect accent = {ox + 2, row_y, 3, row_h};
            SDL_RenderFillRect(renderer, &accent);
        }

        /* Tool name */
        Uint8 name_r = is_sel ? 110 : 85;
        Uint8 name_g = is_sel ? 200 : 165;
        Uint8 name_b = is_sel ? 255 : 215;
        font_draw_string(font, renderer, tool->name,
                         ox + 14, row_y + 4,
                         name_r, name_g, name_b);

        /* Description — offset to the right of name column */
        /*
         * Name column is 12 characters wide.
         * Description starts after that fixed column.
         */
        int desc_x = ox + 14 + 12 * font->cell_width;
        Uint8 desc_r = is_sel ? 170 : 110;
        Uint8 desc_g = is_sel ? 170 : 110;
        Uint8 desc_b = is_sel ? 170 : 110;

        /*
         * Truncate description so it fits in the overlay.
         * Available width = overlay_w - desc_x offset - 12px margin.
         */
        int max_desc_chars = (overlay_w - (desc_x - ox) - 12)
                             / font->cell_width;
        if (max_desc_chars < 0) max_desc_chars = 0;

        char desc_trunc[128] = {0};
        strncpy(desc_trunc, tool->desc,
                (size_t)max_desc_chars < sizeof(desc_trunc) - 1
                ? (size_t)max_desc_chars
                : sizeof(desc_trunc) - 1);

        /* Add "..." if truncated */
        if ((int)strlen(tool->desc) > max_desc_chars
                && max_desc_chars > 3) {
            desc_trunc[max_desc_chars - 3] = '.';
            desc_trunc[max_desc_chars - 2] = '.';
            desc_trunc[max_desc_chars - 1] = '.';
            desc_trunc[max_desc_chars]     = '\0';
        }

        font_draw_string(font, renderer, desc_trunc,
                         desc_x, row_y + 4,
                         desc_r, desc_g, desc_b);

        /* Row separator (subtle) */
        if (i < rows_shown - 1 && !is_sel) {
            SDL_SetRenderDrawColor(renderer, 38, 38, 48, 255);
            SDL_RenderDrawLine(renderer,
                               ox + 8,  row_y + row_h - 1,
                               ox + overlay_w - 8,
                               row_y + row_h - 1);
        }
    }

    /* ── 9. Footer separator ── */
    int footer_sep_y = rows_y + rows_shown * row_h + 2;
    SDL_SetRenderDrawColor(renderer, 45, 50, 75, 255);
    SDL_RenderDrawLine(renderer,
                       ox + 1, footer_sep_y,
                       ox + overlay_w - 2, footer_sep_y);

    /* Footer hint text */
    int footer_y = footer_sep_y + (footer_h - font->cell_height) / 2;
    font_draw_string(font, renderer,
                     "Enter=launch",
                     ox + 10, footer_y,
                     60, 120, 200);
    font_draw_string(font, renderer,
                     "Esc=close",
                     ox + 10 + 13 * font->cell_width,
                     footer_y,
                     60, 120, 200);
    font_draw_string(font, renderer,
                     "Up/Down=select",
                     ox + 10 + 23 * font->cell_width,
                     footer_y,
                     60, 120, 200);

    /* Result count in footer right side */
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d tools", count);
    int count_x = ox + overlay_w
                  - (int)strlen(count_str) * font->cell_width - 10;
    font_draw_string(font, renderer, count_str,
                     count_x, footer_y,
                     55, 65, 80);
}