# lucyn::server - TinyElysia (HTTP + WebSocket on vendored Mongoose) and the Moonbug capture server/dashboard.
#   1) on its own:      cmake -S modules/server -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/server)   (after net, security)
# Everything binds to 127.0.0.1 only. Mongoose is GPL-2.0 or commercial: see third_party/mongoose/VENDORED.txt.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_server VERSION 0.1.0 LANGUAGES C CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

foreach(dep core net security)
  if(NOT TARGET lucyn::${dep})
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../${dep} ${CMAKE_CURRENT_BINARY_DIR}/_${dep})
  endif()
endforeach()
if(NOT TARGET lucyn::mongoose)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/mongoose ${CMAKE_CURRENT_BINARY_DIR}/_mongoose)
endif()

# dashboard/home_dashboard.html -> dashboard_gen.inc (one source of truth: edit the .html)
set(LUCYN_DASH_HTML ${CMAKE_CURRENT_LIST_DIR}/../../dashboard/home_dashboard.html)
set(LUCYN_DASH_GEN  ${CMAKE_CURRENT_BINARY_DIR}/gen/dashboard_gen.inc)
add_custom_command(
  OUTPUT ${LUCYN_DASH_GEN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
  COMMAND ${CMAKE_COMMAND} -DFILE=${LUCYN_DASH_HTML} -DOUT=${LUCYN_DASH_GEN} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_dashboard.cmake
  DEPENDS ${LUCYN_DASH_HTML} ${CMAKE_CURRENT_SOURCE_DIR}/embed_dashboard.cmake
  COMMENT "Embedding home_dashboard.html")

add_library(lucyn_server STATIC src/tinyelysia.cpp src/capture_server.cpp ${LUCYN_DASH_GEN})
add_library(lucyn::server ALIAS lucyn_server)
target_include_directories(lucyn_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_features(lucyn_server PUBLIC cxx_std_17)
target_link_libraries(lucyn_server PUBLIC lucyn::net lucyn::security PRIVATE lucyn::mongoose)
if(NOT MSVC)
  target_compile_options(lucyn_server PRIVATE -Wall -Wextra)
endif()


if(LUCYN_BUILD_TESTS)
  add_executable(test_server tests/test_server.cpp)
  target_link_libraries(test_server PRIVATE lucyn::server)
  add_test(NAME server COMMAND test_server)
endif()
