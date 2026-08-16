add_executable(queue_tests
  mutex_queue_test.cpp
  stress_test.cpp)
target_link_libraries(queue_tests PRIVATE cq::cq cq_warnings cq_sanitizers GTest::gtest_main)

include(GoogleTest)
# PRE_TEST: discover at ctest time instead of running the (TSan-instrumented)
# binary on every build.
gtest_discover_tests(queue_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
