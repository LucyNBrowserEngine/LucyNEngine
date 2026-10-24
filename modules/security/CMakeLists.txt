# lucyn::security - NULLBYTE (HMAC-SHA256 integrity seal for captured traffic).
#   1) on its own:      cmake -S modules/security -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/security)
# No third-party code: SHA-256 and HMAC are implemented here and checked against the official test vectors.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_security VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)

if(NOT TARGET lucyn::core)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/_core)
endif()

add_library(lucyn_security STATIC src/nullbyte.cpp)
add_library(lucyn::security ALIAS lucyn_security)
target_include_directories(lucyn_security PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_security PUBLIC cxx_std_17)
target_link_libraries(lucyn_security PUBLIC lucyn::core)
if(WIN32)
  target_link_libraries(lucyn_security PRIVATE bcrypt)
endif()
if(NOT MSVC)
  target_compile_options(lucyn_security PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_security tests/test_security.cpp)
  target_link_libraries(test_security PRIVATE lucyn::security)
  add_test(NAME security COMMAND test_security)
endif()

# libFuzzer harness for the envelope parser / sign path. Clang only (-fsanitize=fuzzer).
option(LUCYN_BUILD_FUZZERS "Build libFuzzer harnesses (requires Clang)" OFF)
if(LUCYN_BUILD_FUZZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(fuzz_nullbyte tests/fuzz_nullbyte.cpp)
    target_link_libraries(fuzz_nullbyte PRIVATE lucyn::security)
    target_compile_options(fuzz_nullbyte PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_nullbyte PRIVATE -fsanitize=fuzzer)
  else()
    message(WARNING "LUCYN_BUILD_FUZZERS needs Clang (-fsanitize=fuzzer); skipping fuzzers")
  endif()
endif()
