add_executable(queue_tests
  mutex_queue_test.cpp
  stress_test.cpp)
target_link_libraries(queue_tests PRIVATE cq::cq cq_warnings cq_sanitizers GTest::gtest_main)

include(GoogleTest)
# Discovery mode is set project-wide (PRE_TEST) in the root CMakeLists.
gtest_discover_tests(queue_tests DISCOVERY_TIMEOUT 60)
