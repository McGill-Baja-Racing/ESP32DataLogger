"""Host checks: python3 -m unittest discover -s tests -p test_logger_write.py"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# The shim mimics FatFs: a failed write breaks the file until it is closed; a failed sync does not.
PREFIX = r"""
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define xSemaphoreTake(mutex, ticks) ((void)(mutex))
#define xSemaphoreGive(mutex) ((void)(mutex))
static const char *TAG = "test";
static int file_mutex;
static char log_path[256], utc_path[264];
static int log_fd = -1, utc_fd = -1;
static uint32_t records_saved;
static atomic_uint write_drops;
enum { HEALTH_SD_WRITE = 1, HEALTH_SAMPLES_DROPPED = 2 };
#define ESP_FAIL (-1)
static bool sd_fault;
static unsigned sd_onsets, samples_dropped;
static void node_health_set(int code, bool active)
{
    if (code != HEALTH_SD_WRITE) return;
    if (active && !sd_fault) sd_onsets++;
    sd_fault = active;
}
static void node_health_count(int code, uint32_t amount, uint32_t subject, int error)
{
    (void)subject;
    if (code == HEALTH_SAMPLES_DROPPED && error == ESP_FAIL) samples_dropped += amount;
}
static char fault;
static int failures, broken_fd = -1;
static ssize_t faulty_write(int fd, const void *data, size_t size)
{
    if (fd == broken_fd) { errno = EIO; return -1; }
    if (fault == 'w' && fd == log_fd && failures > 0) {
        failures--;
        broken_fd = fd;
        return write(fd, data, size / 2 + 3);
    }
    return write(fd, data, size);
}
static int faulty_fsync(int fd)
{
    if (fd == broken_fd) { errno = EIO; return -1; }
    if (fault == 's' && fd == utc_fd && failures > 0) { failures--; errno = EIO; return -1; }
    return fsync(fd);
}
static int faulty_close(int fd)
{
    if (fd == broken_fd) broken_fd = -1;
    return close(fd);
}
#define write faulty_write
#define fsync faulty_fsync
#define close faulty_close
"""

MAIN = r"""
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    snprintf(log_path, sizeof(log_path), "%s", argv[1]);
    snprintf(utc_path, sizeof(utc_path), "%s", argv[2]);
    fault = argv[3][0];
    failures = atoi(argv[4]);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0666);
    utc_fd = open(utc_path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0666);
    for (int block = 0; block < 3; block++) {
        int64_t records[4][2], utc[4];
        for (int i = 0; i < 4; i++) {
            records[i][0] = block * 4 + i;
            records[i][1] = 0;
            utc[i] = 1000 + block * 4 + i;
        }
        save_records((const int64_t (*)[2])records, utc, 4);
    }
    close_files();
    printf("%u %u %u %d\n", atomic_load(&write_drops), samples_dropped, sd_onsets, sd_fault);
    return 0;
}
"""


class LoggerWriteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        source = (ROOT / "src/logger/data_logger.c").read_text()
        start = source.index("static void close_files(void)")
        cut = source[start:source.index("static void writer_task(void *argument)", start)]
        program = Path(cls.temp.name) / "writer.c"
        program.write_text(PREFIX + cut + MAIN)
        cls.binary = Path(cls.temp.name) / "writer"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(program),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, fault, failures):
        work = Path(tempfile.mkdtemp(dir=self.temp.name))
        log, utc = work / "log.bin", work / "log.bin.utc"
        result = subprocess.run([str(self.binary), str(log), str(utc), fault, str(failures)],
                                capture_output=True, text=True, check=True)
        data, stamps = log.read_bytes(), utc.read_bytes()
        ids = [struct.unpack_from("<q", data, i)[0] for i in range(0, len(data) - 15, 16)]
        return (len(data) % 16, ids, list(struct.unpack(f"<{len(stamps) // 8}q", stamps)),
                [int(value) for value in result.stdout.split()])

    def test_clean_blocks_are_saved_in_order(self):
        self.assertEqual(self.run_case("w", 0), (0, list(range(12)), list(range(1000, 1012)), [0, 0, 0, 0]))

    def test_failed_write_is_retried_once_without_duplicates(self):
        self.assertEqual(self.run_case("w", 1), (0, list(range(12)), list(range(1000, 1012)), [0, 0, 1, 0]))

    def test_block_failing_write_twice_is_cut_and_reported(self):
        self.assertEqual(self.run_case("w", 2), (0, list(range(4, 12)), list(range(1004, 1012)), [4, 4, 1, 0]))

    def test_failed_sync_is_retried_once(self):
        self.assertEqual(self.run_case("s", 1), (0, list(range(12)), list(range(1000, 1012)), [0, 0, 1, 0]))

    def test_block_failing_sync_twice_leaves_no_utc_behind(self):
        self.assertEqual(self.run_case("s", 2), (0, list(range(4, 12)), list(range(1004, 1012)), [4, 4, 1, 0]))

    def test_card_that_keeps_failing_keeps_sd_write_active(self):
        self.assertEqual(self.run_case("w", 99), (0, [], [], [12, 12, 1, 1]))


if __name__ == "__main__":
    unittest.main()
