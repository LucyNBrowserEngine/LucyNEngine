# lucyn::js - the real shell::ScriptHost: QuickJS plus a DOM bridge over lucyn::dom.
#   1) on its own:      cmake -S modules/js -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/js)    (after shell and profile)
# QuickJS-ng is vendored in third_party/quickjs (plain C; this module turns the C language on for it).
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_js VERSION 0.1.0 LANGUAGES C CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)

foreach(dep core profile dom shell)
  if(NOT TARGET lucyn::${dep})
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../${dep} ${CMAKE_CURRENT_BINARY_DIR}/_${dep})
  endif()
endforeach()
if(NOT TARGET lucyn::quickjs)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/quickjs ${CMAKE_CURRENT_BINARY_DIR}/_quickjs)
endif()
if(NOT TARGET lucyn::mongoose)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/mongoose ${CMAKE_CURRENT_BINARY_DIR}/_mongoose)
endif()

# src/prelude/*.js -> prelude_gen.inc (one source of truth: edit the .js files)
file(GLOB LUCYN_JS_PRELUDE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/prelude/p*.js)
set(LUCYN_JS_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen/prelude_gen.inc)
add_custom_command(
  OUTPUT ${LUCYN_JS_GEN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/src/prelude -DOUT=${LUCYN_JS_GEN} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_prelude.cmake
  DEPENDS ${LUCYN_JS_PRELUDE} ${CMAKE_CURRENT_SOURCE_DIR}/embed_prelude.cmake
  COMMENT "Embedding the JS prelude")

add_library(lucyn_js STATIC src/host.cpp src/dom_bridge.cpp src/window.cpp src/prelude.cpp src/storage_bridge.cpp src/fetch_bridge.cpp src/websocket_bridge.cpp ${LUCYN_JS_GEN})
add_library(lucyn::js ALIAS lucyn_js)
target_include_directories(lucyn_js PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_features(lucyn_js PUBLIC cxx_std_17)
target_link_libraries(lucyn_js PUBLIC lucyn::shell lucyn::dom lucyn::profile lucyn::core PRIVATE lucyn::quickjs lucyn::mongoose)
if(NOT MSVC)
  target_compile_options(lucyn_js PRIVATE -Wall -Wextra)
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_js tests/test_js.cpp)
  target_link_libraries(test_js PRIVATE lucyn::js)
  add_test(NAME js COMMAND test_js)

  add_executable(test_js_net tests/test_js_net.cpp)
  target_link_libraries(test_js_net PRIVATE lucyn::js)
  add_test(NAME js_net COMMAND test_js_net)
endif()
