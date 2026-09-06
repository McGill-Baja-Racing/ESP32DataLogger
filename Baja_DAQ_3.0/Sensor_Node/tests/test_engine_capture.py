"""Host regression checks for the actual spark ISR (no ESP32 required)."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/sensors/engine_rpm.c').read_text()
start = source.index('static void IRAM_ATTR engine_rpm_isr')
end = source.index('static esp_err_t init_engine_rpm', start)
harness = r'''
#include <stdint.h>
#include <limits.h>
#define ENCODER_COUNTS_PER_REVOLUTION 128
#include <stdbool.h>
#include <assert.h>
#define IRAM_ATTR
#define MIN_SPARK_INTERVAL_US 10000
#define MAX_SPARK_INTERVAL_US 60000
#define ENGINE_STOP_TIMEOUT_US 100000
#define REVOLUTIONS_PER_SPARK 1U
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define pdFALSE 0
#define pdTRUE 1
#define portENTER_CRITICAL_ISR(x) ((void)0)
#define portEXIT_CRITICAL_ISR(x) ((void)0)
#define portYIELD_FROM_ISR() ((void)0)
typedef int BaseType_t;
typedef struct { int64_t timestamp_us, elapsed_us; bool valid; } spark_capture_t;
typedef struct { int64_t last_spark_us, last_zero_us; uint32_t dropped; void *events; int lock; } engine_rpm_context_t;
typedef struct { int64_t timestamp_us; int32_t engine_rpm; bool spark; } engine_event_t;
static engine_rpm_context_t engine;
static int receive_ok;
static int64_t now;
static spark_capture_t captured;
static int queue_ok = 1;
static int64_t esp_timer_get_time(void) { return now; }
static int xQueueSendFromISR(void *q, const spark_capture_t *c, int *wake) {
    (void)q; (void)wake; captured = *c; return queue_ok;
}
static int xQueueReceive(void *q, spark_capture_t *c, int ticks) {
    (void)q; (void)ticks; *c = captured; return receive_ok;
}
'''
encoder = (Path(__file__).resolve().parents[1] / 'src/sensors/bearing_encoder.c').read_text()
harness += encoder[encoder.index('static int32_t calculate_rpm'):encoder.index('static int encoder_is_stopped')]
harness += source[start:end]
harness += source[source.index('bool engine_rpm_next_event'):source.index('uint32_t engine_rpm_dropped_events')]
harness += r'''
static void edge(engine_rpm_context_t *c, int64_t t, int64_t count) {
    now = t; (void)count; engine_rpm_isr(c);
    assert(captured.timestamp_us == t);
}
int main(void) {
    assert(calculate_rpm(128, 60000000) == 1);
    assert(calculate_rpm(64, 20000) == 1500);
    assert(calculate_rpm(-64, 20000) == -1500);
    assert(calculate_rpm(0, 20000) == 0);
    assert(calculate_rpm(64, 0) == 0);
    engine_rpm_context_t c = {0};
    edge(&c, 1000, 0); assert(!captured.valid);
    edge(&c, 11000, 10); assert(captured.valid && captured.elapsed_us == 10000);
    edge(&c, 12000, 11); assert(!captured.valid && c.last_spark_us == 12000);
    edge(&c, 21000, 20); assert(!captured.valid);
    edge(&c, 81000, 30); assert(captured.valid && captured.elapsed_us == 60000);
    edge(&c, 141001, 40); assert(!captured.valid);
    edge(&c, 161001, 35); assert(captured.valid);
    edge(&c, 181001, 35); assert(captured.valid);
    edge(&c, 181001 + (INT64_C(1) << 32) + 20000, 40); assert(!captured.valid);
    for (int i = 0; i < 10; i++) { edge(&c, now + 6667, 40); assert(!captured.valid); }
    queue_ok = 0; edge(&c, now + 20000, 45); assert(c.dropped == 1);
    engine_event_t event;
    engine.last_zero_us = 1000000;
    now = 1099999; assert(!engine_rpm_next_event(&event));
    now = 1100000; assert(engine_rpm_next_event(&event));
    assert(!event.spark && event.engine_rpm == 0);
    assert(event.timestamp_us == now);
    now = 1199999; assert(!engine_rpm_next_event(&event));
    now = 1200000; assert(engine_rpm_next_event(&event));
    engine.last_spark_us = 1250000;
    now = 1349999; assert(!engine_rpm_next_event(&event));
    now = 1350000; assert(engine_rpm_next_event(&event));
    receive_ok = 1;
    captured = (spark_capture_t){.timestamp_us = 1360000, .elapsed_us = 20000, .valid = true};
    assert(engine_rpm_next_event(&event));
    assert(event.spark && event.engine_rpm == 3000);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'test.c'
    exe = Path(directory) / 'test'
    src.write_text(harness)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Spark capture tests passed')
