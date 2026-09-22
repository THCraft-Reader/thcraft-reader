include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
  return()
endif()

set(NATIVE_FONT_SCRIPT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../EpdFont/scripts")
set(NATIVE_TEXT_BUILTIN_LANGS "all" CACHE STRING "Native font translation coverage")
file(GLOB_RECURSE native_font_inputs CONFIGURE_DEPENDS
  "${NATIVE_FONT_SCRIPT_ROOT}/../builtinFonts/source/*.ttf"
  "${NATIVE_FONT_SCRIPT_ROOT}/../builtinFonts/source/*.otf"
  "${CMAKE_CURRENT_LIST_DIR}/../../I18n/translations/*.yaml"
)
set(NATIVE_FONT_ASSET_HEADER "${NATIVE_TEXT_ASSET_OUTPUT_DIR}/NativeFontAssets.generated.h")
set(NATIVE_FONT_ASSET_SOURCE "${NATIVE_TEXT_ASSET_OUTPUT_DIR}/NativeFontAssets.generated.cpp")
set(NATIVE_FONT_ASSET_STAMP "${NATIVE_TEXT_ASSET_OUTPUT_DIR}/NativeFontAssets.stamp")
add_custom_command(
  OUTPUT "${NATIVE_FONT_ASSET_STAMP}"
  BYPRODUCTS "${NATIVE_FONT_ASSET_HEADER}" "${NATIVE_FONT_ASSET_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${NATIVE_FONT_SCRIPT_ROOT}/build-native-text-assets.py"
    --output "${NATIVE_TEXT_ASSET_OUTPUT_DIR}" --builtin-langs "${NATIVE_TEXT_BUILTIN_LANGS}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${NATIVE_FONT_ASSET_STAMP}"
  DEPENDS
    "${NATIVE_FONT_SCRIPT_ROOT}/build-native-text-assets.py"
    "${NATIVE_FONT_SCRIPT_ROOT}/native-text-assets.json"
    "${NATIVE_FONT_SCRIPT_ROOT}/font_ranges.py"
    "${CMAKE_CURRENT_LIST_DIR}/../../../scripts/gen_i18n.py"
    ${native_font_inputs}
  COMMENT "Subsetting and embedding pinned native OpenType fonts"
  VERBATIM
)
add_custom_target(NativeFontAssets DEPENDS "${NATIVE_FONT_ASSET_STAMP}")
add_custom_target(NativeTextAssets DEPENDS NativeFontAssets NativeThaiDictionaryAssets)
add_library(NativeTextAssetsData STATIC EXCLUDE_FROM_ALL
  "${NATIVE_FONT_ASSET_SOURCE}" "${NATIVE_THAI_DICTIONARY_SOURCE}"
)
add_dependencies(NativeTextAssetsData NativeTextAssets)
target_include_directories(NativeTextAssetsData PUBLIC "${NATIVE_TEXT_ASSET_OUTPUT_DIR}")
