# lucyn::lnm - the LNM (Lucy Native Markup) parser + interpreter (LNM_SPEC.md v0.2).
#   1) on its own:      cmake -S lnminterpreter -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(lnminterpreter)   (needs dom, paint and layout)
#
# This folder sits next to modules/ (author's request) but is laid out exactly like a
# module (include/lucyn/lnm, src, tools, tests), so moving it to modules/lnm/ later is a
# rename: nothing in here refers to its own location except through CMAKE_CURRENT_LIST_DIR.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_lnm VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

foreach(dep dom paint layout)
  if(NOT TARGET lucyn::${dep})
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../modules/${dep} ${CMAKE_CURRENT_BINARY_DIR}/_${dep})
  endif()
endforeach()

add_library(lucyn_lnm STATIC src/parser.cpp src/compile.cpp src/app.cpp)
add_library(lucyn::lnm ALIAS lucyn_lnm)
target_include_directories(lucyn_lnm PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(lucyn_lnm PUBLIC cxx_std_17)
target_link_libraries(lucyn_lnm PUBLIC lucyn::dom lucyn::layout lucyn::paint)
if(NOT MSVC)
  target_compile_options(lucyn_lnm PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_lnm_parser tests/test_lnm_parser.cpp)
  target_link_libraries(test_lnm_parser PRIVATE lucyn::lnm)
  add_test(NAME lnm_parser COMMAND test_lnm_parser)
  add_executable(test_lnm_app tests/test_lnm_app.cpp)
  target_link_libraries(test_lnm_app PRIVATE lucyn::lnm)
  add_test(NAME lnm_app COMMAND test_lnm_app)
endif()

if(LUCYN_BUILD_TOOLS)
  add_executable(lucyn-lnm tools/lucyn-lnm.cpp)
  target_link_libraries(lucyn-lnm PRIVATE lucyn::lnm)
endif()
