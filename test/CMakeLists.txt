cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(WIN32)
  add_library(expat STATIC
    ${REPO_ROOT}/lib/expat/xmlparse.c
    ${REPO_ROOT}/lib/expat/xmlrole.c
    ${REPO_ROOT}/lib/expat/xmltok.c
  )
  target_include_directories(expat PUBLIC ${REPO_ROOT}/lib/expat)
  target_compile_definitions(expat PUBLIC XML_STATIC PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024 BYTEORDER=1234)
  add_library(EXPAT::EXPAT ALIAS expat)
endif()

if(MSVC)
  add_compile_options(/utf-8 /Zc:preprocessor)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES strcasecmp=_stricmp strncasecmp=_strnicmp)
endif()

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/ThaiText
)
if(MSVC)
  target_compile_options(crosspoint_test_common INTERFACE /W4)
else()
  target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
endif()

enable_testing()
include(GoogleTest)
option(THAI_HOST_ENGINE_STATS "Measure Thai engine counters in the host probe" ON)
if(THAI_HOST_ENGINE_STATS)
  add_compile_definitions(THAI_ENGINE_STATS=1)
endif()

add_library(crosspoint_thai STATIC
  ${REPO_ROOT}/lib/ThaiText/ThaiCluster.cpp
  ${REPO_ROOT}/lib/ThaiText/ThaiLineBreaker.cpp
  ${REPO_ROOT}/lib/ThaiText/ThaiDictionary.cpp
  ${REPO_ROOT}/lib/ThaiText/ThaiSegmenter.cpp
  ${REPO_ROOT}/lib/ThaiText/ThaiLayoutId.cpp
  ${REPO_ROOT}/lib/ThaiText/ThaiStats.cpp
)
target_include_directories(crosspoint_thai PUBLIC ${REPO_ROOT}/lib/ThaiText)
target_link_libraries(crosspoint_thai PRIVATE crosspoint_test_common)

add_library(crosspoint_shape STATIC
  ${REPO_ROOT}/lib/EpdFont/ThaiShape.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
)
target_include_directories(crosspoint_shape PUBLIC ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/miniz/src)
target_link_libraries(crosspoint_shape PUBLIC crosspoint_thai PRIVATE crosspoint_test_common)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(html_to_plain_text)
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(library_builder)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(progress_comparison)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(korean_line_breaking)
add_subdirectory(ligature_guard)
add_subdirectory(page_link)
add_subdirectory(content_opf_parser)
add_subdirectory(chapter_position)
add_subdirectory(home_button)
add_subdirectory(button_press_navigation)

add_subdirectory(inflate_stream)
add_subdirectory(fs_helpers)
add_subdirectory(txt_to_html)

add_subdirectory(absolute_grayscale)

add_subdirectory(sd_card_font)
add_subdirectory(glyph_bitmap)
add_subdirectory(thai_text)
add_subdirectory(thai_render)
add_subdirectory(thai_cache)
add_subdirectory(thai_ui)
