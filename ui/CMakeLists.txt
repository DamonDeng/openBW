cmake_minimum_required(VERSION 3.1)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")

find_package(SDL2 REQUIRED)

# SDL2_mixer: look it up ourselves (no bundled FindSDL2_mixer.cmake).
find_path(SDL2_MIXER_INCLUDE_DIR SDL_mixer.h
	PATHS ${SDL2_INCLUDE_DIRS} ENV SDL2DIR
	PATH_SUFFIXES SDL2 include/SDL2 include)
find_library(SDL2_MIXER_LIBRARY NAMES SDL2_mixer
	PATHS ENV SDL2DIR
	PATH_SUFFIXES lib)

include_directories(
	..
	${SDL2_INCLUDE_DIRS}
	${SDL2_MIXER_INCLUDE_DIR}
)

add_definitions(-DOPENBW_NO_SDL_IMAGE)

if (NOT SDL2_MIXER_LIBRARY OR NOT SDL2_MIXER_INCLUDE_DIR)
	message(STATUS "SDL2_mixer not found — building openbw_ui without sound")
	add_definitions(-DOPENBW_NO_SDL_MIXER)
	set(_openbw_ui_extra_libs)
else()
	message(STATUS "SDL2_mixer found: ${SDL2_MIXER_LIBRARY}")
	set(_openbw_ui_extra_libs ${SDL2_MIXER_LIBRARY})
endif()

add_definitions(-DOPENBW_ENABLE_UI)

add_library(openbw_ui ./sdl2.cpp)

target_link_libraries(openbw_ui ${SDL2_LIBRARIES} ${_openbw_ui_extra_libs})

set_property(TARGET openbw_ui PROPERTY POSITION_INDEPENDENT_CODE ON)
