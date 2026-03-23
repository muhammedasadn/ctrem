# Minimum CMake version required
cmake_minimum_required(VERSION 3.16)

# Project name and language
project(cterm C)

# Use the C99 standard
# C99 added many useful features we'll need later
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Tell CMake where our header files live
include_directories(include)

# Define what we're building:
# an executable named "cterm" built from src/main.c
add_executable(cterm
    src/main.c
)
