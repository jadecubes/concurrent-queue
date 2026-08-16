add_executable(queue_bench queue_bench.cpp)
target_link_libraries(queue_bench PRIVATE cq::cq cq_warnings benchmark::benchmark_main)

# One-iteration smoke run so CI (and `ctest`) picks up every future benchmark
# target without hardcoding binary paths.
add_test(NAME bench_smoke COMMAND queue_bench --benchmark_min_time=1x)
