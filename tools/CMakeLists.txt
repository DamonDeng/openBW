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
