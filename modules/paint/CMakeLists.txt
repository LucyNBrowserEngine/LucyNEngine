# lucyn::paint - display list, Painter interface, SoftwarePainter (SESSION11_PLAN.md Part B).
#   1) on its own:      cmake -S modules/paint -B build && cmake --build build && ctest --test-dir build
#   2) from the root:   add_subdirectory(modules/paint)
# No SDL anywhere in this module (README: "layout never draws, and exactly one
# backend turns [the display list] into pixels" — ui::Window shows the result
# later). stb_truetype/stb_image/stb_image_write are vendored single headers
# (third_party/stb); one small font is vendored and embedded into the binary
# (third_party/fonts, embed_font.cmake) so there is no host font search.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lucyn_paint VERSION 0.1.0 LANGUAGES CXX)
  enable_testing()
endif()

option(LUCYN_BUILD_TESTS "Build module tests" ON)
option(LUCYN_BUILD_TOOLS "Build module command-line tools" ON)

# Optional: when modules/dom and modules/layout are both present (root build,
# or modules/layout's own standalone build, which pulls paint in as a
# subdirectory), lucyn-paint also gets --file page.html out.png. paint's own
# CMakeLists.txt runs too early to see lucyn::layout (layout hasn't defined
# its target yet when it add_subdirectory()s this one), so that wiring is
# done by whoever DOES see both: modules/layout/CMakeLists.txt (standalone)
# or the root CMakeLists.txt (full build) — same pattern the root already
# uses to wire lucyn::js into lucyn-shell after the fact.

set(LUCYN_PAINT_FONT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fonts/PTSans-Regular.ttf)
set(LUCYN_PAINT_FONT_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen/font_gen.inc)
add_custom_command(
  OUTPUT ${LUCYN_PAINT_FONT_GEN}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
  COMMAND ${CMAKE_COMMAND} -DFONT=${LUCYN_PAINT_FONT} -DSYM=kLucynPaintFont -DOUT=${LUCYN_PAINT_FONT_GEN}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_font.cmake
  DEPENDS ${LUCYN_PAINT_FONT} ${CMAKE_CURRENT_SOURCE_DIR}/embed_font.cmake
  COMMENT "Embedding the paint module's font (third_party/fonts, see VENDORED.txt)")

add_library(lucyn_paint STATIC src/software_painter.cpp src/font_data.cpp ${LUCYN_PAINT_FONT_GEN})
add_library(lucyn::paint ALIAS lucyn_paint)
target_include_directories(lucyn_paint
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_BINARY_DIR}/gen
          ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)
target_compile_features(lucyn_paint PUBLIC cxx_std_17)
if(NOT MSVC)
  target_compile_options(lucyn_paint PRIVATE -Wall -Wextra)
else()
  target_compile_definitions(lucyn_paint PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(LUCYN_BUILD_TOOLS)
  add_executable(lucyn-paint tools/lucyn-paint.cpp)
  target_link_libraries(lucyn-paint PRIVATE lucyn::paint)
  # --file support (LUCYN_PAINT_HAVE_LAYOUT_DOM) is wired in from the outside,
  # see the comment above and modules/layout/CMakeLists.txt.
endif()

if(LUCYN_BUILD_TESTS)
  add_executable(test_paint tests/test_paint.cpp)
  target_link_libraries(test_paint PRIVATE lucyn::paint)
  target_include_directories(test_paint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)
  add_test(NAME paint COMMAND test_paint)
endif()
