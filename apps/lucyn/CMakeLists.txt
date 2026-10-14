cmake_minimum_required(VERSION 3.16)
project(lucyn_app VERSION 0.5.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lucyn main.cpp)
target_compile_options(lucyn PRIVATE -Wall -Wextra)
target_link_libraries(lucyn PRIVATE lucyn::controller)

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
    target_include_directories(test_cli_download PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/net/tests)
    add_test(NAME cli_download COMMAND test_cli_download $<TARGET_FILE:lucyn>)
endif()
