add_executable(queue_tests
  mutex_queue_test.cpp
  mpmc_queue_test.cpp
  spsc_queue_test.cpp
  stress_test.cpp)
target_link_libraries(queue_tests PRIVATE cq::cq cq_warnings cq_sanitizers GTest::gtest_main)

include(GoogleTest)
# PRE_TEST + a generous timeout: never run the (possibly TSan-instrumented)
# binary at build time, and allow for its slow (~0.25s) process startup.
gtest_discover_tests(queue_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
