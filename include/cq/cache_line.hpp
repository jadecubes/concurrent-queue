#ifndef CQ_CACHE_LINE_HPP_
#define CQ_CACHE_LINE_HPP_

#include <cstddef>

namespace cq {

// Pad members owned by different threads onto separate cache lines so a store
// on one side does not invalidate the other side's line (false sharing).
// A fixed constant rather than std::hardware_destructive_interference_size:
// that value moves with compiler version and tuning flags (GCC warns about
// any header use for exactly that reason), and 128 covers both x86-64
// adjacent-line prefetching and Apple/ARM64 hardware.
inline constexpr std::size_t kCacheLineSize = 128;

}  // namespace cq

#endif  // CQ_CACHE_LINE_HPP_
