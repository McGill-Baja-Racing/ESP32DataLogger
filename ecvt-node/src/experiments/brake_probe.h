#pragma once
#include "esp_err.h"

// Unloaded bench test: verified zero-velocity mode, 0006 -> 0007 -> 000F,
// ten-second enabled window, then 0000. Cleanup retries disable, never output bits.
// Success confirms drive state, never standstill or physical brake engagement.
esp_err_t brake_probe_run();
// Always sends disable, even if no previous probe is remembered.
esp_err_t brake_probe_restore();
bool brake_probe_restore_pending();
