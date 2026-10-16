# lucyn::dom - works two ways:
#   1) on its own:      cmake -S modules/dom -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/dom)
# Needs Gumbo (sudo apt install libgumbo-dev). If lucyn::net exists (root build),
# the lucyn-dom tool also gets --url.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_dom VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GUMBO QUIET IMPORTED_TARGET gumbo)
endif()
if(TARGET PkgConfig::GUMBO)
  set(LUCYN_GUMBO_LIB PkgConfig::GUMBO)
elseif(TARGET lucyn::gumbo)
  # Bundled gumbo from third_party/litehtml (available when LUCYN_LITEHTML=ON
  # and litehtml was added before this module).
  set(LUCYN_GUMBO_LIB lucyn::gumbo)
else()
  find_path(GUMBO_INCLUDE_DIR gumbo.h)
  find_library(GUMBO_LIBRARY gumbo)
  if(NOT GUMBO_INCLUDE_DIR OR NOT GUMBO_LIBRARY)
    # Last resort: build bundled gumbo from third_party/litehtml if present.
    set(_BUNDLED_GUMBO ${CMAKE_CURRENT_LIST_DIR}/../../third_party/litehtml)
    if(EXISTS "${_BUNDLED_GUMBO}/CMakeLists.txt" AND NOT TARGET lucyn_gumbo)
      add_subdirectory(${_BUNDLED_GUMBO} ${CMAKE_CURRENT_BINARY_DIR}/_litehtml_for_gumbo)
    endif()
    if(TARGET lucyn::gumbo)
      set(LUCYN_GUMBO_LIB lucyn::gumbo)
    else()
      message(FATAL_ERROR "lucyn::dom needs Gumbo: sudo apt install libgumbo-dev, or enable LUCYN_LITEHTML=ON (or turn off LUCYN_MODULE_DOM)")
    endif()
  else()
    add_library(lucyn_gumbo_imported INTERFACE)
    target_include_directories(lucyn_gumbo_imported INTERFACE ${GUMBO_INCLUDE_DIR})
    target_link_libraries(lucyn_gumbo_imported INTERFACE ${GUMBO_LIBRARY})
    set(LUCYN_GUMBO_LIB lucyn_gumbo_imported)
  endif()
endif()

add_library(lucyn_dom STATIC src/dom.cpp src/parse.cpp src/mutate.cpp src/diff.cpp)
add_library(lucyn::dom ALIAS lucyn_dom)
target_include_directories(lucyn_dom PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_dom PUBLIC cxx_std_17)
target_link_libraries(lucyn_dom PRIVATE ${LUCYN_GUMBO_LIB})
if(NOT MSVC)
  target_compile_options(lucyn_dom PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TOOLS)
  add_executable(lucyn-dom tools/lucyn-dom.cpp)
  target_link_libraries(lucyn-dom PRIVATE lucyn::dom)
  if(TARGET lucyn::net)
    target_link_libraries(lucyn-dom PRIVATE lucyn::net)
    target_compile_definitions(lucyn-dom PRIVATE LUCYN_DOM_HAVE_NET=1)
  endif()
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_dom tests/test_dom.cpp)
  target_link_libraries(test_dom PRIVATE lucyn::dom)
  add_test(NAME dom COMMAND test_dom)

  add_executable(test_dom_mutate tests/test_dom_mutate.cpp)
  target_link_libraries(test_dom_mutate PRIVATE lucyn::dom)
  add_test(NAME dom_mutate COMMAND test_dom_mutate)
endif()
