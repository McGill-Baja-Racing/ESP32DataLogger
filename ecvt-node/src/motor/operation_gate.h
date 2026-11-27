#pragma once
#include <atomic>

// A stop invalidates both the active operation and any queued command.
// Only a later explicitly submitted command may use the new generation.
struct OperationGate {
    std::atomic<unsigned> stop_epoch{0};
    std::atomic<unsigned> active_epoch{0};
    std::atomic<bool> busy{false};

    bool cancelled() const { return stop_epoch.load() != active_epoch.load(); }
    bool admit() { bool expected = false; return busy.compare_exchange_strong(expected, true); }
    void stop() { busy.store(true); stop_epoch.fetch_add(1); }
};
