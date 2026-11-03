add_executable(ReadingStatsCoreTest
  ReadingStatsCoreTest.cpp
  ${REPO_ROOT}/src/util/ReadingStatsCore.cpp
)

target_include_directories(ReadingStatsCoreTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(ReadingStatsCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsCoreTest)

FetchContent_Declare(arduinojson GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git GIT_TAG v7.4.2
                     SOURCE_SUBDIR no-build)
FetchContent_MakeAvailable(arduinojson)

add_executable(ReadingStatsStoreTest
  ReadingStatsStoreTest.cpp
  ${REPO_ROOT}/src/ReadingStatsStore.cpp
  ${REPO_ROOT}/src/util/ReadingStatsCore.cpp
)
target_include_directories(ReadingStatsStoreTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/TrustedTime
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/minibidi_arabic/stubs
)
target_include_directories(ReadingStatsStoreTest SYSTEM PRIVATE ${arduinojson_SOURCE_DIR}/src)
if(MSVC)
  target_compile_options(ReadingStatsStoreTest PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/stubs/HostTime.h)
endif()
target_link_libraries(ReadingStatsStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReadingStatsStoreTest)
