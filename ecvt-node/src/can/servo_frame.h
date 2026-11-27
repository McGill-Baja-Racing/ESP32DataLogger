#pragma once
#include <cstdint>

// Only the standalone drive's SDO/TPDO identifiers are recognized.
enum class ServoFrameKind { Ignore, Sdo, Status, Position, Velocity, Current };
inline ServoFrameKind servo_frame_kind(uint32_t id, uint8_t length)
{
    switch (id) {
    case 0x581: return length == 8 ? ServoFrameKind::Sdo : ServoFrameKind::Ignore;
    case 0x181: return length >= 2 ? ServoFrameKind::Status : ServoFrameKind::Ignore;
    case 0x281: return length >= 4 ? ServoFrameKind::Position : ServoFrameKind::Ignore;
    case 0x381: return length >= 4 ? ServoFrameKind::Velocity : ServoFrameKind::Ignore;
    case 0x481: return length >= 2 ? ServoFrameKind::Current : ServoFrameKind::Ignore;
    default: return ServoFrameKind::Ignore;
    }
}
inline uint32_t servo_frame_u32(const uint8_t *data)
{
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
           (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}
inline uint16_t servo_frame_u16(const uint8_t *data)
{
    return uint16_t(data[0]) | (uint16_t(data[1]) << 8);
}
