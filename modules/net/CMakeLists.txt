# lucyn::net — Moonbug, the network capture wiretap (Session 19).
#
# Session 19 ripped out the old engine: HttpClient / libcurl-impersonate are GONE (Chromium's
# network stack in CEF is the HTTP client now) and the controller feeds Moonbug straight from
# CEF's CefResourceRequestHandler. What remains here is ONLY the Moonbug data structures + tap:
# the Capture struct, the listener registry, the history ring and the JSON serialiser. It is
# fully self-contained (standard library only) — no libcurl, no adblock, no engine code.
#
#   1) on its own:      cmake -S modules/net -B build && cmake --build build
#   2) from the root:   add_subdirectory(modules/net)
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_net VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)

# ---- library -------------------------------------------------------------------
add_library(lucyn_net STATIC src/moonbug.cpp)
add_library(lucyn::net ALIAS lucyn_net)
target_include_directories(lucyn_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_net PRIVATE cxx_std_17)
if(NOT MSVC)
  target_compile_options(lucyn_net PRIVATE -Wall -Wextra)
endif()

# ---- tests ---------------------------------------------------------------------
# Only the Moonbug unit tests survive: the old HttpClient capture tests left with the class.
if(LUCYN_BUILD_TESTS)
  add_executable(test_moonbug tests/test_moonbug.cpp)
  target_link_libraries(test_moonbug PRIVATE lucyn::net)
  add_test(NAME moonbug COMMAND test_moonbug)
endif()
