#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

#include <glog/logging.h>

namespace utils {

static_assert(
    std::is_same_v<uint64_t, unsigned long>,
    "utils::Log requires uint64_t to be implemented as unsigned long.");

/// This function computes the log2 function on integer types. It is faster than
/// the cmath `log2` function because it doesn't use floating point values for
/// calculation.
constexpr inline uint64_t Log2(uint64_t val) {
  // The `clz` function is undefined when the passed value is 0 and the value of
  // `log` is `-inf` so we special case it here.
  if (val == 0) return 0;
  // clzl = count leading zeros from long
  //        ^     ^       ^          ^
  int ret = __builtin_clzl(val);
  return 64UL - static_cast<uint64_t>(ret) - 1UL;
}

/// Return `true` if `val` is a power of 2.
constexpr inline bool IsPow2(uint64_t val) noexcept {
  return val != 0ULL && (val & (val - 1ULL)) == 0ULL;
}

/// Return `val` if it is power of 2, otherwise get the next power of 2 value.
/// If `val` is sufficiently large, the next power of 2 value may not fit into
/// the result type and you will get a wrapped value to 1ULL.
constexpr inline uint64_t Ceil2(uint64_t val) noexcept {
  if (val == 0ULL || val == 1ULL) return 1ULL;
  return 1ULL << (Log2(val - 1ULL) + 1ULL);
}

/// Round `val` to the next `multiple` value, if needed.
/// `std::nullopt` is returned in case of an overflow or if `multiple` is 0.
///
/// Examples:
///
///     RoundUint64ToMultiple(5, 8) == 8
///     RoundUint64ToMultiple(8, 8) == 8
///     RoundUint64ToMultiple(9, 8) == 16
constexpr inline std::optional<uint64_t> RoundUint64ToMultiple(
    uint64_t val, uint64_t multiple) noexcept {
  if (multiple == 0) return std::nullopt;
  uint64_t numerator = val + multiple - 1;
  // Check for overflow.
  if (numerator < val) return std::nullopt;
  // Rely on integer division to get the rounded multiple.
  // No overflow is possible as the final, rounded value can only be less than
  // or equal to `numerator`.
  return (numerator / multiple) * multiple;
}

}  // namespace utils
