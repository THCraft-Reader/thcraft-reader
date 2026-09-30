# Semantic layout tests deliberately reuse the fixed-metric parser adapters.
# Pixel/placement proof belongs to ThaiRenderProbe, not this target.
add_executable(ThaiTextTest
  ThaiTextTest.cpp
  ThaiClusterTest.cpp
  ThaiDictionaryTest.cpp
  ThaiSegmenterTest.cpp
  ThaiParserTest.cpp
  ThaiTokenTest.cpp
  ThaiLayoutTest.cpp
  ${REPO_ROOT}/test/chapter_html_slim_parser/ParserLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ThaiTextTest PRIVATE
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/ThaiText
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(ThaiTextTest PRIVATE crosspoint_test_common crosspoint_thai GTest::gtest_main expat)
target_compile_definitions(ThaiTextTest PRIVATE CROSSPOINT_PARSER_TEST=1)
gtest_discover_tests(ThaiTextTest)

add_executable(ThaiDictionaryBenchmark
  ThaiDictionaryBenchmark.cpp
)
target_include_directories(ThaiDictionaryBenchmark PRIVATE ${REPO_ROOT}/lib/ThaiText)
target_link_libraries(ThaiDictionaryBenchmark PRIVATE crosspoint_test_common crosspoint_thai)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME ThaiDictionaryGenerator
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_dictionary_generator.py)
