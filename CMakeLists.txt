cmake_minimum_required(VERSION 3.24)

project(concurrent_queue
  VERSION 0.1.0
  DESCRIPTION "Concurrent queues in C++20, from locked baseline to lock-free"
  LANGUAGES CXX)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(CQ_BUILD_TESTS "Build unit and stress tests" ON)
option(CQ_BUILD_BENCHMARKS "Build Google Benchmark targets" ON)

# Every build tree gets a compile_commands.json for clangd/clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Header-only library; consumers inherit C++20 via the compile feature and
# the thread requirement from the library that creates it.
find_package(Threads REQUIRED)
add_library(cq INTERFACE)
add_library(cq::cq ALIAS cq)
target_include_directories(cq INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(cq INTERFACE cxx_std_20)
target_link_libraries(cq INTERFACE Threads::Threads)

add_library(cq_warnings INTERFACE)
target_compile_options(cq_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wconversion>
  # Validates Doxygen comments against signatures (clang-only).
  $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wdocumentation>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>)

# Applied only to our own targets: instrumenting the FetchContent deps just
# slows the build, and TSan still intercepts their pthread-level sync.
add_library(cq_sanitizers INTERFACE)
if(ENABLE_TSAN)
  target_compile_options(cq_sanitizers INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fsanitize=thread -fno-omit-frame-pointer>)
  target_link_options(cq_sanitizers INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fsanitize=thread>)
endif()

include(FetchContent)
enable_testing()
# Project-wide: never execute a (possibly TSan-instrumented) test binary at
# build time just to enumerate tests.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

if(CQ_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_subdirectory(tests)
endif()

if(CQ_BUILD_BENCHMARKS AND NOT ENABLE_TSAN)
  FetchContent_Declare(
    benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  add_subdirectory(bench)
endif()
