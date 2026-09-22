# Pinned sources only: do not add the vendor CMake projects, which discover
# optional host dependencies and enable modules outside the firmware contract.
set(NATIVE_FTHB_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(NATIVE_FREETYPE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../vendor/freetype-2.14.3")
set(NATIVE_HARFBUZZ_ROOT "${CMAKE_CURRENT_LIST_DIR}/../vendor/harfbuzz-14.5.0")

# These NativeText-relative manifests are also consumed by the PlatformIO port.
# Each FreeType entry is a compilation unit from docs/INSTALL.ANY, not a glob.
file(STRINGS "${NATIVE_FTHB_ROOT}/port/freetype-sources.txt" NATIVE_FREETYPE_SOURCES)
file(STRINGS "${NATIVE_FTHB_ROOT}/port/harfbuzz-sources.txt" NATIVE_HARFBUZZ_SOURCES)
list(TRANSFORM NATIVE_FREETYPE_SOURCES PREPEND "${NATIVE_FTHB_ROOT}/")
list(TRANSFORM NATIVE_HARFBUZZ_SOURCES PREPEND "${NATIVE_FTHB_ROOT}/")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${NATIVE_FTHB_ROOT}/port/freetype-sources.txt"
  "${NATIVE_FTHB_ROOT}/port/harfbuzz-sources.txt"
)

# The ft2build.h overlay must precede the upstream include directory, including
# for consumers: it selects the same module/options/allocator configuration.
set(NATIVE_FREETYPE_INCLUDE_DIRS
  "${NATIVE_FTHB_ROOT}/port/freetype"
  "${NATIVE_FREETYPE_ROOT}/include"
  "${NATIVE_FTHB_ROOT}"
)
set(NATIVE_FREETYPE_DEFINITIONS FT2_BUILD_LIBRARY=1)
set(NATIVE_HARFBUZZ_INCLUDE_DIRS
  "${NATIVE_FTHB_ROOT}/port/harfbuzz"
  "${NATIVE_HARFBUZZ_ROOT}/src"
)
set(NATIVE_HARFBUZZ_DEFINITIONS HAVE_CONFIG_OVERRIDE_H=1)

add_library(NativeFreeType STATIC ${NATIVE_FREETYPE_SOURCES})
target_include_directories(NativeFreeType BEFORE PUBLIC ${NATIVE_FREETYPE_INCLUDE_DIRS})
target_compile_definitions(NativeFreeType PRIVATE ${NATIVE_FREETYPE_DEFINITIONS})
target_compile_features(NativeFreeType PRIVATE c_std_99)
set_target_properties(NativeFreeType PROPERTIES
  C_EXTENSIONS OFF
  DISABLE_PRECOMPILE_HEADERS ON
)

# Compile the release amalgamation exactly once; no individual hb-*.cc units.
add_library(NativeHarfBuzz STATIC ${NATIVE_HARFBUZZ_SOURCES})
target_include_directories(NativeHarfBuzz PUBLIC "${NATIVE_HARFBUZZ_ROOT}/src")
target_include_directories(NativeHarfBuzz PRIVATE "${NATIVE_FTHB_ROOT}/port/harfbuzz")
target_compile_definitions(NativeHarfBuzz PRIVATE ${NATIVE_HARFBUZZ_DEFINITIONS})
target_compile_features(NativeHarfBuzz PRIVATE cxx_std_11)
set_target_properties(NativeHarfBuzz PROPERTIES
  CXX_EXTENSIONS OFF
  DISABLE_PRECOMPILE_HEADERS ON
)
target_link_libraries(NativeHarfBuzz PUBLIC NativeFreeType)

# Leave HarfBuzz's real thread safety enabled on every host platform.
find_package(Threads REQUIRED)
target_link_libraries(NativeHarfBuzz PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(NativeHarfBuzz PUBLIC m)
endif()

if(MSVC)
  target_compile_definitions(NativeFreeType PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(NativeHarfBuzz PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  target_compile_options(NativeFreeType PRIVATE /utf-8)
  target_compile_options(NativeHarfBuzz PRIVATE /bigobj /utf-8)
elseif(MINGW)
  target_compile_options(NativeHarfBuzz PRIVATE -Wa,-mbig-obj)
endif()
