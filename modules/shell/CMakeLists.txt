# lucyn::shell - open a site inside the shell page, inject user scripts, snapshot before/after.
#   1) on its own:      cmake -S modules/shell -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/shell)
# Needs lucyn::dom and lucyn::core (pulled in when standalone). If lucyn::net exists (root build),
# the lucyn-shell tool can fetch URLs; otherwise it takes --file only.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_shell VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

if(NOT TARGET lucyn::core)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/_core)
endif()
if(NOT TARGET lucyn::dom)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../dom ${CMAKE_CURRENT_BINARY_DIR}/_dom)
endif()

add_library(lucyn_shell STATIC
  src/scripts.cpp src/url.cpp src/snapshot.cpp src/host.cpp src/assets.cpp src/hydrator.cpp)
add_library(lucyn::shell ALIAS lucyn_shell)
target_include_directories(lucyn_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_shell PUBLIC cxx_std_17)
target_link_libraries(lucyn_shell PUBLIC lucyn::dom lucyn::core)
if(NOT MSVC)
  target_compile_options(lucyn_shell PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TOOLS)
  add_executable(lucyn-shell tools/lucyn-shell.cpp)
  target_link_libraries(lucyn-shell PRIVATE lucyn::shell)
  if(TARGET lucyn::net)
    target_link_libraries(lucyn-shell PRIVATE lucyn::net)
    target_compile_definitions(lucyn-shell PRIVATE LUCYN_SHELL_HAVE_NET=1)
  endif()
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_shell tests/test_shell.cpp)
  target_link_libraries(test_shell PRIVATE lucyn::shell)
  add_test(NAME shell COMMAND test_shell)
endif()
