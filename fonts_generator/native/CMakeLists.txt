cmake_minimum_required(VERSION 3.20)
project(font_generator_native LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Use python build.py to configure the pinned Emscripten toolchain")
endif()
if(NOT DEFINED FG_FREETYPE_SOURCE OR NOT DEFINED FG_HARFBUZZ_SOURCE)
    message(FATAL_ERROR "FG_FREETYPE_SOURCE and FG_HARFBUZZ_SOURCE are required")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
foreach(dependency ZLIB BZIP2 PNG BROTLI)
    set(FT_DISABLE_${dependency} ON CACHE BOOL "" FORCE)
endforeach()

# The upstream amalgamation includes the complete OpenType shapers and Unicode
# data, without pulling platform shapers, ICU, GLib, or FreeType integration in.
add_library(harfbuzz STATIC "${FG_HARFBUZZ_SOURCE}/src/harfbuzz.cc")
target_include_directories(harfbuzz PUBLIC "${FG_HARFBUZZ_SOURCE}/src")
target_compile_definitions(harfbuzz PRIVATE HB_NO_MT HB_NO_MMAP)
target_compile_options(harfbuzz PRIVATE -fno-exceptions -fno-rtti)

# FreeType's auto-hinter needs OpenType coverage for unencoded Thai alternates.
add_library(HarfBuzz::HarfBuzz ALIAS harfbuzz)
set(HarfBuzz_INCLUDE_DIR "${FG_HARFBUZZ_SOURCE}/src")
set(HarfBuzz_LIBRARY harfbuzz)
set(FT_DISABLE_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_HARFBUZZ ON CACHE BOOL "" FORCE)
add_subdirectory("${FG_FREETYPE_SOURCE}" freetype EXCLUDE_FROM_ALL)

add_library(font_engine STATIC font_engine.cpp)
target_include_directories(font_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(font_engine PRIVATE freetype harfbuzz)
target_compile_options(font_engine PRIVATE -fno-exceptions -fno-rtti)
