# Developer Troubleshooting

## Pre-Run Checks

1. Turn on the power.
2. Ensure the sensor is connected to the correct pins.
   - Verify the pin assignments in the sensor's corresponding code file.
3. If using serial output or can bus, ensure the appropriate settings are configured in the `.ini` file.
   - Make sure to verify the right env [section]

---

## Encountered Issues

### Symptom: IDE does not detect the board

**Possible causes:**

- The USB-C cable provides power only (no data).
  - **Solution:** Replace it with a USB-C cable that supports data transfer.

- The esp32 or development board is damaged.
  - **Solution:** Verify the circuit wiring, make any necessary corrections, then test with a replacement board.
