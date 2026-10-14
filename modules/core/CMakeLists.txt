# lucyn::core - paths and tiny shared helpers. No dependencies.
#   1) on its own:      cmake -S modules/core -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/core)   (other modules pull it in themselves)
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_core VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)

add_library(lucyn_core STATIC src/paths.cpp)
add_library(lucyn::core ALIAS lucyn_core)
target_include_directories(lucyn_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_core PUBLIC cxx_std_17)
if(NOT MSVC)
  target_compile_options(lucyn_core PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE lucyn::core)
  add_test(NAME core COMMAND test_core)
endif()
