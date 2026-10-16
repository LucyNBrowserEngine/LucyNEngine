# lucyn::layout - box tree + block/inline layout (SESSION11_PLAN.md Part C).
# SESSION17+: litehtml integration for full CSS colours/images/gradients.
#   1) on its own:      cmake -S modules/layout -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/layout)   (needs dom and paint)
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_layout VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_LITEHTML "Integrate litehtml for full CSS colour/image rendering" ON)

foreach(dep dom paint)
  if(NOT TARGET lucyn::${dep})
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../${dep} ${CMAKE_CURRENT_BINARY_DIR}/_${dep})
  endif()
endforeach()

# ---- litehtml (vendored) ---------------------------------------------------
if(LUCYN_LITEHTML)
  set(LITEHTML_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/litehtml)
  if(NOT TARGET litehtml)
    add_subdirectory(${LITEHTML_DIR} ${CMAKE_CURRENT_BINARY_DIR}/_litehtml)
  endif()
endif()

# ---- layout library --------------------------------------------------------
set(LAYOUT_SOURCES src/layout.cpp src/style.cpp src/css.cpp)
if(LUCYN_LITEHTML AND TARGET litehtml)
  list(APPEND LAYOUT_SOURCES
       src/litehtml_container.cpp
       src/litehtml_layout.cpp)
endif()

add_library(lucyn_layout STATIC ${LAYOUT_SOURCES})
add_library(lucyn::layout ALIAS lucyn_layout)
target_include_directories(lucyn_layout PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_layout PUBLIC cxx_std_17)
target_link_libraries(lucyn_layout PUBLIC lucyn::dom lucyn::paint)

if(LUCYN_LITEHTML AND TARGET litehtml)
  target_link_libraries(lucyn_layout PUBLIC litehtml::litehtml)
  target_compile_definitions(lucyn_layout PUBLIC LUCYN_HAVE_LITEHTML=1)
  # litehtml headers need to be visible to the container .cpp
  target_include_directories(lucyn_layout PRIVATE ${LITEHTML_DIR}/include)
endif()

if(NOT MSVC)
  target_compile_options(lucyn_layout PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_layout tests/test_layout.cpp)
  target_link_libraries(test_layout PRIVATE lucyn::layout)
  add_test(NAME layout COMMAND test_layout)
  add_executable(test_style tests/test_style.cpp)
  target_link_libraries(test_style PRIVATE lucyn::layout)
  add_test(NAME layout_style COMMAND test_style)
endif()

# paint's CMakeLists.txt runs BEFORE lucyn::layout exists (it is add_subdirectory'd
# from the foreach above, and this library isn't defined until the lines just
# above), so paint cannot see us at its own configure time. Same chicken-and-egg
# fix the root CMakeLists.txt already uses for lucyn-shell + lucyn::js: wire
# lucyn-paint's --file support here, now that both targets exist.
if(TARGET lucyn-paint AND TARGET lucyn::dom)
  target_link_libraries(lucyn-paint PRIVATE lucyn::dom lucyn::layout)
  target_compile_definitions(lucyn-paint PRIVATE LUCYN_PAINT_HAVE_LAYOUT_DOM=1)
endif()
