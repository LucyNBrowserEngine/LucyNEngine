# lucyn::cef_host - the CEF engine host (Session 19, steps 2-4).
#   1) on its own:      cmake -S modules/cef_host -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/cef_host)    (after cmake/FindCEF.cmake is included)
#
# Wraps Chromium Embedded Framework behind a CEF-free public header. Needs the
# core module (nothing-data paths) and the CEF distro (lucyn::cef, from the root's
# cmake/FindCEF.cmake). Standalone build pulls both in here, same "if(NOT TARGET)"
# pattern the other dependency-heavy modules use.
#
# CEF 156 headers require C++20, so this target compiles as C++20 even though the
# rest of LucyN is C++17 (see engine-history/SESSION19_CEF_MIGRATION.md).
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_cef_host VERSION 0.1.0 LANGUAGES C CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)
option(LUCYN_WITH_CEF "Link the CEF engine (Chromium Embedded Framework)" ON)

if(NOT LUCYN_WITH_CEF)
  message(STATUS "lucyn-cef_host: LUCYN_WITH_CEF=OFF, nothing to build")
  return()
endif()

# ---- deps: CEF (interface target) + core (nothing-data paths) --------------
if(NOT TARGET lucyn::cef)
  include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/FindCEF.cmake)
endif()
if(NOT TARGET lucyn::core)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/_core)
endif()

# ---- library ----------------------------------------------------------------
add_library(lucyn_cef_host STATIC src/browser_host.cpp src/preview_window.cpp)
add_library(lucyn::cef_host ALIAS lucyn_cef_host)
target_include_directories(lucyn_cef_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The headful preview window is X11 on Linux. Optional on purpose: without the
# headers the same file compiles to no-ops and the engine simply runs headless
# (that is also what keeps CI and machines without a display green).
if(UNIX AND NOT APPLE)
  find_package(X11 QUIET)
  if(X11_FOUND)
    target_compile_definitions(lucyn_cef_host PRIVATE LUCYN_HAVE_X11=1)
    target_include_directories(lucyn_cef_host PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(lucyn_cef_host PRIVATE X11::X11)
    message(STATUS "lucyn-cef_host: preview window enabled (X11)")
  else()
    message(STATUS "lucyn-cef_host: no X11 found - preview window disabled (engine stays headless)")
  endif()
endif()
# PRIVATE, not PUBLIC: cef_host's *implementation* needs C++20 (CEF 156 headers),
# but its public header (browser_host.h) is CEF-free and C++17-clean. Keeping the
# feature PRIVATE stops it propagating to consumers — controller/app stay C++17,
# where std::filesystem::path::u8string() is still std::string (it becomes
# std::u8string in C++20 and would break existing controller code).
target_compile_features(lucyn_cef_host PRIVATE cxx_std_20)
# lucyn::cef carries the distro root as a SYSTEM include, so its headers never
# trip -Wall -Wextra; our own .cpp still does.
target_link_libraries(lucyn_cef_host
  PUBLIC  lucyn::core
  PRIVATE lucyn::cef)
if(NOT MSVC)
  target_compile_options(lucyn_cef_host PRIVATE -Wall -Wextra)
endif()

# ---- tools -------------------------------------------------------------------
# lucyn-cef-smoke: init CEF, load a URL, print the OnLoadEnd title to stderr.
if(LUCYN_BUILD_TOOLS)
  add_executable(lucyn-cef-smoke tools/lucyn_cef_smoke.cpp)
  target_link_libraries(lucyn-cef-smoke PRIVATE lucyn::cef_host)
  if(NOT MSVC)
    target_compile_options(lucyn-cef-smoke PRIVATE -Wall -Wextra)
  endif()
  if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set_target_properties(lucyn-cef-smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
  # Load libcef.so from next to the binary ($ORIGIN/cef = build/bin/cef), so CEF's
  # DIR_MODULE is the runtime folder and it finds icudtl.dat / *.pak / locales there.
  # (CMake would otherwise add third_party/cef/Release to the rpath, making CEF
  # look for icudtl.dat in a directory that doesn't contain it -> ICU crash.)
  set_target_properties(lucyn-cef-smoke PROPERTIES
    INSTALL_RPATH "$ORIGIN/cef"
    BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# ---- tests -------------------------------------------------------------------
if(LUCYN_BUILD_TESTS)
  add_executable(test_cef_host tests/test_cef_host.cpp)
  target_link_libraries(test_cef_host PRIVATE lucyn::cef_host)
  if(NOT MSVC)
    target_compile_options(test_cef_host PRIVATE -Wall -Wextra)
  endif()
  # Needs libcef.so + icudtl.dat + *.pak: those live in build/bin/cef
  # (lucyn_cef_copy_runtime). Default RUNTIME_OUTPUT_DIRECTORY is build/bin.
  add_test(NAME cef_host COMMAND test_cef_host)
  # Same $ORIGIN/cef rpath as the smoke tool: libcef.so + ICU data resolve from build/bin/cef.
  set_target_properties(test_cef_host PROPERTIES
    INSTALL_RPATH "$ORIGIN/cef"
    BUILD_WITH_INSTALL_RPATH TRUE)
  # CEF init spins up a browser process; give it a little headroom over the
  # default 1500ms before ctest calls it hung.
  set_tests_properties(cef_host PROPERTIES TIMEOUT 60)
endif()
