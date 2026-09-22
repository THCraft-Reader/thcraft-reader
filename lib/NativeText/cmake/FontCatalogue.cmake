include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
  return()
endif()

set(native_catalogue_script "${CMAKE_CURRENT_LIST_DIR}/../../EpdFont/scripts/build-native-font-catalogue.py")
set(native_catalogue_root "${CMAKE_CURRENT_LIST_DIR}/../../EpdFont/scripts")
set(native_catalogue_stamp "${NATIVE_TEXT_ASSET_OUTPUT_DIR}/NativeFontCatalogue.stamp")
set(native_catalogue_header "${NATIVE_TEXT_ASSET_OUTPUT_DIR}/NativeFontCatalogue.generated.h")
set(native_catalogue_source "${NATIVE_TEXT_ASSET_OUTPUT_DIR}/NativeFontCatalogue.generated.cpp")
add_custom_command(
  OUTPUT "${native_catalogue_stamp}"
  BYPRODUCTS "${native_catalogue_header}" "${native_catalogue_source}"
  COMMAND "${Python3_EXECUTABLE}" "${native_catalogue_script}" --output "${NATIVE_TEXT_ASSET_OUTPUT_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${native_catalogue_stamp}"
  DEPENDS
    "${native_catalogue_script}"
    "${native_catalogue_root}/font_sources.py"
    "${native_catalogue_root}/sd-fonts.yaml"
    "${native_catalogue_root}/native-font-sources.lock.json"
    "${native_catalogue_root}/native-font-catalogue.json"
  COMMENT "Embedding the pinned native font catalogue without network access"
  VERBATIM
)
add_custom_target(NativeFontCatalogueAssets DEPENDS "${native_catalogue_stamp}")
add_dependencies(NativeTextAssets NativeFontCatalogueAssets)
target_sources(NativeTextAssetsData PRIVATE "${native_catalogue_source}")
