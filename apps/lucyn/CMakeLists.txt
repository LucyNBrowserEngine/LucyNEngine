cmake_minimum_required(VERSION 3.16)
project(lucyn_app VERSION 0.5.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same "if(NOT TARGET)" guard the other modules use, so apps/lucyn also builds
# standalone (or with -DLUCYN_MODULE_LOGGER=OFF at the root).
if(NOT TARGET lucyn::logger)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../modules/logger ${CMAKE_CURRENT_BINARY_DIR}/logger)
endif()

add_executable(lucyn main.cpp)
target_compile_options(lucyn PRIVATE -Wall -Wextra)
# lucyn::logger: every line the engine prints on its own behalf (the launch
# banner, the mode, warnings) goes through the logger, always on stderr —
# stdout belongs to the JSON protocol.
target_link_libraries(lucyn PRIVATE lucyn::controller lucyn::logger)

# Install alongside the other tools so it shares nothing-data/.
if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set_target_properties(lucyn PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
endif()

# CLI contract test for `lucyn --download` (runs the real binary against a loopback server).
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME OR LUCYN_BUILD_TESTS)
    add_executable(test_cli_download tests/test_cli_download.cpp)
    target_link_libraries(test_cli_download PRIVATE lucyn::controller)
    target_compile_options(test_cli_download PRIVATE -Wall -Wextra)
    # mini_server.h sits beside this test (moved out of modules/net/tests when libcurl left).
    add_test(NAME cli_download COMMAND test_cli_download $<TARGET_FILE:lucyn>)
endif()
