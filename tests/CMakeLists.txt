add_executable(queue_tests
  mutex_queue_test.cpp
  stress_test.cpp)
target_link_libraries(queue_tests PRIVATE cq::cq cq_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(queue_tests DISCOVERY_TIMEOUT 60)
