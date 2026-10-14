# lucyn::layout - box tree + block/inline layout (SESSION11_PLAN.md Part C).
#   1) on its own:      cmake -S modules/layout -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/layout)   (needs dom and paint)
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_layout VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)

foreach(dep dom paint)
  if(NOT TARGET lucyn::${dep})
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../${dep} ${CMAKE_CURRENT_BINARY_DIR}/_${dep})
  endif()
endforeach()

add_library(lucyn_layout STATIC src/layout.cpp)
add_library(lucyn::layout ALIAS lucyn_layout)
target_include_directories(lucyn_layout PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_layout PUBLIC cxx_std_17)
target_link_libraries(lucyn_layout PUBLIC lucyn::dom lucyn::paint)
if(NOT MSVC)
  target_compile_options(lucyn_layout PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_layout tests/test_layout.cpp)
  target_link_libraries(test_layout PRIVATE lucyn::layout)
  add_test(NAME layout COMMAND test_layout)
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
