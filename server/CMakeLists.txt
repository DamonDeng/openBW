cmake_minimum_required(VERSION 3.5)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Headless authoritative sim. No SDL2; only bwgame + sync + asio (header-only).
add_executable(openbw_server ./main.cpp)

target_include_directories(openbw_server PRIVATE ..)

# asio needs threads.
find_package(Threads REQUIRED)
target_link_libraries(openbw_server PRIVATE Threads::Threads)
