cmake_minimum_required(VERSION 3.16)
project(lucyn_controller VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- dependencies -----------------------------------------------------------
# When built standalone, pull in the needed siblings.
if(NOT TARGET lucyn::core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core    ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()
if(NOT TARGET lucyn::profile)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../profile ${CMAKE_CURRENT_BINARY_DIR}/profile)
endif()
if(NOT TARGET lucyn::net)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../net     ${CMAKE_CURRENT_BINARY_DIR}/net)
endif()
if(NOT TARGET lucyn::dom)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../dom     ${CMAKE_CURRENT_BINARY_DIR}/dom)
endif()
if(NOT TARGET lucyn::shell)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../shell   ${CMAKE_CURRENT_BINARY_DIR}/shell)
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
if(NOT TARGET lucyn::js)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../js      ${CMAKE_CURRENT_BINARY_DIR}/js)
endif()

# ---- library ----------------------------------------------------------------
add_library(lucyn_controller STATIC
    src/controller.cpp
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
        lucyn::profile
        lucyn::net
        lucyn::shell
        lucyn::js
        lucyn::security
        lucyn::server
        lucyn::adblock
)

# ---- tests ------------------------------------------------------------------
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME OR LUCYN_BUILD_TESTS)
  add_executable(test_controller tests/test_controller.cpp)
  target_link_libraries(test_controller PRIVATE lucyn::controller)
  target_compile_options(test_controller PRIVATE -Wall -Wextra)
  enable_testing()
  add_test(NAME controller COMMAND test_controller)
  add_executable(test_controller_net tests/test_controller_net.cpp)
  target_link_libraries(test_controller_net PRIVATE lucyn::controller)
  target_include_directories(test_controller_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../net/tests)
  add_test(NAME controller_net COMMAND test_controller_net)
  add_executable(test_controller_download tests/test_controller_download.cpp)
  target_link_libraries(test_controller_download PRIVATE lucyn::controller)
  target_compile_options(test_controller_download PRIVATE -Wall -Wextra)
  target_include_directories(test_controller_download PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../net/tests)
  add_test(NAME controller_download COMMAND test_controller_download)
endif()
