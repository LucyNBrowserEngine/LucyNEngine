# lucyn::logger - small, thread-safe engine diagnostics logger.
# Standalone: cmake -S modules/logger -B build-logger
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_logger VERSION 0.1.0 LANGUAGES CXX)
endif()

add_library(lucyn_logger STATIC src/logger.cpp)
add_library(lucyn::logger ALIAS lucyn_logger)
target_include_directories(lucyn_logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lucyn_logger PUBLIC cxx_std_17)
if(NOT MSVC)
  target_compile_options(lucyn_logger PRIVATE -Wall -Wextra)
endif()
