cmake_minimum_required(VERSION 3.16)
project(MCRoute VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    find_path(CURSES_INCLUDE_DIR
        NAMES curses.h
        HINTS
            /mingw64/include
            "$ENV{MINGW_PREFIX}/include"
        PATH_SUFFIXES pdcurses
        REQUIRED
    )
    find_library(CURSES_LIBRARY
        NAMES pdcurses_wincon pdcurses
        HINTS
            /mingw64/lib
            "$ENV{MINGW_PREFIX}/lib"
        REQUIRED
    )
    set(CURSES_INCLUDE_DIRS ${CURSES_INCLUDE_DIR})
    set(CURSES_LIBRARIES ${CURSES_LIBRARY})
else()
    find_package(Curses REQUIRED)
endif()

add_executable(MCRoute src/main.cpp)
target_include_directories(MCRoute PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(MCRoute PRIVATE ${CURSES_LIBRARIES})

if(WIN32)
    target_link_libraries(MCRoute PRIVATE ws2_32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(MCRoute PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS MCRoute RUNTIME DESTINATION bin)