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

# CascLib: vendored under deps/CascLib. Static-link only; the
# shared lib and tests are turned off. This adds ~2 MB to the
# sprite_viewer binary and lets us open a user's SC:R install
# for HD asset loading (see hd_asset_loader.{h,cpp}).
#
# add_subdirectory only fires once even if tools/CMakeLists.txt
# also references it -- the top-level tools build guards its own
# CascLib inclusion with EXCLUDE_FROM_ALL. We do the same here so
# a build that turns sprite_viewer on but tools off still works.
if(NOT TARGET casc_static)
    if(EXISTS ${CMAKE_SOURCE_DIR}/deps/CascLib/CMakeLists.txt)
        set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
        set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
        set(CASC_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
        # CascLib pins to cmake_minimum_required(3.2); modern CMake
        # rejects it without this bump.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        add_subdirectory(${CMAKE_SOURCE_DIR}/deps/CascLib
                         ${CMAKE_BINARY_DIR}/deps/CascLib EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR
            "deps/CascLib missing. Run:\n"
            "  git submodule update --init deps/CascLib")
    endif()
endif()

add_executable(sprite_viewer
    main.cpp
    viewer_window.cpp
    sim_harness.cpp
    hd_asset_loader.cpp
    mapping_tab.cpp
    frames_tab.cpp
    playground_tab.cpp
    drawing_playground_tab.cpp
    # Reuse the Qt drawing backend from simsc_app directly. It
    # provides native_window::* + native_window_drawing::* symbols
    # that ui.h and its friends need. No .qrc means the HUD chrome
    # doesn't load -- QImage::isNull() gates it off, so we render
    # a clean canvas without the console frame.
    ../simsc_app/qt_native_window.cpp
)

target_include_directories(sprite_viewer PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/CascLib/src
)

target_link_libraries(sprite_viewer
    PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    casc_static
)

set_target_properties(sprite_viewer PROPERTIES
    MACOSX_BUNDLE FALSE
)
