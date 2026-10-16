# lucyn::net - works two ways:
#   1) on its own:      cmake -S modules/net -B build
#   2) from the root:   add_subdirectory(modules/net)
# It depends on lucyn::profile and pulls it in by itself when built standalone.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_net VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

# ---- libcurl-impersonate: ONE place, third_party/curl-impersonate/ ---------------------------------------------------------------
# LucyN looks for libcurl-impersonate (headers AND library) in <repo>/third_party/curl-impersonate/ and NOWHERE else: not in a
# path you pass with -D, not in /usr, not stock libcurl. There is no fallback. A silent fallback to stock libcurl is what made
# the engine speak curl's TLS fingerprint instead of a browser's, so a missing library is now a configure error with the fix.
# Layout (exactly what the release tarball unpacks to): libcurl-impersonate.so, include/curl/*.h, LICENSE_*.
set(LUCYN_CURL_IMPERSONATE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/curl-impersonate")
get_filename_component(LUCYN_CURL_IMPERSONATE_ROOT "${LUCYN_CURL_IMPERSONATE_ROOT}" ABSOLUTE)

if(NOT TARGET lucyn::profile)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../profile ${CMAKE_CURRENT_BINARY_DIR}/_profile)
endif()
if(NOT TARGET lucyn::adblock)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../adblock ${CMAKE_CURRENT_BINARY_DIR}/_adblock)
endif()

# A previous configure may have cached an answer from somewhere else (an older CMakeLists, a system path): forget it.
unset(LUCYN_CURL_INCLUDE CACHE)
unset(LUCYN_CURL_LIB CACHE)
find_path(LUCYN_CURL_INCLUDE curl/curl.h
          PATHS "${LUCYN_CURL_IMPERSONATE_ROOT}/include" NO_DEFAULT_PATH)
find_library(LUCYN_CURL_LIB NAMES curl-impersonate curl-impersonate-chrome
             PATHS "${LUCYN_CURL_IMPERSONATE_ROOT}" "${LUCYN_CURL_IMPERSONATE_ROOT}/lib" "${LUCYN_CURL_IMPERSONATE_ROOT}/lib64"
             NO_DEFAULT_PATH)
if(NOT LUCYN_CURL_INCLUDE OR NOT LUCYN_CURL_LIB)
  message(FATAL_ERROR
    "lucyn::net: libcurl-impersonate not found in ${LUCYN_CURL_IMPERSONATE_ROOT}\n"
    "  (need include/curl/curl.h and libcurl-impersonate.so there; LucyN looks nowhere else)\n"
    "  Install it with (pick the file for `uname -m`: x86_64-linux-gnu | aarch64-linux-gnu | riscv64-linux-gnu | arm64-macos | x86_64-macos):\n"
    "    cd third_party && mkdir -p curl-impersonate && cd curl-impersonate\n"
    "    curl -LO https://github.com/lexiforest/curl-impersonate/releases/download/v2.2.3/libcurl-impersonate-v2.2.3.x86_64-linux-gnu.tar.gz\n"
    "    tar xzf libcurl-impersonate-v2.2.3.x86_64-linux-gnu.tar.gz && rm libcurl-impersonate-v2.2.3.x86_64-linux-gnu.tar.gz")
endif()
message(STATUS "lucyn::net: libcurl-impersonate = ${LUCYN_CURL_LIB} (headers: ${LUCYN_CURL_INCLUDE})")
set(_curl_includes "${LUCYN_CURL_INCLUDE}")
set(_curl_libs     "${LUCYN_CURL_LIB}")

# The library there must really be libcurl-impersonate, not a plain libcurl someone dropped in.
include(CheckSymbolExists)
unset(LUCYN_HAS_IMPERSONATE CACHE)   # never trust an answer cached from an earlier configure
set(CMAKE_REQUIRED_INCLUDES  "${_curl_includes}")
set(CMAKE_REQUIRED_LIBRARIES "${_curl_libs}")
check_symbol_exists(curl_easy_impersonate "curl/curl.h" LUCYN_HAS_IMPERSONATE)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT LUCYN_HAS_IMPERSONATE)
  message(FATAL_ERROR "lucyn::net: ${LUCYN_CURL_LIB} has no curl_easy_impersonate. It is not libcurl-impersonate; "
                      "replace third_party/curl-impersonate/ with the release tarball (see third_party/README.md).")
endif()

add_library(lucyn_net STATIC src/http_client.cpp src/moonbug.cpp)
add_library(lucyn::net ALIAS lucyn_net)
target_include_directories(lucyn_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${_curl_includes})
target_link_libraries(lucyn_net PUBLIC lucyn::profile lucyn::adblock PRIVATE ${_curl_libs})
target_compile_definitions(lucyn_net PRIVATE LUCYN_HAS_IMPERSONATE=1)
# Make the runtime loader use THAT library too: without an rpath, a system libcurl could be picked up at run time.
if(NOT WIN32)
  target_link_options(lucyn_net PUBLIC "LINKER:-rpath,${LUCYN_CURL_IMPERSONATE_ROOT}")
endif()
if(NOT MSVC)
  target_compile_options(lucyn_net PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TOOLS)
  add_executable(lucyn-fetch tools/lucyn-fetch.cpp)
  target_link_libraries(lucyn-fetch PRIVATE lucyn::net)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_net tests/test_net.cpp)
  target_link_libraries(test_net PRIVATE lucyn::net)
  add_test(NAME net COMMAND test_net)

  add_executable(test_moonbug tests/test_moonbug.cpp)
  target_link_libraries(test_moonbug PRIVATE lucyn::net)
  add_test(NAME moonbug COMMAND test_moonbug)

  add_executable(test_net_request tests/test_net_request.cpp)
  target_link_libraries(test_net_request PRIVATE lucyn::net)
  add_test(NAME net_request COMMAND test_net_request)
endif()
