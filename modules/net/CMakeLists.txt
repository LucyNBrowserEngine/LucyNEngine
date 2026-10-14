# lucyn::net - works two ways:
#   1) on its own:      cmake -S modules/net -B build [-DLUCYN_CURL_IMPERSONATE_ROOT=...]
#   2) from the root:   add_subdirectory(modules/net)
# It depends on lucyn::profile and pulls it in by itself when built standalone.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_net VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

# Point this at an unpacked libcurl-impersonate release (the unpacked release tarball: include/ plus libcurl-impersonate.so).
# Empty = build against stock libcurl: everything compiles and runs, but
# impersonation is reported as unavailable instead of silently faked.
set(LUCYN_CURL_IMPERSONATE_ROOT "" CACHE PATH "Path to libcurl-impersonate (include/ + lib/)")

# If you unpacked the release into <repo>/third_party/curl-impersonate/, it is used
# automatically (no -D needed). Pass -DLUCYN_USE_BUNDLED_CURL=OFF to ignore it.
option(LUCYN_USE_BUNDLED_CURL "Use third_party/curl-impersonate when it exists" ON)
set(_bundled "${CMAKE_CURRENT_LIST_DIR}/../../third_party/curl-impersonate")
if(LUCYN_USE_BUNDLED_CURL AND NOT LUCYN_CURL_IMPERSONATE_ROOT AND EXISTS "${_bundled}/include/curl/curl.h")
  get_filename_component(LUCYN_CURL_IMPERSONATE_ROOT "${_bundled}" ABSOLUTE)
  message(STATUS "lucyn::net: using bundled libcurl-impersonate at ${LUCYN_CURL_IMPERSONATE_ROOT}")
endif()

if(NOT TARGET lucyn::profile)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../profile ${CMAKE_CURRENT_BINARY_DIR}/_profile)
endif()
if(NOT TARGET lucyn::adblock)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../adblock ${CMAKE_CURRENT_BINARY_DIR}/_adblock)
endif()

if(LUCYN_CURL_IMPERSONATE_ROOT)
  # The release tarball puts libcurl-impersonate.so at its top level (no lib/ folder)
  # and the headers in include/curl/. Both layouts (flat and lib/) are accepted.
  find_path(LUCYN_CURL_INCLUDE curl/curl.h PATHS "${LUCYN_CURL_IMPERSONATE_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
  find_library(LUCYN_CURL_LIB NAMES curl-impersonate curl-impersonate-chrome
               PATHS "${LUCYN_CURL_IMPERSONATE_ROOT}" "${LUCYN_CURL_IMPERSONATE_ROOT}/lib" "${LUCYN_CURL_IMPERSONATE_ROOT}/lib64"
               NO_DEFAULT_PATH REQUIRED)
  set(_curl_includes "${LUCYN_CURL_INCLUDE}")
  set(_curl_libs     "${LUCYN_CURL_LIB}")
else()
  find_package(CURL REQUIRED)
  set(_curl_includes "${CURL_INCLUDE_DIRS}")
  set(_curl_libs     "${CURL_LIBRARIES}")
endif()

# Does the curl we found actually have curl_easy_impersonate?
include(CheckSymbolExists)
unset(LUCYN_HAS_IMPERSONATE CACHE)   # never trust an answer cached from an earlier configure
set(CMAKE_REQUIRED_INCLUDES  "${_curl_includes}")
set(CMAKE_REQUIRED_LIBRARIES "${_curl_libs}")
check_symbol_exists(curl_easy_impersonate "curl/curl.h" LUCYN_HAS_IMPERSONATE)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

add_library(lucyn_net STATIC src/http_client.cpp src/moonbug.cpp)
add_library(lucyn::net ALIAS lucyn_net)
target_include_directories(lucyn_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${_curl_includes})
target_link_libraries(lucyn_net PUBLIC lucyn::profile PRIVATE lucyn::adblock ${_curl_libs})
if(LUCYN_HAS_IMPERSONATE)
  target_compile_definitions(lucyn_net PRIVATE LUCYN_HAS_IMPERSONATE=1)
else()
  message(WARNING "lucyn::net: no curl_easy_impersonate found, building WITHOUT impersonation. Set LUCYN_CURL_IMPERSONATE_ROOT.")
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
