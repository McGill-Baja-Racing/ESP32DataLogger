#!/usr/bin/env python3
import argparse
import csv
import struct
from pathlib import Path

LEGACY_REC = struct.Struct("<IIII")
REC = struct.Struct("<IIiiiii4i")  # raw record from src/main.cpp
REC_SIZE = REC.size                 # 44 bytes
LEGACY_REC_SIZE = LEGACY_REC.size   # 16 bytes
RATED_CURRENT_A = 5.2
CURRENT_SCALE_A_PER_UNIT = RATED_CURRENT_A * 0.001  # 1 unit = 0.1% of rated current
POSITION_SCALE_REV_PER_UNIT = 1.0 / 10000.0
TIMESTAMP_SCALE_MS_PER_US = 1.0 / 1000.0
RPM_CENTI_SCALE = 1.0 / 100.0

LOG_RECORD_TYPE_SERVO_TPDO2 = 2
LOG_RECORD_TYPE_SERVO_TPDO3 = 3
LOG_RECORD_TYPE_SERVO_TPDO4 = 4
LOG_RECORD_TYPE_SENSOR_THROTTLE = 10
LOG_RECORD_TYPE_SENSOR_ENGINE_RPM = 11
LOG_RECORD_TYPE_SENSOR_WHEEL_RPM = 12
LOG_RECORD_TYPE_SENSOR_BRAKE_STATE = 13
LOG_RECORD_TYPE_CONTROLLER_COMMAND = 100

def decode_p4_log(bin_path: Path, csv_path: Path) -> None:
    data = bin_path.read_bytes()
    n = len(data)
    use_legacy_format = (n % REC_SIZE) != 0 and (n % LEGACY_REC_SIZE) == 0
    rec = LEGACY_REC if use_legacy_format else REC
    rec_size = LEGACY_REC_SIZE if use_legacy_format else REC_SIZE
    usable = n - (n % rec_size)
    if usable != n:
        print(f"Warning: ignoring {n - usable} trailing bytes (incomplete record).")

    last_vel = None
    last_pos_rev = None
    last_cur_a = None

    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "timestamp_ms",
            "record_type",
            "sensor_value",
            "feedback_position_rev",
            "feedback_velocity",
            "feedback_current_a",
            "command_position_rev",
            "command_speed_rev_s",
            "calc_throttle_pct",
            "calc_engine_rpm",
            "calc_wheel_rpm",
            "calc_target_engine_rpm",
            "calc_brake_state",
        ])

        for off in range(0, usable, rec_size):
            if use_legacy_format:
                t_us, legacy_val1, _legacy_val2, record_type = rec.unpack_from(data, off)
                primary_value = legacy_val1
                command_position_counts = 0
                command_speed_counts = 0
                throttle_pct = 0
                engine_rpm_centi = 0
                wheel_rpm_centi = 0
                target_engine_rpm_centi = 0
                brake_state = 0
            else:
                (
                    t_us,
                    record_type,
                    primary_value,
                    _secondary_value,
                    command_position_counts,
                    command_speed_counts,
                    throttle_pct,
                    engine_rpm_centi,
                    wheel_rpm_centi,
                    target_engine_rpm_centi,
                    brake_state,
                ) = rec.unpack_from(data, off)

            row_type = "unknown"
            command_position_rev = ""
            command_speed_rev_s = ""
            calc_throttle_pct = ""
            calc_engine_rpm = ""
            calc_wheel_rpm = ""
            calc_target_engine_rpm = ""
            calc_brake_state = ""
            sensor_value = ""

            if record_type == LOG_RECORD_TYPE_SERVO_TPDO2:
                row_type = "servo_tpdo2_position"
                last_pos_rev = primary_value * POSITION_SCALE_REV_PER_UNIT
            elif record_type == LOG_RECORD_TYPE_SERVO_TPDO3:
                row_type = "servo_tpdo3_velocity"
                last_vel = primary_value
            elif record_type == LOG_RECORD_TYPE_SERVO_TPDO4:
                row_type = "servo_tpdo4_current"
                last_cur_a = primary_value * CURRENT_SCALE_A_PER_UNIT
            elif record_type == LOG_RECORD_TYPE_SENSOR_THROTTLE:
                row_type = "sensor_throttle"
                sensor_value = primary_value
                calc_throttle_pct = throttle_pct
                calc_engine_rpm = engine_rpm_centi * RPM_CENTI_SCALE
                calc_wheel_rpm = wheel_rpm_centi * RPM_CENTI_SCALE
                calc_target_engine_rpm = target_engine_rpm_centi * RPM_CENTI_SCALE
                calc_brake_state = brake_state
            elif record_type == LOG_RECORD_TYPE_SENSOR_ENGINE_RPM:
                row_type = "sensor_engine_rpm"
                sensor_value = primary_value * RPM_CENTI_SCALE
                calc_throttle_pct = throttle_pct
                calc_engine_rpm = engine_rpm_centi * RPM_CENTI_SCALE
                calc_wheel_rpm = wheel_rpm_centi * RPM_CENTI_SCALE
                calc_target_engine_rpm = target_engine_rpm_centi * RPM_CENTI_SCALE
                calc_brake_state = brake_state
            elif record_type == LOG_RECORD_TYPE_SENSOR_WHEEL_RPM:
                row_type = "sensor_wheel_rpm"
                sensor_value = primary_value * RPM_CENTI_SCALE
                calc_throttle_pct = throttle_pct
                calc_engine_rpm = engine_rpm_centi * RPM_CENTI_SCALE
                calc_wheel_rpm = wheel_rpm_centi * RPM_CENTI_SCALE
                calc_target_engine_rpm = target_engine_rpm_centi * RPM_CENTI_SCALE
                calc_brake_state = brake_state
            elif record_type == LOG_RECORD_TYPE_SENSOR_BRAKE_STATE:
                row_type = "sensor_brake_state"
                sensor_value = primary_value
                calc_throttle_pct = throttle_pct
                calc_engine_rpm = engine_rpm_centi * RPM_CENTI_SCALE
                calc_wheel_rpm = wheel_rpm_centi * RPM_CENTI_SCALE
                calc_target_engine_rpm = target_engine_rpm_centi * RPM_CENTI_SCALE
                calc_brake_state = brake_state
            elif record_type == LOG_RECORD_TYPE_CONTROLLER_COMMAND:
                row_type = "controller_command"
                command_position_rev = command_position_counts * POSITION_SCALE_REV_PER_UNIT
                command_speed_rev_s = command_speed_counts * 60.0 * POSITION_SCALE_REV_PER_UNIT
                calc_throttle_pct = throttle_pct
                calc_engine_rpm = engine_rpm_centi * RPM_CENTI_SCALE
                calc_wheel_rpm = wheel_rpm_centi * RPM_CENTI_SCALE
                calc_target_engine_rpm = target_engine_rpm_centi * RPM_CENTI_SCALE
                calc_brake_state = brake_state

            w.writerow([
                t_us * TIMESTAMP_SCALE_MS_PER_US,
                row_type,
                sensor_value,
                "" if last_pos_rev is None else last_pos_rev,
                "" if last_vel is None else last_vel,
                "" if last_cur_a is None else last_cur_a,
                command_position_rev,
                command_speed_rev_s,
                calc_throttle_pct,
                calc_engine_rpm,
                calc_wheel_rpm,
                calc_target_engine_rpm,
                calc_brake_state,
            ])

    print(f"Wrote: {csv_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Decode logged ESP32 ECVT binary data.")
    parser.add_argument("bin_path", nargs="?", default="./Data/LOG_0783.BIN", help="Input BIN file")
    parser.add_argument("csv_path", nargs="?", default="./Data/LOG_0783.csv", help="Output CSV file")
    args = parser.parse_args()
    decode_p4_log(Path(args.bin_path), Path(args.csv_path))
