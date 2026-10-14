# lucyn::ui - SDL2 window wrapper + Ghostful Mode (SESSION11_PLAN.md Part D).
#
# SDL2 is an OPTIONAL dependency, on purpose (SESSION11_PLAN.md "SDL2 decisions
# to make and write down"): if it isn't found, this module quietly does not
# build, and everything else — paint, layout, screenshot — still does. Keeps
# the "tiny, small deps" promise and keeps the sandbox usable without a display.
#   1) on its own:      cmake -S modules/ui -B build && cmake --build build && \
#                        SDL_VIDEODRIVER=dummy ctest --test-dir build --output-on-failure
#   2) from the root:   add_subdirectory(modules/ui)   (guarded by LUCYN_MODULE_UI)
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_ui VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)

foreach(dep paint)
  if(NOT TARGET lucyn::${dep})
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../${dep} ${CMAKE_CURRENT_BINARY_DIR}/_${dep})
  endif()
endforeach()

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  # Fallback: SDL2 ships an sdl2-config.cmake/pkg-config file on most distros,
  # but not always a CONFIG package CMake's find_package(SDL2) recognises.
  # Try pkg-config before giving up, since that's what `apt install libsdl2-dev`
  # actually leaves behind.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2_PC QUIET sdl2)
  endif()
endif()

if(NOT SDL2_FOUND AND NOT SDL2_PC_FOUND)
  message(STATUS "lucyn::ui: SDL2 not found (apt install libsdl2-dev) — skipping. paint/layout/screenshot are unaffected.")
  return()
endif()

add_library(lucyn_ui STATIC src/window.cpp)
add_library(lucyn::ui ALIAS lucyn_ui)
target_include_directories(lucyn_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_ui PUBLIC cxx_std_17)
target_link_libraries(lucyn_ui PUBLIC lucyn::paint)
if(SDL2_FOUND)
  target_link_libraries(lucyn_ui PRIVATE $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,${SDL2_LIBRARIES}>)
  if(NOT TARGET SDL2::SDL2)
    target_include_directories(lucyn_ui PRIVATE ${SDL2_INCLUDE_DIRS})
  endif()
else()
  target_include_directories(lucyn_ui PRIVATE ${SDL2_PC_INCLUDE_DIRS})
  target_link_libraries(lucyn_ui PRIVATE ${SDL2_PC_LIBRARIES})
  target_link_directories(lucyn_ui PRIVATE ${SDL2_PC_LIBRARY_DIRS})
endif()
if(NOT MSVC)
  target_compile_options(lucyn_ui PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_ui tests/test_ui.cpp)
  target_link_libraries(test_ui PRIVATE lucyn::ui)
  add_test(NAME ui COMMAND test_ui)
  set_tests_properties(ui PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()
