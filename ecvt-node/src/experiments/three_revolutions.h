#pragma once
#include <cstdint>
#include <cmath>
#include <limits>

inline bool hold_demand_matches(uint32_t requested, uint32_t demanded)
{
    const int64_t difference = static_cast<int64_t>(static_cast<int32_t>(requested)) -
                               static_cast<int64_t>(static_cast<int32_t>(demanded));
    return difference >= -1 && difference <= 1;
}

// DSY manual section 5.3: motor rev = command units * 6091:01 / 6091:02.
// Keep the existing driver conversion; refuse any other configured scaling.
inline bool three_revolution_target(uint32_t motor_units, uint32_t command_units,
                                    int32_t start_counts, float &start, float &target)
{
    if (!motor_units || static_cast<uint64_t>(motor_units) * 10000 != command_units) return false;
    const int64_t end_counts = static_cast<int64_t>(start_counts) + 30000;
    if (end_counts > std::numeric_limits<int32_t>::max() - 1 ||
        start_counts < std::numeric_limits<int32_t>::min() + 1) return false;
    start = static_cast<float>(start_counts) / 10000.0f;
    target = static_cast<float>(end_counts) / 10000.0f;
    // The legacy driver serializes with float multiply; don't hide precision loss.
    return std::llround(start * 10000.0f) == start_counts &&
           std::llround(target * 10000.0f) == end_counts && target > start;
}
