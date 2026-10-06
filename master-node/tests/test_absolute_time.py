"""Host checks: python3 -m unittest discover -s tests -p test_absolute_time.py"""
import ctypes
import datetime
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class RmcFix(ctypes.Structure):
    _fields_ = [("utc_ms", ctypes.c_int64), ("latitude_e7", ctypes.c_int32),
                ("longitude_e7", ctypes.c_int32), ("speed_kph_x100", ctypes.c_uint16),
                ("has_location", ctypes.c_bool), ("has_speed", ctypes.c_bool),
                ("has_utc", ctypes.c_bool)]


def sentence(payload):
    checksum = 0
    for byte in payload.encode():
        checksum ^= byte
    return f"${payload}*{checksum:02X}".encode()


class AbsoluteTimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        library = Path(cls.temp.name) / "gps.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-I", str(ROOT / "src"), str(ROOT / "src/time/gps_time.c"), str(ROOT / "src/gps/rmc_parser.c"),
                        "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.gps_rmc_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(RmcFix)]
        cls.lib.gps_rmc_parse.restype = ctypes.c_bool
        cls.lib.gps_time_sample_utc.argtypes = [ctypes.c_int64, ctypes.c_uint64, ctypes.c_uint32]
        cls.lib.gps_time_sample_utc.restype = ctypes.c_int64

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, time="184203.125", date="100926", status="A", talker="GP"):
        payload = f"{talker}RMC,{time},{status},,,,,,,{date},,,A"
        line = sentence(payload)
        result = RmcFix()
        valid = self.lib.gps_rmc_parse(line, ctypes.byref(result))
        return valid and result.has_utc, result.utc_ms, line

    def test_utc_calendar_and_fraction(self):
        for time, date, expected in [
            ("184203.125", "100926", "2026-09-10T18:42:03.125+00:00"),
            ("000000", "290224", "2024-02-29T00:00:00+00:00"),
            ("235959.9", "311299", "2099-12-31T23:59:59.900+00:00"),
        ]:
            valid, value, _ = self.parse(time, date, talker="GN")
            self.assertTrue(valid)
            self.assertEqual(value, int(datetime.datetime.fromisoformat(expected).timestamp() * 1000))

    def test_invalid_dates_times_and_fix(self):
        for kwargs in [dict(date="290223"), dict(date="310426"), dict(date="000026"),
                       dict(date="100919"), dict(time="240000"), dict(time="120060"),
                       dict(time="120000."), dict(time="120000.1x"), dict(status="V"),
                       dict(time=""), dict(date="")]:
            self.assertFalse(self.parse(**kwargs)[0], kwargs)
        _, _, line = self.parse()
        value = RmcFix()
        self.assertFalse(self.lib.gps_rmc_parse(line[:-2] + b"ZZ", ctypes.byref(value)))
        self.assertFalse(self.lib.gps_rmc_parse(b"$GPRMC*00", ctypes.byref(value)))

    def telemetry(self, **changes):
        fields = dict(talker="GPRMC", time="184203.125", status="A",
                      lat="4807.038", ns="N", lon="01131.000", ew="E",
                      speed="10.0", course="0.0", date="100926")
        fields.update(changes)
        result = RmcFix()
        line = sentence(",".join(fields.values()) + ",,,A")
        valid = self.lib.gps_rmc_parse(line, ctypes.byref(result))
        return valid, result

    def test_valid_telemetry_and_boundaries(self):
        valid, fix = self.telemetry()
        self.assertTrue(valid and fix.has_location and fix.has_speed and fix.has_utc)
        self.assertEqual((fix.latitude_e7, fix.longitude_e7, fix.speed_kph_x100),
                         (481173000, 115166667, 1852))
        for lat, lon, ns, ew, expected in [
            ("0000.000", "00000.000", "N", "E", (0, 0)),
            ("9000.000", "18000.000", "S", "W", (-900000000, -1800000000)),
        ]:
            valid, fix = self.telemetry(lat=lat, lon=lon, ns=ns, ew=ew, speed="0")
            self.assertTrue(valid)
            self.assertEqual((fix.latitude_e7, fix.longitude_e7), expected)
            self.assertEqual(fix.speed_kph_x100, 0)
        valid, fix = self.telemetry(speed="0.005")
        self.assertTrue(valid)
        self.assertEqual(fix.speed_kph_x100, 1)

    def test_malformed_telemetry_is_rejected(self):
        cases = [dict(lat=v) for v in ["NaN", "inf", "4807.038x", "-4807.038",
                 "4860.0", "9000.01", "9100.0", "4807.", "407.0", "4.07", ""]]
        cases += [dict(lon=v) for v in ["18100.0", "18000.01", "01160.0", "1131.0", "11.31"]]
        cases += [dict(ns=v) for v in ["E", "NN", "", "n"]]
        cases += [dict(ew=v) for v in ["N", "WW", "", "w"]]
        cases += [dict(speed=v) for v in ["NaN", "inf", "-1", "+1", "1e2", "1x",
                  "1.", " 1", "400", "9" * 23, "1" * 24]]
        cases += [dict(status=v) for v in ["V", "AA", "a", ""]]
        cases += [dict(talker=v) for v in ["GPGGA", "XXXRMC", "1PRMC"]]
        for case in cases:
            valid, fix = self.telemetry(**case)
            self.assertFalse(valid, case)
            self.assertFalse(fix.has_location or fix.has_speed or fix.has_utc, case)

    def test_missing_telemetry_and_bad_utc_are_independent(self):
        valid, fix = self.telemetry(speed="")
        self.assertTrue(valid and fix.has_location and fix.has_utc)
        self.assertFalse(fix.has_speed)
        valid, fix = self.telemetry(lat="", ns="", lon="", ew="")
        self.assertTrue(valid and fix.has_speed and fix.has_utc)
        self.assertFalse(fix.has_location)
        valid, fix = self.telemetry(date="310426")
        self.assertTrue(valid and fix.has_location and fix.has_speed)
        self.assertFalse(fix.has_utc)

    def test_truncated_and_corrupt_sentences(self):
        valid = sentence("GPRMC,184203.125,A,4807.038,N,01131.000,E,10.0,0.0,100926,,,A")
        bad = [valid[:i] for i in range(len(valid))]
        bad += [valid + b"junk", valid[:-2] + b"ZZ", valid.replace(b"10.0", b"20.0"),
                sentence("GPRMC,184203.125,A,4807.038,N,01131.000,E,10.0"),
                sentence("GPRMC,184203.125,A,4807.038,N,01131.000,E,10.0,0.0,100926," + "A" * 24)]
        for line in bad:
            fix = RmcFix()
            fix.has_utc = fix.has_speed = fix.has_location = True
            self.assertFalse(self.lib.gps_rmc_parse(line, ctypes.byref(fix)), line)
            self.assertFalse(fix.has_utc or fix.has_speed or fix.has_location)

    def test_sample_mapping_and_wrap(self):
        convert = self.lib.gps_time_sample_utc
        self.assertEqual(convert(0, 1000, 900), 0)
        self.assertEqual(convert(1800000000000, 1000, 900), 1799999999900)
        self.assertEqual(convert(1800000000000, 2**32 + 20, 2**32 - 10), 1799999999970)
        self.assertEqual(convert(1800000000000, 2**32 - 10, 20), 1800000000030)


    def test_shared_reader_emits_gps_samples_and_synchronizes_utc(self):
        def without_includes(text):
            return "\n".join(line for line in text.splitlines() if not line.startswith("#include"))
        receiver = (ROOT / "src/gps/gps_receiver.c").read_text()
        receiver = receiver[:receiver.index("esp_err_t gps_receiver_start(")]
        clock = (ROOT / "src/time/absolute_clock.c").read_text()
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <setjmp.h>
#include "protocol/app_protocol.h"
#include "time/gps_time.h"
#include "gps/rmc_parser.h"
typedef struct { uint32_t id; uint8_t dlc; uint64_t data; } can_message_t;
typedef void (*gps_sample_handler_t)(const can_message_t *);
typedef int TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define ESP_LOGI(...) ((void)0)
#define UART_NUM_1 1
#define GPIO_NUM_33 33
#define GPIO_NUM_32 32
#define pdMS_TO_TICKS(x) (x)
static int64_t esp_timer_get_time(void) { return 1000000; }
static const char *input;
static size_t cursor, input_length;
static jmp_buf finished;
static int uart_read_bytes(int uart, uint8_t *out, unsigned n, unsigned timeout) {
    (void)uart; (void)n; (void)timeout;
    if (cursor == input_length) longjmp(finished, 1);
    *out = (uint8_t)input[cursor++]; return 1;
}
""" + without_includes(clock) + "\n" + without_includes(receiver) + r"""
static unsigned samples;
static bool expect_utc;
static void capture(const can_message_t *sample) {
    const uint32_t ids[] = {CAN_ID_GPS_SPEED, CAN_ID_GPS_LATITUDE, CAN_ID_GPS_LONGITUDE};
    assert(sample->id == ids[samples % 3]);
    assert((sample->data >> 32) == 1000);
    if (expect_utc) assert(absolute_clock_sample_utc(1000) == INT64_C(1789065723125));
    else assert(absolute_clock_sample_utc(1000) == 0);
    ++samples;
}
static void feed_bytes(const char *data, size_t length, unsigned expected_samples,
                       bool valid_date) {
    input = data; input_length = length; cursor = 0; samples = 0; synchronized = false;
    expect_utc = valid_date; on_sample = capture;
    if (!setjmp(finished)) gps_task(NULL);
    assert(samples == expected_samples);
    assert(synchronized == valid_date);
}
static void feed(const char *date, const char *speed, unsigned count, bool valid_date) {
    char payload[128], sentence[160];
    snprintf(payload, sizeof(payload), "GPRMC,184203.125,A,4807.038,N,01131.000,E,%s,0.0,%s,,,A", speed, date);
    unsigned checksum = 0;
    for (const char *p = payload; *p; ++p) checksum ^= (unsigned char)*p;
    snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", payload, checksum);
    feed_bytes(sentence, strlen(sentence), count, valid_date);
    if (valid_date) {
        /* Later observations cannot move the first anchor. */
        absolute_clock_observe_utc(INT64_C(1789069999999));
        assert(absolute_clock_sample_utc(1000) == INT64_C(1789065723125));
    }
    char overflow[512];
    memset(overflow, 'X', 200);
    memcpy(overflow + 200, sentence, strlen(sentence));
    feed_bytes(overflow, 200 + strlen(sentence), 0, false);
    /* An overlong line is discarded; the next complete line still works. */
    memcpy(overflow + 200 + strlen(sentence), sentence, strlen(sentence));
    feed_bytes(overflow, 200 + 2 * strlen(sentence), count, valid_date);
    /* An embedded NUL must not hide a malformed suffix from the parser. */
    size_t n = strlen(sentence) - 2;
    memcpy(overflow, sentence, n);
    memcpy(overflow + n, "\0junk\n", 6);
    feed_bytes(overflow, n + 6, 0, false);
}
int main(void) {
    (void)TAG; (void)gps_task_handle;
    feed("100926", "10.0", 3, true);
    feed("100926", "NaN", 0, false);
    feed("100926", "1\r0", 0, false);
    feed("100926", "400", 0, false);
    feed("100926", "", 0, true);
    /* Reject an invalid UTC date without losing valid speed/position data. */
    feed("100919", "10.0", 3, false);
    return 0;
}
"""
        test = Path(self.temp.name) / "gps_pipeline.c"
        test.write_text(harness)
        binary = test.with_suffix("")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src"), str(test), str(ROOT / "src/time/gps_time.c"), str(ROOT / "src/gps/rmc_parser.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_csv_export_with_and_without_persisted_utc(self):
        source = (ROOT / "src/web/web_server.c").read_text()
        helper_start = source.index("static int64_t read_utc_ms(")
        helper_end = source.index("static bool sample_at_or_before(", helper_start)
        function = (source[helper_start:helper_end] +
                    source[source.index("static esp_err_t stream_csv("):source.index("static esp_err_t download_handler(")])
        harness = r"""
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include "protocol/app_protocol.h"
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>
typedef int esp_err_t;
typedef int httpd_req_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct { const char *signal, *node, *units; } live_signal_metadata_t;
static const live_signal_metadata_t *metadata_for(uint32_t id) { (void)id; return NULL; }
static char output[4096];
static esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *data, size_t n) {
    (void)req; if (data) strncat(output, data, n); return ESP_OK;
}
""" + function + r"""
int main(void) {
    FILE *bin = tmpfile(), *utc = tmpfile(); assert(bin && utc);
    uint64_t records[][2] = {{0x700, ((uint64_t)900 << 32) | 1234},
                             {0xBB, ((uint64_t)1000 << 32) | 42},
                             {0xBB, ((uint64_t)1010 << 32) | 43},
                             {0xBB, ((uint64_t)1020 << 32) | 44}};
    int64_t times[] = {0, 0, INT64_C(1789065723125)};
    assert(fwrite(records, sizeof(records), 1, bin) == 1);
    assert(fwrite(times, sizeof(times), 1, utc) == 1);
    rewind(bin); rewind(utc);
    assert(stream_csv(NULL, bin, utc) == ESP_OK);
    assert(strstr(output, "timestamp_ms,absolute_time_utc,value"));
    assert(strstr(output, ",1000,,42,"));
    assert(strstr(output, ",1010,2026-09-10T18:42:03.125Z,43,"));
    assert(strstr(output, ",1020,,44,"));
    rewind(bin); output[0] = 0;
    assert(stream_csv(NULL, bin, NULL) == ESP_OK);
    assert(strstr(output, ",1010,,43,"));
    fclose(bin); fclose(utc); return 0;
}
"""
        test = Path(self.temp.name) / "csv_test.c"
        test.write_text(harness)
        binary = test.with_suffix("")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
