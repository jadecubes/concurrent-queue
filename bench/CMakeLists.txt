add_executable(queue_bench queue_bench.cpp)
target_link_libraries(queue_bench PRIVATE cq::cq cq_warnings benchmark::benchmark_main)
