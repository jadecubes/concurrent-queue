add_executable(queue_bench queue_bench.cpp)
target_link_libraries(queue_bench PRIVATE cq::cq cq_warnings benchmark::benchmark_main
  concurrentqueue TBB::tbb)

# One-iteration smoke run; CI's Release job runs it via ctest.
add_test(NAME bench_smoke COMMAND queue_bench --benchmark_min_time=1x)

# The shallowest non-blocking run must report non-zero retry pressure, or the
# diagnostic has gone dead. Kept separate from bench_smoke so that test still
# gates the process exit code (PASS_REGULAR_EXPRESSION overrides that check).
#
# [^\n] keeps the match inside the _mean row: CTest's `.` crosses newlines, so
# without it the pattern can start on _mean and satisfy its tail from a later
# row. Asserting non-zero rather than a magnitude keeps the gate off the
# scheduler's back.
add_test(NAME bench_retry_pressure
  COMMAND queue_bench
    "--benchmark_filter=^MutexQueue/try_throughput/capacity:1/"
    --benchmark_min_time=1000x
    --benchmark_repetitions=10)
set_tests_properties(bench_retry_pressure PROPERTIES
  PASS_REGULAR_EXPRESSION "_mean[^\n]*retries/op=[0-9.]*[1-9]")
