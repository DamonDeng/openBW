cmake_minimum_required(VERSION 3.5)

# Match server: bwgame.h + JSON-lite hand writer are C++14-clean, but the
# bundled server sha256.h and its style stay on C++17 alongside the rest
# of the tree.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone preprocessing binary: converts .scm/.scx archives into
# per-map static JSON dumps + an _index.json manifest. Not linked into
# the server / observer.
add_executable(openbw_static_map_parser ./static_map_parser.cpp)

# bwgame.h and friends sit in the openbw top-level dir; server/sha256.h
# is one level down.
target_include_directories(openbw_static_map_parser PRIVATE ..)

# slim_mpq: produce a slim StarDat.mpq (etc.) that excludes files
# matching drop prefixes (sound/, music/, smk/). Reuses openBW's own
# MPQ reader + writer helpers. Not linked into server / observer.
add_executable(slim_mpq ./slim_mpq.cpp)
target_include_directories(slim_mpq PRIVATE ..)

# anim_dump: spike toward the SC:R HD renderer. Opens a user's
# StarCraft: Remastered install via the vendored CascLib submodule,
# reads images.rel to map an image_id to a main_NNN.anim file, and
# dumps its header + embedded DDS layers to disk. Not linked into
# server / observer / simsc_app. See tools/anim_dump.cpp for the
# rationale + command-line usage.
#
# CascLib is added_subdirectory'd here (not top-level) so headless
# server builds don't pay the cost. CASC_BUILD_STATIC_LIB=ON gives
# us the static `casc_static` target to link against; the shared
# `casc` build is left on but unused.
if(EXISTS ${CMAKE_SOURCE_DIR}/deps/CascLib/CMakeLists.txt)
	set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
	set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
	set(CASC_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
	# CascLib pins cmake_minimum_required to 3.2, which modern CMake
	# refuses. Override the policy version bump so its subproject
	# still configures. Safe -- CascLib only uses standard commands.
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
	add_subdirectory(${CMAKE_SOURCE_DIR}/deps/CascLib
	                 ${CMAKE_BINARY_DIR}/deps/CascLib EXCLUDE_FROM_ALL)

	add_executable(anim_dump ./anim_dump.cpp)
	target_include_directories(anim_dump PRIVATE
		${CMAKE_SOURCE_DIR}/deps/CascLib/src)
	target_link_libraries(anim_dump PRIVATE casc_static)
else()
	message(STATUS
		"deps/CascLib not present -- anim_dump target skipped. "
		"Run: git submodule update --init deps/CascLib")
endif()
