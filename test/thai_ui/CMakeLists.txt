add_executable(ThaiUiProbe
  ThaiUiProbe.cpp
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
target_include_directories(ThaiUiProbe PRIVATE
  ${REPO_ROOT}/test/thai_render/adapters
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
)
if(MSVC)
  target_include_directories(ThaiUiProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/adapters)
endif()
target_compile_definitions(ThaiUiProbe PRIVATE THAI_RENDER_PROBE=1 CROSSPOINT_VECTOR_FONTS=0)
target_link_libraries(ThaiUiProbe PRIVATE crosspoint_test_common crosspoint_shape)
set_target_properties(ThaiUiProbe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/thai_ui/$<0:>"
)
