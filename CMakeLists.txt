cmake_minimum_required(VERSION 3.5)
project(openbw_agent CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# Force the observer executable to be built when this repo is the top-level
# project. When consumed as a subdirectory (e.g. by the bwapi project),
# openbw_ui builds as a library and the observer is off unless enabled.
option(OPENBW_BUILD_OBSERVER "Build the openbw_observer SDL2 spectator client" ON)
option(OPENBW_BUILD_SERVER   "Build the openbw_server headless authoritative sim" ON)

# ui/CMakeLists.txt hard-requires SDL2 (openbw_ui and openbw_observer target
# both live there). Only descend if we actually intend to build them --
# otherwise a headless server-only image would need to install SDL2 dev libs
# just to satisfy the cmake configure step.
if (OPENBW_BUILD_OBSERVER)
	add_subdirectory(ui)
endif()
if (OPENBW_BUILD_SERVER)
	add_subdirectory(server)
endif()
