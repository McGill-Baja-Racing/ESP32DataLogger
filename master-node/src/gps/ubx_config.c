#include "gps/ubx_config.h"

#include <string.h>

#define UBX_CLASS_ACK      0x05
#define UBX_ID_ACK_ACK     0x01
#define UBX_CLASS_CFG      0x06
#define UBX_ID_CFG_MSG     0x01
#define UBX_ID_CFG_RATE    0x08
#define NMEA_CLASS         0xF0

/* Standard NMEA sentences the NEO-6M sends by default. Only RMC is parsed,
 * so the others are turned off: at 9600 baud all of them together take about
 * 500 ms to send and could not fit five times a second. */
static const uint8_t unused_nmea_ids[] = {
    0x00, /* GGA */
    0x01, /* GLL */
    0x02, /* GSA */
    0x03, /* GSV */
    0x05, /* VTG */
};

static size_t frame(uint8_t message_class, uint8_t id, const uint8_t *payload,
                    uint16_t length, uint8_t *out)
{
    out[0] = 0xB5;
    out[1] = 0x62;
    out[2] = message_class;
    out[3] = id;
    out[4] = (uint8_t)(length & 0xFF);
    out[5] = (uint8_t)(length >> 8);
    memcpy(out + 6, payload, length);
    /* 8-bit Fletcher checksum over class, id, length and payload. */
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < 6u + length; ++i) {
        a = (uint8_t)(a + out[i]);
        b = (uint8_t)(b + a);
    }
    out[6 + length] = a;
    out[7 + length] = b;
    return 8u + length;
}

size_t ubx_rate_command(unsigned index, uint8_t *out)
{
    size_t unused_count = sizeof(unused_nmea_ids);
    if (index < unused_count) {
        /* Short CFG-MSG form: rate 0 on the port this command arrives on. */
        const uint8_t payload[] = {NMEA_CLASS, unused_nmea_ids[index], 0};
        return frame(UBX_CLASS_CFG, UBX_ID_CFG_MSG, payload, sizeof(payload), out);
    }
    if (index == unused_count) {
        /* measRate in ms, one navigation solution per measurement, aligned
         * to GPS time (whole UTC seconds land on a measurement). */
        const uint8_t payload[] = {
            UBX_MEASUREMENT_PERIOD_MS & 0xFF, UBX_MEASUREMENT_PERIOD_MS >> 8,
            1, 0,
            1, 0,
        };
        return frame(UBX_CLASS_CFG, UBX_ID_CFG_RATE, payload, sizeof(payload), out);
    }
    return 0;
}

size_t ubx_expected_ack(const uint8_t *command, uint8_t *ack)
{
    const uint8_t acknowledged[] = {command[2], command[3]};
    return frame(UBX_CLASS_ACK, UBX_ID_ACK_ACK, acknowledged, sizeof(acknowledged), ack);
}
