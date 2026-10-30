cmake_minimum_required(VERSION 3.16)
project(lucyn_controller VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- dependencies -----------------------------------------------------------
# When built standalone, pull in the needed siblings.
if(NOT TARGET lucyn::core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core    ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()
if(NOT TARGET lucyn::net)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../net     ${CMAKE_CURRENT_BINARY_DIR}/net)
endif()
if(NOT TARGET lucyn::security)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../security ${CMAKE_CURRENT_BINARY_DIR}/security)
endif()
if(NOT TARGET lucyn::server)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../server  ${CMAKE_CURRENT_BINARY_DIR}/server)
endif()
if(NOT TARGET lucyn::adblock)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../adblock ${CMAKE_CURRENT_BINARY_DIR}/adblock)
endif()
if(NOT TARGET lucyn::logger)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../logger ${CMAKE_CURRENT_BINARY_DIR}/logger)
endif()
if(NOT TARGET lucyn::search AND (NOT DEFINED LUCYN_MODULE_SEARCH OR LUCYN_MODULE_SEARCH))
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../search  ${CMAKE_CURRENT_BINARY_DIR}/search)
endif()

# ---- library ----------------------------------------------------------------
add_library(lucyn_controller STATIC
    src/controller.cpp
    src/control_link.cpp
)
add_library(lucyn::controller ALIAS lucyn_controller)

target_include_directories(lucyn_controller
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(lucyn_controller PRIVATE -Wall -Wextra)
target_link_libraries(lucyn_controller
    PUBLIC
        lucyn::core
        lucyn::net
        lucyn::security
        lucyn::server
        lucyn::adblock
    PRIVATE
        lucyn::logger
)
if(TARGET lucyn::search)
  target_link_libraries(lucyn_controller PRIVATE lucyn::search)
  target_compile_definitions(lucyn_controller PRIVATE LUCYN_CONTROLLER_HAVE_SEARCH=1)
endif()

# Session 19 steps 5-6: Chromium-backed visit/html/eval/extract/extractAll/click
# via cef_host. Only when CEF is on (the root adds cef_host before this module);
# otherwise — or in a standalone build — those six commands keep the old path.
if(TARGET lucyn::cef_host)
  target_link_libraries(lucyn_controller PRIVATE lucyn::cef_host)
  target_compile_definitions(lucyn_controller PRIVATE LUCYN_CONTROLLER_HAVE_CEF=1)
endif()

# ---- tests ------------------------------------------------------------------
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME OR LUCYN_BUILD_TESTS)
  add_executable(test_controller tests/test_controller.cpp)
  target_link_libraries(test_controller PRIVATE lucyn::controller)
  target_compile_options(test_controller PRIVATE -Wall -Wextra)
  enable_testing()
  add_test(NAME controller COMMAND test_controller)
endif()
