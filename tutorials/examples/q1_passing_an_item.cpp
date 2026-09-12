// Independent, defined-behaviour witnesses for tutorial Q1. Never execute UB.
#include <cq/mutex_queue.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// tests/queue_test_util.hpp's run_blocked pattern, without a GoogleTest dependency.
// The timeout observes non-completion, not proof that the OS entered a CV wait.
// Unblock and join before asserting so failure cannot destroy a live queue.
template <typename Blocked, typename Unblock>
bool run_blocked(Blocked blocked_op, Unblock unblock) {
  std::packaged_task<bool()> task(std::move(blocked_op));
  auto pending = task.get_future();
  std::thread worker(std::move(task));
  constexpr auto kSettleTime = std::chrono::milliseconds(5);
  const bool still_pending = pending.wait_for(kSettleTime) == std::future_status::timeout;
  const bool unblocked = unblock();
  worker.join();
  require(still_pending, "operation returned before capacity was freed");
  require(unblocked, "unblocking operation failed");
  return pending.get();
}

void full_waits_until_pop() {
  cq::MutexQueue<int> queue(2);
  require(queue.push(1), "push 1");
  require(queue.push(2), "push 2");
  int out = 0;
  require(run_blocked([&] { return queue.push(3); }, [&] { return queue.pop(out) && out == 1; }),
          "pending push 3 completes");
  require(queue.pop(out) && out == 2, "FIFO job 2");
  require(queue.pop(out) && out == 3, "pending job 3 reuses freed slot");
  queue.close();
}

void close_drains_in_order() {
  cq::MutexQueue<int> queue(2);
  require(queue.push(1) && queue.push(2), "fill queue");
  queue.close();
  queue.close();
  require(!queue.push(3), "close refuses producer");
  int out = 0;
  require(queue.pop(out) && out == 1, "close preserves job 1");
  require(queue.pop(out) && out == 2, "close preserves job 2");
  require(!queue.pop(out) && out == 2, "closed and drained leaves out intact");
}

void failed_push_owns_its_parameter() {
  cq::MutexQueue<std::unique_ptr<int>> queue(2);
  require(queue.push(std::make_unique<int>(1)), "fill slot 0");
  require(queue.push(std::make_unique<int>(2)), "fill slot 1");
  auto candidate = std::make_unique<int>(3);
  require(!queue.try_push(std::move(candidate)), "full try_push returns false");
  // NOLINTNEXTLINE(bugprone-use-after-move) -- observing the documented moved-from state.
  require(candidate == nullptr, "failed try_push consumed the rvalue");
  queue.close();

  cq::MutexQueue<std::string> copies(2);
  require(copies.push("one") && copies.push("two"), "fill copy queue");
  const std::string original = "still owned by caller";
  require(!copies.try_push(original), "full lvalue try_push returns false");
  require(original == "still owned by caller", "failed try_push preserves the lvalue");
  copies.close();
}
}  // namespace

int main() {
  try {
    full_waits_until_pop();
    close_drains_in_order();
    failed_push_owns_its_parameter();
    std::cout << "Q1: full wait, close/drain, and argument ownership witnessed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
