include(HostSupport.cmake)

add_executable(NativeTextTest
  NativeResourcesTest.cpp
  NativeThaiSegmenterTest.cpp
  NativeBidiTest.cpp
  NativeEngineTest.cpp
  NativeFontRuntimeTest.cpp
  NativeParagraphLayoutTest.cpp
  NativeRendererTest.cpp
  NativePayloadTest.cpp
  NativeParsedTextTest.cpp
  NativeEpubParserTest.cpp
  NativeFontFacadeTest.cpp
  NativeFontInstallerTest.cpp
  NativeFontManifestTest.cpp
  NativeFontWebApiTest.cpp
  NativeTxtPaginatorTest.cpp
  NativeTxtCacheTest.cpp
)
target_link_libraries(NativeTextTest PRIVATE
  GTest::gtest_main NativeHostSupport
)
target_include_directories(NativeTextTest PRIVATE "${REPO_ROOT}/src")
target_compile_definitions(NativeTextTest PRIVATE
  "NATIVE_DICTIONARY_PATH=\"${NATIVE_TEXT_ASSET_OUTPUT_DIR}/thbrk.tri\""
  "NATIVE_FONT_PATH=\"${REPO_ROOT}/lib/EpdFont/builtinFonts/source/NotoSansThai/NotoSansThai-Regular.ttf\""
  "NATIVE_SUBSET_FONT_PATH=\"${NATIVE_TEXT_ASSET_OUTPUT_DIR}/subsets/NotoSansThai-Regular.ttf\""
  "NATIVE_VARIABLE_FONT_PATH=\"${REPO_ROOT}/lib/NativeText/vendor/harfbuzz-14.5.0/test/api/fonts/Roboto-Variable.abc.ttf\""
)
add_dependencies(NativeTextTest NativeTextAssets)
if(MSVC)
  target_compile_options(NativeTextTest PRIVATE /utf-8)
endif()
gtest_discover_tests(NativeTextTest)

add_executable(NativeFontWebHost NativeFontWebHost.cpp)
target_link_libraries(NativeFontWebHost PRIVATE NativeHostSupport)


