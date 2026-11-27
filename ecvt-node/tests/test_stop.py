"""Compile the complete production core/runner with a deterministic fake drive."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]

class StopTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'Host C++ compiler unavailable')
    def test_frontend_independent_core_moves_parameters_and_stop(self):
        headers = {
            'esp_err.h': '#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_TIMEOUT=3, ESP_ERR_NO_MEM=4;\nconst char *esp_err_to_name(int);\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'esp_twai.h': '#pragma once\nusing twai_node_handle_t=void*;\n',
            'freertos/FreeRTOS.h': '#pragma once\n#include <cstdint>\nconstexpr int pdTRUE=1, pdPASS=1;\n#define pdMS_TO_TICKS(x) (x)\nusing portMUX_TYPE=int;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n',
            'freertos/queue.h': '#pragma once\n#include "FreeRTOS.h"\nusing QueueHandle_t=void*;\nQueueHandle_t xQueueCreate(unsigned,unsigned);\nvoid vQueueDelete(QueueHandle_t);\nint xQueueSend(QueueHandle_t,const void*,unsigned);\nint xQueueReceive(QueueHandle_t,void*,unsigned);\nvoid xQueueReset(QueueHandle_t);\n',
            'freertos/task.h': '#pragma once\n#include "FreeRTOS.h"\nvoid vTaskDelay(unsigned);\nint xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,void*);\n',
        }
        harness = (PROJECT/'tests/motor_core_harness.cpp').read_text()
        # Unused drive configuration calls succeed; state/target/object calls are
        # explicitly simulated by the harness. No motor-core function is stubbed.
        custom = set(re.findall(r'int (canopen_drive_\w+)\(', harness))
        declarations = (PROJECT/'src/canopen/drive.h').read_text()
        for name, args in re.findall(r'esp_err_t (canopen_drive_\w+)\((.*?)\);', declarations, re.S):
            if name not in custom:
                harness += f'\nint {name}({args}) {{return 0;}}\n'
        with tempfile.TemporaryDirectory(prefix='ecvt-core-') as directory:
            tmp=Path(directory)
            for name,content in headers.items():
                path=tmp/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(content)
            source=tmp/'harness.cpp'; source.write_text('#include <algorithm>\n'+harness)
            binary=tmp/'core'
            sources=['drive/control.cpp','drive/operations.cpp','canopen/feedback.cpp']
            subprocess.run(['c++','-std=c++17','-I',str(tmp),'-I',str(PROJECT/'src'),
                            *[str(PROJECT/'src'/s) for s in sources],str(source),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True,check=True)
            self.assertIn('Frontend-independent core',result.stdout)
