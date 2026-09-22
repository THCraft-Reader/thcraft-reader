include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(EXPAT REQUIRED)

set(native_json_root "${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson")
if(NOT EXISTS "${native_json_root}/src/ArduinoJson.h")
  FetchContent_Declare(native_json
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
    SOURCE_SUBDIR crosspoint-host-no-cmake)
  FetchContent_MakeAvailable(native_json)
  set(native_json_root "${native_json_SOURCE_DIR}")
endif()

# Reuse the firmware's pinned image codecs, not decoder/renderer substitutes.
set(native_png_root "${REPO_ROOT}/.pio/libdeps/x4pro/PNGdec")
if(NOT EXISTS "${native_png_root}/src/PNGdec.cpp")
  FetchContent_Declare(native_png
    GIT_REPOSITORY https://github.com/bitbank2/PNGdec.git
    GIT_TAG 9a9c585fd39d148c5517597b02ce490e0fdb1bb4
    SOURCE_SUBDIR crosspoint-host-no-cmake)
  FetchContent_MakeAvailable(native_png)
  set(native_png_root "${native_png_SOURCE_DIR}")
endif()
set(native_jpeg_root "${REPO_ROOT}/.pio/libdeps/x4pro/JPEGDEC")
if(NOT EXISTS "${native_jpeg_root}/src/JPEGDEC.cpp")
  FetchContent_Declare(native_jpeg
    GIT_REPOSITORY https://github.com/bitbank2/JPEGDEC.git
    GIT_TAG 86282979224c8a32fd51e091ed5a35b0c699a52b
    SOURCE_SUBDIR crosspoint-host-no-cmake)
  FetchContent_MakeAvailable(native_jpeg)
  set(native_jpeg_root "${native_jpeg_SOURCE_DIR}")
endif()
add_library(NativeHostPng STATIC
  "${native_png_root}/src/PNGdec.cpp"
  "${native_png_root}/src/adler32.c"
  "${native_png_root}/src/crc32.c"
  "${native_png_root}/src/infback.c"
  "${native_png_root}/src/inffast.c"
  "${native_png_root}/src/inflate.c"
  "${native_png_root}/src/inftrees.c"
  "${native_png_root}/src/zutil.c"
)
target_include_directories(NativeHostPng PUBLIC "${native_png_root}/src")
target_compile_definitions(NativeHostPng PUBLIC __LINUX__=1 PNG_MAX_BUFFERED_PIXELS=16416)
add_library(NativeHostJpeg STATIC "${native_jpeg_root}/src/JPEGDEC.cpp")
target_include_directories(NativeHostJpeg PUBLIC "${native_jpeg_root}/src")
if(WIN32)
  # PNGdec's bundled inflate uses the Unix/Arduino spelling for unsigned int.
  target_compile_definitions(NativeHostPng PRIVATE uint=unsigned)
endif()

set(native_i18n_dir "${CMAKE_CURRENT_BINARY_DIR}/i18n")
file(GLOB native_translation_inputs CONFIGURE_DEPENDS "${REPO_ROOT}/lib/I18n/translations/*.yaml")
add_custom_command(
  OUTPUT "${native_i18n_dir}/I18nKeys.h" "${native_i18n_dir}/I18nStrings.h" "${native_i18n_dir}/I18nStrings.cpp"
  COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/scripts/gen_i18n.py"
    "${REPO_ROOT}/lib/I18n/translations" "${native_i18n_dir}"
  DEPENDS "${REPO_ROOT}/scripts/gen_i18n.py" ${native_translation_inputs}
  VERBATIM
)
add_library(NativeHostIo STATIC HostStorage.cpp HostMemory.cpp)
target_include_directories(NativeHostIo PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  "${REPO_ROOT}/lib/NativeText"
  "${REPO_ROOT}/lib/hal"
)
target_link_libraries(NativeHostIo PUBLIC NativeAllocator)
# Exercise the firmware FT_Stream/HalFile path against the real host SD adapter.
target_compile_definitions(NativeTextRuntime PRIVATE NATIVE_TEXT_HOST_STORAGE=1)
target_include_directories(NativeTextRuntime BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
target_link_libraries(NativeTextRuntime PUBLIC NativeHostIo)

add_library(NativeHostSupport STATIC
  HostDisplay.cpp
  HostFontSystem.cpp
  "${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp"
  "${REPO_ROOT}/lib/GfxRenderer/NativeTextRendering.cpp"
  "${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp"
  "${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp"
  "${REPO_ROOT}/lib/EpdFont/EpdFont.cpp"
  "${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp"
  "${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp"
  "${REPO_ROOT}/src/SdCardFontSystem.cpp"
  "${REPO_ROOT}/src/ReaderFontSizes.cpp"
  "${REPO_ROOT}/src/FontInstaller.cpp"
  "${REPO_ROOT}/src/FontManifest.cpp"
  "${REPO_ROOT}/src/network/FontWebApi.cpp"
  "${REPO_ROOT}/src/activities/settings/TextSettingsPreview.cpp"
  "${REPO_ROOT}/lib/Memory/BuildScratch.cpp"
  "${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp"
  "${REPO_ROOT}/lib/I18n/I18n.cpp"
  "${native_i18n_dir}/I18nStrings.cpp"
  "${REPO_ROOT}/lib/Epub/Epub.cpp"
  "${REPO_ROOT}/lib/Txt/Txt.cpp"
  "${REPO_ROOT}/lib/Txt/NativeTxtPaginator.cpp"
  "${REPO_ROOT}/lib/Txt/NativeTxtCache.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/Page.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/Section.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/converters/ImageDecoderFactory.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/converters/PngToFramebufferConverter.cpp"
  "${REPO_ROOT}/lib/ZipFile/ZipFile.cpp"
  "${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp"
  "${REPO_ROOT}/lib/JpegToBmpConverter/JpegToBmpConverter.cpp"
  "${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp"
  "${REPO_ROOT}/lib/miniz/src/InflateStream.cpp"
  "${REPO_ROOT}/lib/miniz/src/miniz_impl.c"
)
target_include_directories(NativeHostSupport BEFORE PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  "${native_i18n_dir}"
)
target_include_directories(NativeHostSupport PUBLIC
  "${REPO_ROOT}/src"
  "${native_json_root}/src"
  "${REPO_ROOT}/lib/GfxRenderer"
  "${REPO_ROOT}/lib/EpdFont"
  "${REPO_ROOT}/lib/Memory"
  "${REPO_ROOT}/lib/Utf8"
  "${REPO_ROOT}/lib/I18n"
  "${REPO_ROOT}/lib/Epub"
  "${REPO_ROOT}/lib/Txt"
  "${REPO_ROOT}/lib/Serialization"
  "${REPO_ROOT}/lib/XmlParserUtils"
  "${REPO_ROOT}/lib/ZipFile"
  "${REPO_ROOT}/lib/FsHelpers"
  "${REPO_ROOT}/lib/JpegToBmpConverter"
  "${REPO_ROOT}/lib/PngToBmpConverter"
  "${REPO_ROOT}/lib/miniz/src"
  "${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include"
)
target_compile_definitions(NativeHostSupport PUBLIC
  CROSSPOINT_NATIVE_TEXT=1 XML_STATIC XML_GE=0 XML_CONTEXT_BYTES=1024
)
target_link_libraries(NativeHostSupport PUBLIC
  NativeTextRuntime NativeHostIo NativeHostPng NativeHostJpeg EXPAT::EXPAT
)
if(MSVC)
  target_compile_options(NativeHostSupport PRIVATE /utf-8 /Gy)
else()
  target_compile_options(NativeHostSupport PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(NativeHostSupport INTERFACE -Wl,--gc-sections)
endif()
