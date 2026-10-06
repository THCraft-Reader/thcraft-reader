# Keep the renderer/parser/font closure identical to ThaiRenderProbe; only its
# entry point and unsupported ImageServices adapter are replaced here.
FetchContent_Declare(image_preparation_jpegdec
  GIT_REPOSITORY https://github.com/bitbank2/JPEGDEC.git
  GIT_TAG 86282979224c8a32fd51e091ed5a35b0c699a52b
)
# JPEGDEC's CMakeLists is ESP-IDF-only. Populate sources without configuring it.
FetchContent_GetProperties(image_preparation_jpegdec)
if(NOT image_preparation_jpegdec_POPULATED)
  cmake_policy(PUSH)
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(image_preparation_jpegdec)
  cmake_policy(POP)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(IMAGE_PREPARATION_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(IMAGE_PREPARATION_EPUB "${REPO_ROOT}/test/epubs/test_jpeg_images.epub")
add_custom_command(
  OUTPUT "${IMAGE_PREPARATION_FIXTURE_DIR}/grayscale.jpg"
         "${IMAGE_PREPARATION_FIXTURE_DIR}/grayscale.rawdeflate"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_fixture.py"
          "${IMAGE_PREPARATION_EPUB}" "${IMAGE_PREPARATION_FIXTURE_DIR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prepare_fixture.py" "${IMAGE_PREPARATION_EPUB}"
  VERBATIM
)
add_custom_target(image_preparation_fixtures DEPENDS
  "${IMAGE_PREPARATION_FIXTURE_DIR}/grayscale.jpg"
  "${IMAGE_PREPARATION_FIXTURE_DIR}/grayscale.rawdeflate"
)

add_executable(ImagePreparationTest
  ImagePreparationTest.cpp
  JpegOnlyFactory.cpp
  ${REPO_ROOT}/test/thai_render/HostAllocation.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/test/thai_render/UzlibChecksums.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${image_preparation_jpegdec_SOURCE_DIR}/src/JPEGDEC.cpp
)
target_include_directories(ImagePreparationTest PRIVATE
  ${REPO_ROOT}/test/thai_render/adapters
  ${REPO_ROOT}/test/fs_helpers/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/miniz/src
  ${image_preparation_jpegdec_SOURCE_DIR}/src
)
target_compile_definitions(ImagePreparationTest PRIVATE
  THAI_RENDER_PROBE=1 CROSSPOINT_PARSER_TEST=1 CROSSPOINT_VECTOR_FONTS=0
  IMAGE_PREPARATION_FIXTURE_DIR="${IMAGE_PREPARATION_FIXTURE_DIR}"
)
target_link_libraries(ImagePreparationTest PRIVATE crosspoint_test_common crosspoint_shape expat GTest::gtest)
add_dependencies(ImagePreparationTest image_preparation_fixtures)
set_target_properties(ImagePreparationTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/image_preparation/$<0:>"
)
gtest_discover_tests(ImagePreparationTest)
