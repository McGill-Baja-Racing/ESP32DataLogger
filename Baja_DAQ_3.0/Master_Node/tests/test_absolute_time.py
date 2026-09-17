"""Host checks: python3 -m unittest discover -s tests -p test_absolute_time.py"""
import ctypes
import datetime
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AbsoluteTimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        library = Path(cls.temp.name) / "gps.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-I", str(ROOT / "src"), str(ROOT / "src/time/gps_time.c"),
                        "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.gps_time_parse_rmc.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int64)]
        cls.lib.gps_time_parse_rmc.restype = ctypes.c_bool
        cls.lib.gps_time_sample_utc.argtypes = [ctypes.c_int64, ctypes.c_uint64, ctypes.c_uint32]
        cls.lib.gps_time_sample_utc.restype = ctypes.c_int64

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, time="184203.125", date="100926", status="A", talker="GP"):
        payload = f"{talker}RMC,{time},{status},,,,,,,{date},,,A"
        checksum = 0
        for c in payload.encode():
            checksum ^= c
        line = f"${payload}*{checksum:02X}".encode()
        result = ctypes.c_int64()
        valid = self.lib.gps_time_parse_rmc(line, ctypes.byref(result))
        return valid, result.value, line

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
        value = ctypes.c_int64()
        self.assertFalse(self.lib.gps_time_parse_rmc(line[:-2] + b"ZZ", ctypes.byref(value)))
        self.assertFalse(self.lib.gps_time_parse_rmc(b"$GPRMC*00", ctypes.byref(value)))

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
static size_t cursor;
static jmp_buf finished;
static int uart_read_bytes(int uart, uint8_t *out, unsigned n, unsigned timeout) {
    (void)uart; (void)n; (void)timeout;
    if (!input[cursor]) longjmp(finished, 1);
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
static void feed(const char *date, bool valid_date) {
    char payload[128], sentence[160];
    snprintf(payload, sizeof(payload), "GPRMC,184203.125,A,4807.038,N,01131.000,E,10.0,0.0,%s,,,A", date);
    unsigned checksum = 0;
    for (const char *p = payload; *p; ++p) checksum ^= (unsigned char)*p;
    snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", payload, checksum);
    input = sentence; cursor = 0; samples = 0; synchronized = false;
    expect_utc = valid_date; on_sample = capture;
    if (!setjmp(finished)) gps_task(NULL);
    assert(samples == 3);
}
int main(void) {
    (void)TAG; (void)gps_task_handle;
    feed("100926", true);
    /* Reject an invalid UTC date without losing valid speed/position data. */
    feed("100919", false);
    return 0;
}
"""
        test = Path(self.temp.name) / "gps_pipeline.c"
        test.write_text(harness)
        binary = test.with_suffix("")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src"), str(test), str(ROOT / "src/time/gps_time.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_csv_export_with_and_without_persisted_utc(self):
        source = (ROOT / "src/web/web_server.c").read_text()
        function = source[source.index("static esp_err_t stream_csv("):source.index("static esp_err_t download_handler(")]
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
    assert(stream_csv(NULL, bin, false, utc) == ESP_OK);
    assert(strstr(output, "timestamp_ms,absolute_time_utc,value"));
    assert(strstr(output, ",1000,,42,"));
    assert(strstr(output, ",1010,2026-09-10T18:42:03.125Z,43,"));
    assert(strstr(output, ",1020,,44,"));
    rewind(bin); rewind(utc); output[0] = 0;
    assert(stream_csv(NULL, bin, true, utc) == ESP_OK);
    assert(!strstr(output, "0x700"));
    assert(strstr(output, ",1010,2026-09-10T18:42:03.125Z,43,"));
    rewind(bin); output[0] = 0;
    assert(stream_csv(NULL, bin, false, NULL) == ESP_OK);
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
