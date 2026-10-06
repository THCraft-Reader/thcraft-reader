add_executable(SdCardFontTest
  SdCardFontTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(SdCardFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(SdCardFontTest PRIVATE crosspoint_test_common crosspoint_shape GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(SdCardFontTest PRIVATE -fno-exceptions)
endif()
gtest_discover_tests(SdCardFontTest)

# The real system/manager/renderer lifecycle, with only platform persistence and
# hardware adapters. A paired CPSHAPE asset exercises both resident/cached modes.
add_executable(CoverDecodeScopeTest
  CoverDecodeScopeTest.cpp
  ${REPO_ROOT}/src/SdCardFontSystem.cpp
  ${REPO_ROOT}/src/ReaderFontSizes.cpp
  ${REPO_ROOT}/test/thai_render/HostAllocation.cpp
  ${REPO_ROOT}/test/thai_render/UzlibChecksums.c
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontManager.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
)
target_include_directories(CoverDecodeScopeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/lifecycle_adapters
  ${REPO_ROOT}/test/thai_render/adapters
  ${REPO_ROOT}/test/thai_render
  ${REPO_ROOT}/test/inflate_stream/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
)
target_compile_definitions(CoverDecodeScopeTest PRIVATE THAI_RENDER_PROBE=1 CROSSPOINT_VECTOR_FONTS=0)
target_link_libraries(CoverDecodeScopeTest PRIVATE crosspoint_test_common crosspoint_shape GTest::gtest)
set(THAI_SHAPE_ASSET_FONT "${REPO_ROOT}/build/thai/assets-shaped/cpfonts/noto-sans-thai-16.cpfont"
    CACHE FILEPATH "Prepared paired font used by real companion tests")
gtest_discover_tests(CoverDecodeScopeTest PROPERTIES ENVIRONMENT "THAI_SHAPE_TEST_FONT=${THAI_SHAPE_ASSET_FONT}")
