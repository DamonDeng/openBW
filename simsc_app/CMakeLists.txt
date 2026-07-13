cmake_minimum_required(VERSION 3.16)

# Native macOS Qt6 spectator app. Small on purpose:
#   - Reuses the header-only game code (bwgame.h, sync.h, etc.) from
#     the repo root — no separate library to link.
#   - Replaces SDL/asio with QWebSocket for transport and QWidget/QImage
#     for rendering. sync.h talks to the new transport through the
#     duck-typed template contract, so this app never touches the
#     asio version at all.
#
# Day 1 target: a HEADLESS smoke test — no window yet. Just parses a
# --url flag, connects via QWebSocket, and prints frame counters as
# the sync loop advances. Day 2 adds the QWidget game surface.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Homebrew Qt6 on macOS. Users can override with -DCMAKE_PREFIX_PATH.
# We fall back to the default cmake search if Qt6 lives elsewhere.
if (APPLE AND NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt@6")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Network
    WebSockets
)

# UI components are only needed for Day 2+ (widgets, images). Keep the
# find_package call here so we fail fast if the developer's Qt install
# is missing them; the Day 1 headless build doesn't actually use them
# yet, but Day 2 does — no code delta between Day 1 and Day 2 on the
# CMake side.
find_package(Qt6 REQUIRED COMPONENTS Gui Widgets)

# Include path for the game headers. Everything from bwgame.h onward
# is header-only, so we just need the root directory on the include
# path — no library to link.
include_directories(..)

add_executable(simsc_app
    main.cpp
)

target_link_libraries(simsc_app
    PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Gui
    Qt6::Widgets
)

# macOS: build as a normal executable in Day 1, .app bundle later.
# (Bundling requires a plist + icon; not worth doing until the UI
# work of Day 2/3 makes it worth double-clicking.)
set_target_properties(simsc_app PROPERTIES
    MACOSX_BUNDLE FALSE
)
