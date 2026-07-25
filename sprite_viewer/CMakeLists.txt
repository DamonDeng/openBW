cmake_minimum_required(VERSION 3.16)

# Sprite viewer: a Qt6 desktop app that shows one openBW unit at a
# time, driving its animations through the real iscript.bin engine.
# Reuses everything from bwgame.h / ui/ui.h / simsc_app's Qt drawing
# backend.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Homebrew Qt6 on macOS; users can override with -DCMAKE_PREFIX_PATH.
if (APPLE AND NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt@6")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
)

# Include paths -- game headers live at repo root, ui subheaders in
# ../ui, simsc_app has common.h and font_bitmap.h.
include_directories(.. ../ui ../simsc_app)

# Same header guards that simsc_app uses so ui.h/native_window.h
# stay clean.
add_definitions(-DOPENBW_ENABLE_UI -DOPENBW_NO_SDL_IMAGE -DOPENBW_NO_SDL_MIXER)

add_executable(sprite_viewer
    main.cpp
    viewer_window.cpp
    sim_harness.cpp
    # Reuse the Qt drawing backend from simsc_app directly. It
    # provides native_window::* + native_window_drawing::* symbols
    # that ui.h and its friends need. No .qrc means the HUD chrome
    # doesn't load -- QImage::isNull() gates it off, so we render
    # a clean canvas without the console frame.
    ../simsc_app/qt_native_window.cpp
)

target_link_libraries(sprite_viewer
    PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

set_target_properties(sprite_viewer PROPERTIES
    MACOSX_BUNDLE FALSE
)
