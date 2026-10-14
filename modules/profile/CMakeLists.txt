# lucyn::profile - works two ways:
#   1) on its own:      cmake -S modules/profile -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/profile)   (see /CMakeLists.txt)
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_profile VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

find_package(Threads REQUIRED)

# The only dependency: lucyn::core (where nothing-data lives). Pulled in when built standalone.
if(NOT TARGET lucyn::core)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/_core)
endif()

add_library(lucyn_profile STATIC src/profile.cpp src/identity.cpp src/identity_store.cpp)
add_library(lucyn::profile ALIAS lucyn_profile)
target_include_directories(lucyn_profile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_profile PUBLIC cxx_std_17)
target_link_libraries(lucyn_profile PUBLIC Threads::Threads lucyn::core)
if(NOT MSVC)
  target_compile_options(lucyn_profile PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TOOLS)
  add_executable(lucyn-profile tools/lucyn-profile.cpp)
  target_link_libraries(lucyn-profile PRIVATE lucyn::profile)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_profile tests/test_profile.cpp)
  target_link_libraries(test_profile PRIVATE lucyn::profile)
  add_test(NAME profile COMMAND test_profile)

  add_executable(test_identity tests/test_identity.cpp)
  target_link_libraries(test_identity PRIVATE lucyn::profile)
  add_test(NAME identity COMMAND test_identity)
endif()
