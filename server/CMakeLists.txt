cmake_minimum_required(VERSION 3.5)

# nlohmann/json requires C++17 for its filesystem support and cleanest error
# messages, though it works with C++14. Bump the local target to C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Headless authoritative sim. No SDL2; only bwgame + sync + asio (header-only)
# and the auth module.
add_executable(openbw_server ./main.cpp)
target_include_directories(openbw_server PRIVATE ..)

# asio needs threads.
find_package(Threads REQUIRED)
target_link_libraries(openbw_server PRIVATE Threads::Threads)

# openbw_keygen: standalone CLI for generating API keys / users.json entries.
add_executable(openbw_keygen ./keygen.cpp)
target_include_directories(openbw_keygen PRIVATE ..)

# openbw_auth_test: sanity tests for sha256 + user_registry.
add_executable(openbw_auth_test ./auth_test.cpp)
target_include_directories(openbw_auth_test PRIVATE ..)
