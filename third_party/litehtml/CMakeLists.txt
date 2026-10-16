# Vendored litehtml — https://github.com/litehtml/litehtml (BSD-3-Clause)
# Pulled in by modules/layout when LUCYN_LITEHTML=ON.
cmake_minimum_required(VERSION 3.16)
project(litehtml C CXX)

# ---- bundled gumbo -------------------------------------------------------
set(SOURCE_GUMBO
    src/gumbo/src/attribute.c
    src/gumbo/src/char_ref.c
    src/gumbo/src/char_ref_gperf.c
    src/gumbo/src/error.c
    src/gumbo/src/parser.c
    src/gumbo/src/string_buffer.c
    src/gumbo/src/string_piece.c
    src/gumbo/src/tag.c
    src/gumbo/src/tokenizer.c
    src/gumbo/src/utf8.c
    src/gumbo/src/util.c
    src/gumbo/src/vector.c
)
add_library(lucyn_gumbo STATIC ${SOURCE_GUMBO})
add_library(lucyn::gumbo ALIAS lucyn_gumbo)
target_include_directories(lucyn_gumbo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/gumbo/src)
set_target_properties(lucyn_gumbo PROPERTIES C_STANDARD 99)
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_include_directories(lucyn_gumbo PRIVATE src/gumbo/visualc/include)
endif()

# ---- litehtml core -------------------------------------------------------
set(SOURCE_LITEHTML
    src/background.cpp
    src/codepoint.cpp
    src/css_borders.cpp
    src/css_length.cpp
    src/css_parser.cpp
    src/css_properties.cpp
    src/css_selector.cpp
    src/css_tokenizer.cpp
    src/document.cpp
    src/document_container.cpp
    src/el_anchor.cpp
    src/el_base.cpp
    src/el_before_after.cpp
    src/el_body.cpp
    src/el_break.cpp
    src/el_cdata.cpp
    src/el_comment.cpp
    src/el_div.cpp
    src/el_font.cpp
    src/el_image.cpp
    src/el_link.cpp
    src/el_para.cpp
    src/el_script.cpp
    src/el_space.cpp
    src/el_style.cpp
    src/el_table.cpp
    src/el_td.cpp
    src/el_text.cpp
    src/el_title.cpp
    src/el_tr.cpp
    src/element.cpp
    src/encodings.cpp
    src/flex_item.cpp
    src/flex_line.cpp
    src/formatting_context.cpp
    src/gradient.cpp
    src/html.cpp
    src/html_microsyntaxes.cpp
    src/html_tag.cpp
    src/iterators.cpp
    src/line_box.cpp
    src/media_query.cpp
    src/num_cvt.cpp
    src/render_block.cpp
    src/render_block_context.cpp
    src/render_flex.cpp
    src/render_image.cpp
    src/render_inline_context.cpp
    src/render_item.cpp
    src/render_table.cpp
    src/string_id.cpp
    src/strtod.cpp
    src/style.cpp
    src/stylesheet.cpp
    src/table.cpp
    src/url.cpp
    src/url_path.cpp
    src/utf8_strings.cpp
    src/web_color.cpp
)
add_library(litehtml STATIC ${SOURCE_LITEHTML})
add_library(litehtml::litehtml ALIAS litehtml)
target_include_directories(litehtml
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include/litehtml)
target_link_libraries(litehtml PUBLIC lucyn::gumbo)
target_compile_features(litehtml PUBLIC cxx_std_17)
if(MSVC)
    target_compile_definitions(litehtml PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(litehtml PRIVATE /wd4244 /wd4267 /wd4805)
else()
    target_compile_options(litehtml PRIVATE -w)
endif()
