add_executable(queue_tests
  mutex_queue_test.cpp
  queue_contract_test.cpp
  spsc_queue_test.cpp
  stress_test.cpp
  try_operation_test.cpp)
target_link_libraries(queue_tests PRIVATE cq::cq cq_warnings cq_sanitizers GTest::gtest_main)
# The retry helper lives with the benchmarks that use it; the test reaches it.
target_include_directories(queue_tests PRIVATE ${PROJECT_SOURCE_DIR}/bench)

include(GoogleTest)
# PRE_TEST + a generous timeout: never run the (possibly TSan-instrumented)
# binary at build time, and allow for its slow (~0.25s) process startup.
gtest_discover_tests(queue_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
