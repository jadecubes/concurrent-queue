add_executable(queue_tests
  mutex_queue_test.cpp
  stress_test.cpp)
target_link_libraries(queue_tests PRIVATE cq::cq cq_warnings cq_sanitizers GTest::gtest_main)

include(GoogleTest)
# 60s: TSan-instrumented binaries take ~0.25s per process just to start.
gtest_discover_tests(queue_tests DISCOVERY_TIMEOUT 60)
