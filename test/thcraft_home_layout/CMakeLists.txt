add_executable(ThcraftHomeLayoutTest
  ThcraftHomeLayoutTest.cpp
  ${REPO_ROOT}/src/components/themes/thcraft/ThcraftHomeLayout.cpp
)

target_include_directories(ThcraftHomeLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ThcraftHomeLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ThcraftHomeLayoutTest)
