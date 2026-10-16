# Rust kernel

Design draft (2026-10-16). A skeleton in `kernel/` builds for both chips; nothing has run on
hardware yet.

## 1. Overview

One Rust kernel replaces ESP-IDF and FreeRTOS on every board: the C3 nodes, the C3 simulators
and the P4 master. It has four layers, each built on the ones below:

```
C firmware      unchanged
C API           ESP-IDF v5.5.5, FreeRTOS and C library headers
Layer 4         master services: FAT files, Wi-Fi, network, HTTP
Layer 3         drivers: GPIO, CAN, ADC, I2C, UART, hardware timers, SD
Layer 2         system: errors, logging, clock, C library
Layer 1         core: boot, memory, interrupts, scheduler, FreeRTOS
```

Goals:

- The firmware stays C, built by PlatformIO.
- The C API matches ESP-IDF v5.5.5's headers exactly.
- Only what our firmware links is implemented (about 130 functions, listed by the linker).
  Anything missing is a link error, never a stub.
- `sdkconfig.h` and `FreeRTOSConfig.h` only allow what the kernel supports.

## 2. Layers

### Layer 1: core

- **Reuse:** `esp-rtos` 0.4.0 (feature `esp-radio`): scheduler, tasks, queues, semaphores,
  delays, mutexes.
- **We write:** the FreeRTOS functions the headers call (`xQueueGenericSend`,
  `xTaskCreatePinnedToCore`, ...) and task deletion. `portENTER_CRITICAL*` uses esp-hal's
  `critical-section`. No event groups, task notifications, stream buffers, queue sets or
  software timers.
- **Glue:** boot sets the full CPU clock and runs `app_main` as a task. `esp-bootloader-esp-idf`
  0.6.0 adds the app descriptor. `esp-alloc` 0.11.0 provides `malloc`, `free`, `calloc` and
  `realloc`. The C3's `__atomic_*` come from `esp-radio/src/compat/atomic.rs`.

### Layer 2: system and C library

- **Reuse:** `esp-hal` 1.2.2, `esp-println` 0.18.0, `printf-compat` 0.5.0, Rust's `core`. The
  compiler provides `memcpy`, `memset`, `memcmp` and `strlen`.
- **We write:** the rest of the C library (no `math.h` or `scanf`), `esp_err`, `esp_log`,
  `esp_timer_get_time`, time, stdio and the POSIX file calls.
- **Glue:** stdout writes whole lines per task. `fgets` on stdin reads the serial console.

### Layer 3: drivers

- **Reuse:** `esp-hal` for GPIO, I2C, UART, ADC, timers, C3 CAN, and the P4's SD and SDIO.
- **We write:** the ESP-IDF driver APIs on top (`gpio`, `esp_twai`, `adc_oneshot`/`adc_cali`,
  `i2c_master`, `uart`, `gptimer`, `sdmmc_host`/`sdmmc_cmd`), and the P4 CAN driver, adapted
  from esp-hal's C6 one.

### Layer 4: master services

- **Reuse:** `fatfs` (rust-fatfs 0.4, commit `2aefc2a`), `embassy-net-esp-hosted` (Wi-Fi via the
  C6), `embassy-net`, `edge-dhcp` and `edge-http`.
- **We write:** the SD block driver, access-point mode, and the ESP-IDF APIs on top:
  `esp_vfs_fat`, `esp_hosted_init`, `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash` and
  `httpd_*`.
- **Glue:** the network runs on one task that never blocks and isn't called from interrupts.
  C calls post a request that owns its reply, so a timeout can't leave a dangling pointer.
  HTTP handlers and FAT get their own tasks, so streaming a log doesn't stall the network.

### Rules at the C boundary

- A panic prints a backtrace (`esp-backtrace` 0.20.0) and resets.
- Exports are `extern "C"` with `#[unsafe(no_mangle)]`; no newlib or ESP-IDF objects are linked.
- C types come from the headers through bindgen, committed in `kernel/sys/`.
- Each C function is a safe Rust function returning `Result<T, EspError>`, exported by one
  macro that rejects NULL and writes outputs only on success.
- A driver owns its peripheral and pins until it is deleted.
- C callbacks run in interrupt context with no lock held. `IRAM_ATTR` still works.

## 3. Plan and testing

Each step ends with real firmware working on the bench. The master needs P4 v3.x boards:
`esp-hal` doesn't support our v1.3.

| Step | Work                                                        | Done when                                           |
| ---- | ----------------------------------------------------------- | --------------------------------------------------- |
| 1    | Proof of concept on the C3                                  | the checks below pass                               |
| 2    | What `NodeBrake` needs from layers 1–3                      | `NodeBrake` works with today's master               |
| 3    | `NodeEngine`, `NodeEncoder`, `NodeMPU`, then the simulators | each works with today's master                      |
| 4    | Layers 1–3 and the CAN driver on P4 v3.x                    | core and API tests pass on the P4                   |
| 5    | Master storage (SD, `fatfs`, files)                         | `.bin` and `.bin.utc` files match the ESP-IDF build |
| 6    | Master network, then master firmware                        | web page, graphs, CSV download work                 |

Step 1 checks:

- `pio` builds and flashes a C program through the cargo hook.
- The linked C code runs on the board.
- `pio device monitor` reliably shows C text and defmt lines together.
- The symbol list: link every firmware against the empty kernel and record what is missing
  (`nm -u`). This is the to-do list for steps 2–6.
- The core test passes.
- `printf` works with `double`, `int64_t`, more than seven arguments, and a passed-on `va_list`.

Tests:

- **Core test:** the FreeRTOS demo tasks and ESP-IDF's FreeRTOS tests, covering preemption on
  give, `xHigherPriorityTaskWoken`, stack sizes in bytes, `vTaskDelete(NULL)`, and critical
  sections on the P4's two cores.
- **API tests:** one C program per component, run on stock ESP-IDF and on the kernel; serial
  outputs must match.
- **Bench:** each node and the master match today's build.
- **Host tests:** `master-node/tests`, plus `cargo test` for the hardware-free code in
  `kernel/portable/`, which exports nothing to C so it can't replace the laptop's C library.

## 4. Build, flash and logs

- **Toolchain:** latest stable Rust (1.99.1), pinned in `kernel/rust-toolchain.toml`; every
  crate pinned exactly. Teammates install `rustup` on top of PlatformIO.
- **Build:** `pio run` calls cargo through the hook `kernel/pio/cargo.py`. Cargo does the final
  link; `kernel/build.rs` links the C archive and relinks when it changes.
- **Handover:** PlatformIO picks the firmware and chip per environment; the hook passes them on:

  | PlatformIO decides | Passed to cargo as                                                                |
  | ------------------ | --------------------------------------------------------------------------------- |
  | which firmware     | `BAJA_APP_ARCHIVE`, the C archive's path (`.pio/build/NodeBrake/libapp.a`)        |
  | which chip: C3     | `--target riscv32imc-unknown-none-elf --features esp32c3`                         |
  | which chip: P4     | `--target riscv32imafc-unknown-none-elf --no-default-features --features esp32p4` |

- **By hand:** set `BAJA_APP_ARCHIVE`, then `cargo build` (C3 by default) or `cargo run` to
  flash and monitor.
- **Upload:** `upload_command` runs `espflash flash`.
- **Logs:** `pio device monitor`. C logs are plain text, Rust logs are defmt; the filter
  `kernel/pio/filter_defmt.py` decodes them.

```
kernel/
  Cargo.toml           bin crate; esp32c3 (default) and esp32p4 features; lints;
                       later the workspace root for sys/ and portable/
  .cargo/config.toml   default target (C3), espflash runners
  build.rs             links $BAJA_APP_ARCHIVE and -Tlinkall.x
  include/             ESP-IDF, FreeRTOS, C library and lwIP headers
  src/main.rs          boot and panic
  src/export.rs        the C export macro
  src/rtos.rs          layer 1 (submodules in src/rtos/, enforced by mod_module_files)
  src/system.rs        layer 2
  src/drivers.rs       layer 3
  src/services.rs      layer 4
  sys/                 bindgen output, chip-only, no lints
  portable/            hardware-free code, tested on the laptop
  pio/                 cargo hook and monitor filter
```

## Resources

- [ESP-IDF v5.5.5 API, C3](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/index.html) · [P4](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/index.html) · [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/system/freertos.html)
- [FreeRTOS kernel API](https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/00-TaskHandle) · [FreeRTOS standard demo tasks](https://github.com/FreeRTOS/FreeRTOS/tree/main/FreeRTOS/Demo/Common/Minimal) · [ESP-IDF FreeRTOS tests](https://github.com/espressif/esp-idf/tree/v5.5.5/components/freertos/test_apps/freertos)
- [C standard library](https://en.cppreference.com/w/c/header) · [POSIX](https://pubs.opengroup.org/onlinepubs/9799919799/)
- [Rust on ESP Book](https://docs.espressif.com/projects/rust/book/) · [esp-hal](https://docs.espressif.com/projects/rust/esp-hal/latest/) · [esp-rtos](https://docs.espressif.com/projects/rust/esp-rtos/latest/) · [esp-alloc](https://docs.espressif.com/projects/rust/esp-alloc/latest/) · [esp-backtrace](https://docs.rs/esp-backtrace) · [esp-bootloader-esp-idf](https://docs.rs/esp-bootloader-esp-idf)
- [Embassy](https://embassy.dev/book/) · [embassy-net](https://docs.rs/embassy-net) · [embassy-net-esp-hosted](https://docs.embassy.dev/embassy-net-esp-hosted/git/default/index.html) · [edge-http](https://docs.rs/edge-http) · [edge-dhcp](https://docs.rs/edge-dhcp) · [rust-fatfs](https://github.com/rafalh/rust-fatfs)
- [bindgen](https://rust-lang.github.io/rust-bindgen/) · [critical-section](https://docs.rs/critical-section) · [printf-compat](https://github.com/lights0123/printf-compat) · [lwIP threading](https://www.nongnu.org/lwip/2_1_x/multithreading.html)
- [defmt](https://defmt.ferrous-systems.com/) · [defmt-print](https://crates.io/crates/defmt-print) · [esp-println](https://github.com/esp-rs/esp-hal/tree/main/esp-println) · [espflash](https://github.com/esp-rs/espflash) · [ESP-Hosted-MCU](https://github.com/espressif/esp-hosted-mcu)
- [PlatformIO scripting](https://docs.platformio.org/en/latest/scripting/index.html) · [monitor filters](https://docs.platformio.org/en/latest/core/userguide/device/cmd_monitor.html#filters) · [embuild `platformio.cargo.py`](https://github.com/esp-rs/embuild/blob/master/src/pio/resources/platformio.cargo.py.resource) · [`upload_command`](https://docs.platformio.org/en/latest/projectconf/sections/env/options/upload/upload_command.html) · [Cargo build scripts](https://doc.rust-lang.org/cargo/reference/build-scripts.html)
