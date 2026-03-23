cmake_minimum_required(VERSION 3.16)
project(cterm C)

# ── C standard ───────────────────────────────────────────────
# C99: // comments, designated initialisers, <stdint.h>, etc.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Feature-test macros — set GLOBALLY for all source files ──
#
# These must reach the compiler before any system header is
# processed. Setting them here in CMake guarantees every .c
# file in the project compiles with them defined, which is
# safer than relying on each file to define them correctly.
#
# _GNU_SOURCE     — exposes GNU/Linux extensions: openpty(),
#                   getline(), strdup(), and many others.
# _DEFAULT_SOURCE — equivalent on newer glibc versions.
#                   Both together cover all Ubuntu/Debian
#                   versions from 14.04 onward.
add_compile_definitions(
    _GNU_SOURCE
    _DEFAULT_SOURCE
)

# ── Include paths ─────────────────────────────────────────────
include_directories(include)

# ── SDL2 ──────────────────────────────────────────────────────
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

# ── FreeType ──────────────────────────────────────────────────
find_package(Freetype REQUIRED)
include_directories(${FREETYPE_INCLUDE_DIRS})

# ── Source files ─────────────────────────────────────────────
add_executable(cterm
    src/main.c       # Entry point, main loop, event handling
    src/window.c     # SDL2 window + renderer init/destroy
    src/font.c       # FreeType glyph cache + text rendering
    src/pty.c        # PTY open, read, write, resize, destroy
    src/ansi.c       # ANSI/VT100 parser + terminal cell grid
    src/tabs.c       # Tab bar + TabManager
)

# ── Link libraries ────────────────────────────────────────────
target_link_libraries(cterm
    ${SDL2_LIBRARIES}       # Window, renderer, events, input
    ${FREETYPE_LIBRARIES}   # Font rasterisation
    util                    # Contains openpty() on Linux
    m                       # Math library (FreeType uses it)
)

# ── Compiler warnings + debug symbols ────────────────────────
# -Wall/-Wextra  catch real bugs during development.
# -g             embeds debug symbols for use with gdb.
# Remove -g and add -O2 for a release/production build.
target_compile_options(cterm PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -g
)
