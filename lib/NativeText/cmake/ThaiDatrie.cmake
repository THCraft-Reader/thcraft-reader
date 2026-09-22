include_guard(GLOBAL)

set(NATIVE_THAI_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/../vendor/libthai-0.1.30")
set(NATIVE_DATRIE_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/../vendor/libdatrie-0.2.14")
set(NATIVE_THAI_ALLOCATOR_HEADER "${CMAKE_CURRENT_LIST_DIR}/../port/NativeThaiAllocator.h")

# CMake and PlatformIO compile the same explicit vendor slices.
foreach(slice IN ITEMS datrie thai)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../port/${slice}-sources.txt" relative_sources)
    string(TOUPPER "${slice}" upper_slice)
    set(NATIVE_${upper_slice}_SOURCES "")
    foreach(source IN LISTS relative_sources)
        list(APPEND NATIVE_${upper_slice}_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../${source}")
    endforeach()
endforeach()
# The upstream root contains a file named "version", which shadows C++20 <version>.
set(NATIVE_DATRIE_PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/vendor-include")
file(GLOB datrie_headers CONFIGURE_DEPENDS "${NATIVE_DATRIE_VENDOR_DIR}/datrie/*.h")
foreach(header IN LISTS datrie_headers)
    get_filename_component(header_name "${header}" NAME)
    configure_file("${header}" "${NATIVE_DATRIE_PUBLIC_INCLUDE_DIRS}/datrie/${header_name}" COPYONLY)
endforeach()
set(NATIVE_THAI_PUBLIC_INCLUDE_DIRS "${NATIVE_THAI_VENDOR_DIR}/include")
set(NATIVE_THAI_PRIVATE_INCLUDE_DIRS "${NATIVE_THAI_VENDOR_DIR}/src")

# The pinned type headers are portable without an autoconf-generated config.
add_library(NativeDatrie STATIC ${NATIVE_DATRIE_SOURCES})
target_include_directories(NativeDatrie PUBLIC ${NATIVE_DATRIE_PUBLIC_INCLUDE_DIRS})

add_library(NativeThai STATIC ${NATIVE_THAI_SOURCES})
target_include_directories(NativeThai
    PUBLIC ${NATIVE_THAI_PUBLIC_INCLUDE_DIRS}
    PRIVATE ${NATIVE_THAI_PRIVATE_INCLUDE_DIRS}
)
target_link_libraries(NativeThai PUBLIC NativeDatrie)

# Host callers can load a generated dictionary by explicit path. Default search
# is available only on host builds and can be redirected to the generated .tri.
# Firmware integration must define NATIVE_TEXT_THAI_NO_DEFAULT_DICTIONARY=1.
option(NATIVE_TEXT_THAI_NO_DEFAULT_DICTIONARY
    "Disable LibThai's environment and default dictionary-file search"
    ${CMAKE_CROSSCOMPILING}
)
if(NATIVE_TEXT_THAI_NO_DEFAULT_DICTIONARY)
    target_compile_definitions(NativeThai PRIVATE NATIVE_TEXT_THAI_NO_DEFAULT_DICTIONARY=1)
else()
    set(NATIVE_TEXT_THAI_DICT_DIR "${NATIVE_THAI_VENDOR_DIR}/data" CACHE PATH
        "Host LibThai default dictionary directory")
    target_compile_definitions(NativeThai PRIVATE "DICT_DIR=\"${NATIVE_TEXT_THAI_DICT_DIR}\"")
endif()

foreach(native_thai_target IN ITEMS NativeDatrie NativeThai)
    set_target_properties(${native_thai_target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_include_directories(${native_thai_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
    if(MSVC)
        target_compile_options(${native_thai_target} PRIVATE "/FI${NATIVE_THAI_ALLOCATOR_HEADER}")
    else()
        target_compile_options(${native_thai_target} PRIVATE
            "SHELL:-include \"${NATIVE_THAI_ALLOCATOR_HEADER}\"")
    endif()
endforeach()
