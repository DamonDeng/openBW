cmake_minimum_required(VERSION 3.16)

# End-user Qt6 desktop lobby + observer.
#
# Design notes worth keeping close to the CMake:
#   * We reuse simsc_app/qt_native_window.cpp verbatim (no fork).
#     Its API is the ui/native_window.h + native_window_drawing.h
#     + native_sound.h trio; Phase 1a doesn't need any of the sim
#     side of it, but we compile it in so Phase 1b's ObserverWindow
#     wire-up is a one-file addition.
#   * Same OPENBW_* defines as simsc_app, so header-only ui.h behaves
#     the same way (no SDL_image / SDL_mixer includes).
#   * MACOSX_BUNDLE TRUE (unlike simsc_app): this is the user-facing
#     app, so we want a .app bundle with icon + info.plist + staged
#     resources.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Match simsc_app's Homebrew fallback so a user with a Homebrew Qt
# install can just run cmake without CMAKE_PREFIX_PATH.
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

# Header-only openBW headers live in the repo root; ui.h + native_*
# headers live under ../ui. Same as simsc_app.
include_directories(.. ../ui ../simsc_app)

# See simsc_app/CMakeLists.txt for why these three defines.
add_definitions(-DOPENBW_ENABLE_UI -DOPENBW_NO_SDL_IMAGE -DOPENBW_NO_SDL_MIXER)

add_executable(simsc_desktop
    main.cpp
    settings.cpp
    app_paths.cpp
    local_user_roster.cpp
    map_catalog.cpp
    local_server_manager.cpp
    simsc_api_client.cpp
    remote_games_model.cpp
    agent_catalog.cpp
    agent_supervisor.cpp

    ui/main_window.cpp
    ui/settings_tab.cpp
    ui/local_games_tab.cpp
    ui/remote_games_tab.cpp
    ui/new_local_game_dialog.cpp
    ui/new_remote_game_dialog.cpp
    ui/observer_window.cpp

    # Reused from simsc_app -- do NOT duplicate. Contains the
    # native_window / native_window_drawing implementations that
    # ObserverWindow drives.
    ../simsc_app/qt_native_window.cpp
)

target_link_libraries(simsc_desktop
    PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Gui
    Qt6::Widgets
)

# .app bundle on macOS. Non-macOS builds get a plain binary in the
# build tree; packaging for those platforms is out of scope for
# Phase 1.
set_target_properties(simsc_desktop PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "simsc_desktop"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.openbw.simsc_desktop"
    MACOSX_BUNDLE_INFO_STRING "openBW end-user client"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
)

# ------------------------------------------------------------------
# Stage runtime assets next to (or inside) the built binary.
#
# On macOS, applicationDirPath() is <bundle>/Contents/MacOS, and
# resources live at <bundle>/Contents/Resources. AppPaths probes
# those locations first.
#
# The rules below are post-build (not install-time). That lets a
# developer run the un-installed binary straight out of the build
# directory and still find the server + MPQs.
# ------------------------------------------------------------------

if (APPLE)
    set(_bundle_root "$<TARGET_BUNDLE_DIR:simsc_desktop>")
    set(_macos_dir   "${_bundle_root}/Contents/MacOS")
    set(_res_dir     "${_bundle_root}/Contents/Resources")
else()
    set(_macos_dir   "$<TARGET_FILE_DIR:simsc_desktop>")
    set(_res_dir     "$<TARGET_FILE_DIR:simsc_desktop>/../Resources")
endif()

# Copy openbw_server next to the app binary. Depends on the server
# target so CMake tracks the dependency correctly.
if (TARGET openbw_server)
    add_custom_command(TARGET simsc_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_macos_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:openbw_server>"
            "${_macos_dir}/openbw_server"
        COMMENT "simsc_desktop: staging openbw_server binary"
    )
else()
    message(WARNING
        "simsc_desktop: openbw_server target not defined -- the app "
        "will need SIMSC_DESKTOP_SERVER_BIN pointed at a server binary "
        "at run time. Set -DOPENBW_BUILD_SERVER=ON.")
endif()

# Optionally stage MPQs if the user provides them at configure time.
# We deliberately don't ship MPQs in the repo (Blizzard IP), so this
# is opt-in.
if (SIMSC_DESKTOP_MPQ_SOURCE_DIR)
    add_custom_command(TARGET simsc_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_res_dir}/mpqs"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SIMSC_DESKTOP_MPQ_SOURCE_DIR}"
            "${_res_dir}/mpqs"
        COMMENT "simsc_desktop: staging MPQs from ${SIMSC_DESKTOP_MPQ_SOURCE_DIR}"
    )
endif()

# Optionally stage bundled maps.
if (SIMSC_DESKTOP_MAPS_SOURCE_DIR)
    add_custom_command(TARGET simsc_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_res_dir}/maps"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SIMSC_DESKTOP_MAPS_SOURCE_DIR}"
            "${_res_dir}/maps"
        COMMENT "simsc_desktop: staging maps from ${SIMSC_DESKTOP_MAPS_SOURCE_DIR}"
    )
endif()

# Stage maps.json (the shared map catalog) if simsc/app/static/maps.json
# is present. Same origin as the server uses.
set(_maps_json_src "${CMAKE_SOURCE_DIR}/simsc/app/static/maps.json")
if (EXISTS "${_maps_json_src}")
    add_custom_command(TARGET simsc_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_res_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_maps_json_src}"
            "${_res_dir}/maps.json"
        COMMENT "simsc_desktop: staging maps.json"
    )
endif()
