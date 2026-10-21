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
