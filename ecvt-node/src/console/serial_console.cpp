#include "serial_console.h"
#include "command_parser.h"
#include "line_input.h"
#include "app/ecvt_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

void serial_console_print_help()
{
    std::puts("help | status | stop (6040=0000) | mode manual\n"
              "servo ping                         (read-only CAN communication test)\n"
              "servo inspect                      (read-only drive state and output objects)\n"
              "servo errors                       (read-only current error, register and history)\n"
              "servo read <index> <sub> u8/u16/u32  (read-only SDO; decimal or 0x hex addresses)\n"
              "servo cw 0x0000/0x0006/0x0007/0x000F (0000 always requests disable; other steps are guarded)\n"
              "servo velocity-zero                (disabled drive only: select mode 3, target 0; no enable)\n"
              "servo position-mode                (disabled drive only: select mode 1; no target or enable)\n"
              "servo torque-zero-test             (EXPERIMENTAL: mode 4, torque target 0, enable 20 s, then disable)\n"
              "servo reset-fault                  (manual idle bench: fault reset only, no enable)\n"
              "servo brake-test                   (0006 -> 0007 -> 000F, 10 s at zero velocity, then 0000)\n"
              "servo brake-restore                (send 0000 and confirm drive disabled)\n"
              "limits <min_rev> <max_rev> <max_rpm>   (required before movement)\n"
              "profile <accel_rpm_per_s> <decel_rpm_per_s>\n"
              "move <logical_rev> <rpm>             (manual mode)\n"
              "Drive scaling: 10000 counts/rev; verify drive gearing. Brake lock unverified.");
}

static void console_task(void *)
{
    ConsoleLineInput line;
    serial_console_print_help();
    while (true) {
        int c = getchar();
        if (c < 0) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        const auto result = line.push(static_cast<unsigned char>(c));
        if (result != ConsoleLineInput::Result::Pending) {
            if (result == ConsoleLineInput::Result::Overflow) std::puts("Command too long; discarded.");
            else if (result != ConsoleLineInput::Result::Empty) {
                ConsoleCommand command;
                if (result == ConsoleLineInput::Result::Ready && parse_console_command(line.text, &command)) {
                    std::printf("Received: %s\n", line.text);
                    ecvt_app_submit(command);
                } else {
                    std::printf("Invalid command received (%u bytes):", static_cast<unsigned>(line.length));
                    for (std::size_t i = 0; i < line.length; ++i)
                        std::printf(" %02X", static_cast<unsigned char>(line.text[i]));
                    std::puts(". Type help; retype without arrow keys/terminal escape sequences.");
                }
            }
            line.reset();
        }
    }
}

esp_err_t serial_console_start()
{
#if CONFIG_ESP_CONSOLE_UART
    // Default VFS polls the hardware FIFO. Buffer bursts independently of task
    // scheduling/output, and let getchar block until data is available.
    const uart_port_t port = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    if (!uart_is_driver_installed(port)) {
        const esp_err_t result = uart_driver_install(port, 2048, 0, 0, nullptr, 0);
        if (result != ESP_OK) return result;
    }
    uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_use_driver(port);
#endif
    setvbuf(stdin, nullptr, _IONBF, 0);
    return xTaskCreate(console_task, "console", 4096, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
