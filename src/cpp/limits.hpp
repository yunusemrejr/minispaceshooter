#ifndef MINI_LIMITS_HPP
#define MINI_LIMITS_HPP

#include <climits>
#include <cstdint>

namespace mss {

/* Persistent counters keep the v1 save format and saturate instead of wrapping.
 * Widen before arithmetic, including score bonuses at extreme levels. */
inline int counter(int64_t value)
{
    return value < 0 ? 0 : (value > INT_MAX ? INT_MAX : (int)value);
}

inline void increment(uint32_t &value)
{
    if (value < UINT32_MAX) ++value;
}

} // namespace mss
#endif
