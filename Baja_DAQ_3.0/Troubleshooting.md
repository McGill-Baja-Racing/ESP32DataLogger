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

---

# Hardware Troubleshooting

## Checks

1. Continuity Check.


## Encountered Issues

#### Symptom: Spark plug signal from inductor was occasionly missing sparks

**Possible causes:**
- Spark plug being wet (covered in black). See the engine manual provided by SAE outlining how to solve the issue.
   - **Solution:** Clean/ Replace spark plug. 

### Symptom: Wire inductor not picking up signal

**Possible causes:**
- Unknown
   - **Solution:** Replace wire with 16 gauge one.

### Symptoms: transceiver resistor not detected on CAN bus but continuity check passes.

**Possible causes:**
- Unknown
   - **Solution:** Remove current sodder and then resodder. (Do not ask me why this worked) 
