#include "try_operation.hpp"

#include <cstddef>

#include <gtest/gtest.h>

namespace cq::bench {
namespace {

TEST(TryOperation, CountsFailuresBeforeSuccess) {
  std::size_t attempts = 0;

  const auto failures = count_failures_until_success([&] {
    ++attempts;
    return attempts == 3;
  });

  EXPECT_EQ(attempts, 3U);
  EXPECT_EQ(failures, 2U);
}

}  // namespace
}  // namespace cq::bench
