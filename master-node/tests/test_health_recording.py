"""Run production logger admission and draining across two recording sessions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HealthRecordingTests(unittest.TestCase):
    def test_start_stop_health_grace_and_restart(self):
        source = (ROOT / 'src/logger/data_logger.c').read_text()
        start = source[source.index('void data_logger_clear_queue('):
                       source.index('data_logger_rename_result_t data_logger_rename(')]
        admission = source[source.index('bool data_logger_stop('):
                           source.index('logger_state_t data_logger_state(')]
        writer = source[source.index('static void writer_task('):
                        source.index('esp_err_t data_logger_init(')]
        harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include "logger/data_logger.h"
#include "protocol/app_protocol.h"
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(ms) (ms)
#define configASSERT(condition) assert(condition)
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define LOG_RECORDS_PER_BLOCK 100
#define LOG_FLUSH_INTERVAL_MS 15000
#define LOG_STOP_GRACE_MS 200
typedef uint32_t TickType_t;
typedef struct { can_message_t message; int64_t utc_ms; } queued_sample_t;
static logger_state_t state = LOGGER_IDLE;
static bool health_admission_open, health_locked, full, probe_close;
static atomic_uint queue_drops;
static int health_admission_mutex = 1, file_mutex = 2, log_queue = 3;
static FILE *log_file, *utc_file;
static TickType_t tick, stop_tick;
static queued_sample_t queue[8];
static unsigned queued, saved, refused_after_close;
static uint32_t saved_ids[8];
static jmp_buf done;
static int xSemaphoreTake(int mutex, unsigned wait) {
    (void)wait;
    if (mutex == health_admission_mutex) { assert(!health_locked); health_locked = true; }
    return pdTRUE;
}
static void xSemaphoreGive(int mutex) {
    if (mutex != health_admission_mutex) return;
    assert(health_locked); health_locked = false;
    if (probe_close && state == LOGGER_STOPPING && !health_admission_open) {
        probe_close = false;
        can_message_t late = {.id = CAN_ID_NODE_EVENT_BASE + 8, .dlc = 8};
        assert(!data_logger_enqueue_health(&late));
        refused_after_close++;
    }
}
static int xQueueReset(int q) {
    assert(q == log_queue && state == LOGGER_IDLE && !health_admission_open);
    queued = 0; return pdTRUE;
}
static int xQueueSend(int q, const queued_sample_t *sample, unsigned wait) {
    (void)wait; assert(q == log_queue);
    if (full) return 0;
    assert(queued < 8); queue[queued++] = *sample; return pdPASS;
}
static int xQueueReceive(int q, queued_sample_t *sample, unsigned wait) {
    (void)wait; assert(q == log_queue && state != LOGGER_IDLE);
    tick += 100;
    if (!queued) return 0;
    *sample = queue[0];
    for (unsigned i = 1; i < queued; i++) queue[i-1] = queue[i];
    queued--; return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(int q) { assert(q == log_queue); return queued; }
static TickType_t xTaskGetTickCount(void) { return tick; }
static void vTaskDelay(unsigned delay) {
    assert(delay && state == LOGGER_IDLE); longjmp(done, 1);
}
static int64_t absolute_clock_sample_utc(uint32_t ms) { return 123000 + ms; }
static void node_health_count(health_code_t code, unsigned n, uint32_t id, int error) {
    (void)id; (void)error; assert(code == HEALTH_SAMPLES_DROPPED && n == 1);
}
static void node_health_set(health_code_t code, bool active) {
    assert(code == HEALTH_SD_WRITE && !active);
}
static bool write_records(const int64_t records[][2], const int64_t *utc, size_t count) {
    assert(!health_admission_open && tick - stop_tick >= LOG_STOP_GRACE_MS);
    for (size_t i = 0; i < count; i++) {
        assert(utc[i] == 123000 + (uint32_t)((uint64_t)records[i][1] >> 32));
        saved_ids[saved++] = (uint32_t)records[i][0];
    }
    return true;
}
static bool checkpoint_file(void) { assert(false); return false; }
''' + start + admission + writer + r'''
int main(void) {
    can_message_t health = {.id = CAN_ID_NODE_EVENT_BASE + 8, .dlc = 8, .data = UINT64_C(42) << 32};
    can_message_t aim = {.id = CAN_ID_AIM_RPM, .dlc = 8, .data = UINT64_C(43) << 32};
    for (unsigned session = 0; session < 2; session++) {
        assert(state == LOGGER_IDLE && !health_admission_open);
        log_file = tmpfile(); utc_file = tmpfile(); assert(log_file && utc_file);
        assert(!data_logger_enqueue_health(&health)); /* prepared, not started */
        data_logger_clear_queue();
        assert(!data_logger_enqueue_health(&health));
        data_logger_start();
        assert(data_logger_enqueue_health(&health));
        data_logger_enqueue(&aim);
        full = true;
        assert(!data_logger_enqueue_health(&health));
        full = false;
        assert(data_logger_stop() && !data_logger_stop());
        data_logger_enqueue(&aim); /* sensor admission closes at STOP */
        assert(queued == 2);
        assert(data_logger_enqueue_health(&health)); /* health survives STOP grace */
        probe_close = true;
        if (!setjmp(done)) writer_task(NULL);
        assert(state == LOGGER_IDLE && !health_admission_open && queued == 0);
        assert(!log_file && !utc_file && !health_locked);
        assert(!data_logger_enqueue_health(&health));
        assert(saved == (session + 1) * 3 && refused_after_close == session + 1);
        assert(saved_ids[session*3] == health.id && saved_ids[session*3+1] == aim.id &&
               saved_ids[session*3+2] == health.id);
    }
    assert(atomic_load(&queue_drops) == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n')
            (temp / 'test.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(temp), '-I' + str(ROOT / 'src'),
                            '-I' + str(ROOT.parent), str(temp / 'test.c'),
                            '-o', str(temp / 'test')], check=True)
            subprocess.run([str(temp / 'test')], check=True)
