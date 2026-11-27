// Internal admission/cancellation and motion-limit policies for drive control.
// Kept independent of the SDK so callers can check the same rules on the host.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

// A stop invalidates both the active operation and any queued command.
// Only a later explicitly submitted command may use the new generation.
struct OperationGate {
    std::atomic<unsigned> stop_epoch{0};
    std::atomic<unsigned> active_epoch{0};
    std::atomic<bool> busy{false};

    bool cancelled() const
    {
        return stop_epoch.load() != active_epoch.load();
    }
    bool admit()
    {
        bool expected = false;
        return busy.compare_exchange_strong(expected, true);
    }
    void stop()
    {
        busy.store(true);
        stop_epoch.fetch_add(1);
    }
};

struct MotionLimits {
    bool configured = false;
    float min_revs = 0;
    float max_revs = 0;
    float max_rpm = 0;
};

// Supervised single-step interface: mode 1 is the user's explicitly selected
// original state-only test. It preserves position/trajectory registers. Mode 3
// retains the zero target-velocity guard. Demand/actual speed are not enable
// prerequisites. Other operating modes are not enabled here.
inline bool direct_bench_enable_allowed(uint16_t status, uint8_t mode, uint8_t errors,
                                        uint32_t target_velocity)
{
    return (status & 0x006F) == 0x0023 && errors == 0 &&
           (mode == 1 || (mode == 3 && target_velocity == 0));
}

inline bool valid_limits(float lower, float upper, float rpm)
{
    // Serialization bounds, not a mechanical travel recommendation.
    return std::isfinite(lower) && std::isfinite(upper) && std::isfinite(rpm) && lower < upper &&
           lower >= -214748.0f && upper <= 214748.0f && rpm > 0 && rpm <= 12884900.0f;
}

inline bool within_limits(const MotionLimits &limits, float position, float rpm)
{
    return limits.configured && std::isfinite(position) && std::isfinite(rpm) &&
           position >= limits.min_revs && position <= limits.max_revs && rpm > 0 &&
           rpm <= limits.max_rpm;
}
