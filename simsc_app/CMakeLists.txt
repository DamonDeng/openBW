cmake_minimum_required(VERSION 3.16)

# Native macOS Qt6 spectator app. Small on purpose:
#   - Reuses the header-only game code (bwgame.h, sync.h, ui/ui.h, etc.)
#     from the repo root — no library to link.
#   - Replaces SDL/asio with QWebSocket for transport and QWidget/QImage
#     for rendering. sync.h talks to the new transport through the
#     duck-typed template contract, and ui.h talks to the drawing
#     backend through native_window.h / native_window_drawing.h.
#
# Day 2 target: a real window rendering the game via
# ui_functions::update(). The Qt drawing backend is a straight
# reimplementation of ui/sdl2.cpp using QImage and QPainter.

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
    Gui
    Widgets
)

# Include path for the game headers. Everything from bwgame.h onward
# is header-only, so we just need the repo root + ui/ on the include
# path — no library to link.
include_directories(.. ../ui)

# The game rendering (ui.h) expects the ui/ compile unit to declare
# certain macros — see ui/CMakeLists.txt. We're not linking openbw_ui
# (that's SDL), but ui.h itself is header-only + guarded by these.
# OPENBW_NO_SDL_IMAGE / OPENBW_NO_SDL_MIXER suppress any include of
# SDL_image.h / SDL_mixer.h, which we don't have on the Qt path.
add_definitions(-DOPENBW_ENABLE_UI -DOPENBW_NO_SDL_IMAGE -DOPENBW_NO_SDL_MIXER)

add_executable(simsc_app
    main.cpp
    qt_native_window.cpp   # Qt implementation of ui/native_window.h
                           # + ui/native_window_drawing.h + native_sound.h
                           # (drop-in for ui/sdl2.cpp -- pick one or the
                           # other, never both).
    resources.qrc          # embeds assets/*.png into the binary; AUTORCC
                           # (via CMAKE_AUTORCC below) turns .qrc -> .cpp.
)

# Auto-run rcc on any .qrc listed in the target's sources.
set_property(TARGET simsc_app PROPERTY AUTORCC ON)

target_link_libraries(simsc_app
    PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Gui
    Qt6::Widgets
)

# macOS: build as a normal executable in Day 2, .app bundle later.
# (Bundling requires a plist + icon; not worth doing until the UI
# work of Day 3 makes it worth double-clicking.)
set_target_properties(simsc_app PROPERTIES
    MACOSX_BUNDLE FALSE
)
