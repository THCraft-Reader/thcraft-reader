cmake_minimum_required(VERSION 3.16)
project(CrossPointNativeText LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(NativeAllocator STATIC NativeAllocator.cpp)
target_include_directories(NativeAllocator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT CMAKE_CROSSCOMPILING)
  add_library(NativeDictionaryAllocator STATIC NativeAllocator.cpp)
  target_include_directories(NativeDictionaryAllocator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(NativeDictionaryAllocator PRIVATE NATIVE_TEXT_DICTIONARY_BUILDER=1)
endif()

include(cmake/FreeTypeHarfBuzz.cmake)
include(cmake/ThaiDatrie.cmake)
include(cmake/ThaiDictionary.cmake)
include(cmake/FontAssets.cmake)
include(cmake/FontCatalogue.cmake)

add_library(NativeTextVendors INTERFACE)
target_link_libraries(NativeTextVendors INTERFACE
  NativeHarfBuzz NativeFreeType NativeThai NativeDatrie
)
add_custom_target(NativeTextLibraries
  DEPENDS NativeFreeType NativeHarfBuzz NativeThai NativeDatrie
)

add_library(NativeBidi STATIC ../MiniBidi/minibidi.c)
target_include_directories(NativeBidi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../MiniBidi")

add_library(NativeTextRuntime STATIC
  NativeTextEngine.cpp
  NativeFontRegistry.cpp
  NativePlatform.cpp
  NativeParagraphLayout.cpp
  ../Epub/Epub/hyphenation/Hyphenator.cpp
  ../Epub/Epub/hyphenation/HyphenationCommon.cpp
  ../Epub/Epub/hyphenation/LiangHyphenation.cpp
  ../Epub/Epub/hyphenation/LanguageRegistry.cpp
  ../Utf8/Utf8.cpp
  ThaiSegmenter.cpp
)
target_include_directories(NativeTextRuntime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(NativeTextRuntime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libthai-0.1.30/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Utf8"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Epub"
)
target_compile_definitions(NativeTextRuntime PUBLIC CROSSPOINT_NATIVE_TEXT=1)
target_link_libraries(NativeTextRuntime PUBLIC
  NativeHarfBuzz NativeFreeType NativeThai NativeDatrie NativeAllocator NativeBidi NativeTextAssetsData
)
if(MSVC)
  target_compile_options(NativeTextRuntime PRIVATE /utf-8)
  target_compile_options(NativeBidi PRIVATE /utf-8)
endif()
