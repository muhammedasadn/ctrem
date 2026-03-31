#ifndef TOOLS_H
#define TOOLS_H

#include <SDL2/SDL.h>
#include <stdarg.h>
#include "retro_theme.h"

#define MAX_TOOLS     64
#define MAX_TOOL_ARGS 16
#define LAUNCHER_ROWS 12

typedef struct {
    char  name[32];
    char  desc[80];
    char  command[128];
    char *args[MAX_TOOL_ARGS];
    int   new_tab;
} ToolDef;

typedef struct {
    int  visible;
    int  selected;
    int  scroll_offset;
    char search[64];
    int  search_len;
} ToolLauncher;

typedef struct {
    ToolDef      tools[MAX_TOOLS];
    int          count;
    ToolLauncher launcher;
} ToolManager;

void tools_init(ToolManager *tm);
void tools_launcher_open(ToolManager *tm);
void tools_launcher_close(ToolManager *tm);
int  tools_launcher_handle_key(ToolManager *tm, SDL_Keycode sym,
                               SDL_Keymod mod, void *tabmgr_ptr,
                               int cols, int rows);
void tools_launcher_handle_text(ToolManager *tm, const char *text);
void tools_launcher_draw(ToolManager *tm, SDL_Renderer *renderer,
                         void *font_ptr, int win_w, int win_h);
void tools_launch(ToolDef *tool, void *tabmgr_ptr, int cols, int rows);

#endif
