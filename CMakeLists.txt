cmake_minimum_required(VERSION 3.16)
project(cterm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Platform detection ────────────────────────────────────────
if(WIN32)
    message(STATUS "Building for Windows")
    add_compile_definitions(CTERM_WINDOWS _WIN32_WINNT=0x0A00)
    # 0x0A00 = Windows 10 minimum (required for ConPTY)
else()
    message(STATUS "Building for Linux")
    add_compile_definitions(
        CTERM_LINUX
        _GNU_SOURCE
        _DEFAULT_SOURCE
    )
endif()

include_directories(include)

# ── SDL2 ──────────────────────────────────────────────────────
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

# ── FreeType ──────────────────────────────────────────────────
find_package(Freetype REQUIRED)
include_directories(${FREETYPE_INCLUDE_DIRS})

# ── Source files ──────────────────────────────────────────────
add_executable(cterm
    src/main.c
    src/window.c
    src/font.c
    src/pty.c          # dispatcher — includes linux or windows
    src/ansi.c
    src/tabs.c
    src/pane.c
    src/tools.c
)

# ── Link libraries ────────────────────────────────────────────
if(WIN32)
    target_link_libraries(cterm
        ${SDL2_LIBRARIES}
        ${FREETYPE_LIBRARIES}
        # Windows system libraries
        kernel32
        user32
        gdi32
        m
    )
else()
    target_link_libraries(cterm
        ${SDL2_LIBRARIES}
        ${FREETYPE_LIBRARIES}
        util    # openpty() on Linux
        m
    )
endif()

# ── Compiler flags ────────────────────────────────────────────
if(WIN32)
    target_compile_options(cterm PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
else()
    target_compile_options(cterm PRIVATE
        -Wall -Wextra -Wno-unused-parameter -g
    )
endif()