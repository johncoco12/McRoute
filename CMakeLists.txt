cmake_minimum_required(VERSION 3.16)
project(MCRoute VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Curses REQUIRED)

add_executable(MCRoute src/main.cpp)
target_include_directories(MCRoute PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(MCRoute PRIVATE ${CURSES_LIBRARIES})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(MCRoute PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS MCRoute RUNTIME DESTINATION bin)