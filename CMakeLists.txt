cmake_minimum_required(VERSION 3.16)
project(cterm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(include)

# SDL2
find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

# FreeType
find_package(Freetype REQUIRED)
include_directories(${FREETYPE_INCLUDE_DIRS})

add_executable(cterm
    src/main.c
    src/window.c
    src/font.c
)

target_link_libraries(cterm
    ${SDL2_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    m        # math library — FreeType needs it
)