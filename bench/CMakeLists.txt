add_executable(queue_bench queue_bench.cpp)
target_link_libraries(queue_bench PRIVATE cq::cq cq_warnings benchmark::benchmark_main)

# One-iteration smoke run; CI's Release job runs it via ctest.
add_test(NAME bench_smoke COMMAND queue_bench --benchmark_min_time=1x)
