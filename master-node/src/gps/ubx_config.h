#pragma once
#include <stddef.h>
#include <stdint.h>

/* Builds the u-blox (UBX) binary commands that set the GPS update rate.
 *
 * The GY-GPS6MV2 carries a u-blox NEO-6M, which outputs 1 Hz NMEA by default.
 * The commands change only the receiver's RAM settings, so the master sends
 * them on every boot. Plain C with no ESP-IDF calls, so the host tests can
 * check the bytes directly. */

/* Measurement period: 200 ms is 5 Hz, the NEO-6M's maximum. */
#define UBX_MEASUREMENT_PERIOD_MS  200
/* Header, class, id, two length bytes, payload and two checksum bytes. */
#define UBX_FRAME_MAX              32
/* One CFG-MSG per NMEA sentence turned off, then CFG-RATE (sent last, and
 * only if every CFG-MSG was acknowledged). */
#define UBX_RATE_COMMAND_COUNT     6

/* Write the n-th command of the 5 Hz setup into frame (UBX_FRAME_MAX bytes).
 * Returns its length, or 0 once index is past the last command. */
size_t ubx_rate_command(unsigned index, uint8_t *frame);

/* Write the acknowledgement the receiver sends back for command (UBX-ACK-ACK).
 * Returns its length. */
size_t ubx_expected_ack(const uint8_t *command, uint8_t *ack);
