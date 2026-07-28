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

# Crash backtraces. Only meaningful on Linux (execinfo.h + rdynamic);
# harmless on macOS which uses the same interfaces via libSystem. We
# always add -g and -rdynamic so a SIGSEGV in the deployed pod prints
# a symbolic stack trace via server/crash_handler.h. -rdynamic is
# what surfaces C++ mangled names in backtrace_symbols_fd output.
# Kept unconditional (not just Debug) because we WANT the crash frame
# even in a production build; the extra binary size + linker time is
# a rounding error for this project's needs.
target_compile_options(openbw_server PRIVATE -g)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(openbw_server PRIVATE -rdynamic)
endif()

# openbw_keygen: standalone CLI for generating API keys / users.json entries.
add_executable(openbw_keygen ./keygen.cpp)
target_include_directories(openbw_keygen PRIVATE ..)

# openbw_auth_test: sanity tests for sha256 + user_registry.
add_executable(openbw_auth_test ./auth_test.cpp)
target_include_directories(openbw_auth_test PRIVATE ..)
