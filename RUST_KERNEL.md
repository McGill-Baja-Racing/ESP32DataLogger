# Rust kernel

Design draft (2026-10-16). Nothing is built yet.

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

Decisions:

- **The firmware stays C and links against the kernel.** `web_server.c` included, and
  PlatformIO keeps working. The kernel itself is all Rust; only the bootloaders and the C6's
  ESP-Hosted firmware are not.
- **The C API doesn't change:** ESP-IDF v5.5.5's own headers are the source of truth. The
  kernel matches their types, sizes and constants exactly.
- **Scope is what our firmware links** (about 87 ESP-IDF/FreeRTOS functions and ~45 C library
  functions), behaving as the official docs say. Many documented functions are header macros
  over a few internal ones (`xQueueSend` and `xSemaphoreGive` both call `xQueueGenericSend`),
  so the list comes from the linker (section 3), not the docs. Anything missing is a link
  error, never a stub that returns `ESP_OK`.
- **Config is frozen:** `sdkconfig.h` and `FreeRTOSConfig.h` only allow what the kernel
  supports, so an unsupported option fails at compile time.
- **Toolchain:** stable Rust ≥ 1.99, the first release that can define variadic C functions
  (ones taking `...`, like `printf`). Every crate is pinned to an exact version or commit,
  because several are unstable or git-only.
- **Master boards:** buy P4 v3.x; `esp-hal` (Espressif's Rust hardware library) supports
  revision v3.0 and later, not our v1.3. Keep the v1.3 boards for the C master meanwhile: it
  doesn't boot on v3.x until its `sdkconfig.defaults` changes.
- **Fallback:** if the kernel proves impractical, Rust code can still go inside the normal
  ESP-IDF build, which Espressif supports.

## 2. Layers

### Layer 1: core

- **Reuse:** `esp-rtos` 0.4.0, through `esp-radio-rtos-driver` 0.4.2 (feature
  `esp-radio`, no Wi-Fi) for the scheduler, tasks, queues, semaphores and delays. It has a
  100 Hz tick, round-robin within a priority, and a task switch right after an interrupt wakes
  a higher-priority task. `esp-radio-rtos-driver` adds mutexes with priority inheritance (a
  low-priority task holding a mutex is boosted while a higher one waits).
- **We write:** the FreeRTOS internals the headers call (`xQueueGenericSend`,
  `xQueueCreateMutex`, `xTaskCreatePinnedToCore`, ...) and task deletion (esp-rtos can't
  delete tasks). Handles (`TaskHandle_t`, `QueueHandle_t`) are pointers to the Rust objects.
  `portENTER_CRITICAL*` (start a critical section: code that no interrupt or other core may
  run during) maps onto esp-hal's `critical-section` lock, so no spinlocks of our own. The
  firmware uses no event groups, task notifications, stream buffers, queue sets or software
  timers, so the kernel has none.
- **Glue:** boot (`app_main` runs as a task; the app descriptor comes from `esp_app_desc!`),
  `esp-alloc` 0.11.0 behind `malloc`/`free`, and Rust interrupt handlers that call C
  callbacks. ESP-IDF's 25 priorities map onto esp-rtos's 32. The C3 has no atomic
  instructions, so `__atomic_*` (used by `stdatomic.h` in the CAN code) comes from
  `esp-radio/src/compat/atomic.rs`.

### Layer 2: system and C library

- **Reuse:** `esp-hal` 1.2.2 (time, reset reason, random numbers), `printf-compat` 0.5.0 (the
  `printf` engine), and Rust's `core`. The compiler's built-ins already export `memcpy`,
  `memset`, `memcmp` and `strlen`.
- **We write:** the rest of the C library functions, on top of `core` (`strtod` is
  `f64::from_str`, `qsort` is `sort_unstable_by`); there is no `math.h` or `scanf`. Also
  `esp_err`, `esp_log`, `esp_timer_get_time`, time (`settimeofday`, `gmtime_r`, `strftime`),
  stdio and the POSIX file calls (`open`, `fopen`, `readdir`, `rename`, ...). Variadic ones:
  `printf`, `snprintf`, `open`, `esp_log*`.
- **Glue:** stdout writes whole lines per task, so tasks' output doesn't interleave. `fgets`
  on stdin reads the serial console (the master's and simulators' commands).

### Layer 3: drivers

- **Reuse:** `esp-hal` directly for GPIO, I2C, UART, ADC, timers, C3 CAN, and the P4's SD
  card and SDIO (its link to the C6). CAN, ADC, SD and timers are marked unstable in esp-hal.
- **We write:** the ESP-IDF driver APIs on top (`gpio`, `esp_twai`, `adc_oneshot`/`adc_cali`,
  `i2c_master`, `uart`, `gptimer`, `sdmmc_host`/`sdmmc_cmd`), and the P4 CAN driver. esp-hal
  has none for the P4, but its C6 driver covers the same hardware; we add the third
  controller.

### Layer 4: master services

- **Reuse:** `fatfs` (rust-fatfs 0.4, git commit `2aefc2a`: FAT32 with long filenames, blocking
  API, no std), from Embassy (Rust's async embedded framework) `embassy-net-esp-hosted` (Wi-Fi
  via the C6) and `embassy-net` (TCP/IP), then `edge-dhcp` (dropped if the new embassy TCP/IP
  stack's own DHCP server is enough) and `edge-http`.
- **We write:** the SD block driver under `fatfs`, file timestamps from the system clock,
  access-point mode for the Wi-Fi driver (it only does station mode), and the ESP-IDF APIs on
  top: `esp_vfs_fat`, `esp_hosted_init`, `esp_wifi`, `esp_netif`, `esp_event` (default loop
  only), `nvs_flash` (see Risks) and `httpd_*`.
- **Glue:** the network runs async (many jobs taking turns) on one task, like lwIP's core task.
  A C call posts a request and waits on its own semaphore; the request, not the caller's stack,
  owns the reply, so a timeout can't leave a dangling pointer. The network task's priority is
  at or above every caller, and it never blocks. Interrupts never call it; debug builds assert
  the caller's context. HTTP handlers run on their own task (as in ESP-IDF) and FAT on its own
  task, so a handler streaming a log file doesn't deadlock the network.

### Rules at the C boundary

- `panic = "abort"`: a panic (Rust's fatal error) prints to the serial console and resets.
- Exports use plain `extern "C"` and `#[unsafe(no_mangle)]` (C calling convention and C
  names) only. No newlib or ESP-IDF object that defines the same names goes on the link line.
- C types and constants come from the headers through bindgen (a tool that turns C headers
  into Rust declarations). A developer runs it and commits `src/bindings.rs`, so teammates need
  no extra tools; its layout checks fail the build if Rust and C disagree.
- Each C function is a safe Rust function returning `Result<T, EspError>` (Rust's
  success-or-error type; `EspError` mirrors `esp_err.h`), exported by one macro that rejects
  NULL with `ESP_ERR_INVALID_ARG`, writes outputs only on success and converts the error to
  `esp_err_t`.
- A driver owns its esp-hal peripheral and pins from create to delete, so two drivers can't
  share hardware.
- C callbacks (GPIO, CAN and timer events) run in interrupt context with no lock held. A task
  switch they request happens after the callback returns. ESP-IDF's `.iram1` section names
  stay, so `IRAM_ATTR` still works. The interrupt stack is sized for the C callbacks.

## 3. Plan and testing

Each step ends with real firmware working on the bench.

| Step | Work                                                        | Done when                                           |
| ---- | ----------------------------------------------------------- | --------------------------------------------------- |
| 1    | Proof of concept on the C3                                  | the checks below pass                               |
| 2    | What `NodeBrake` needs from layers 1–3                      | `NodeBrake` works with today's master               |
| 3    | `NodeEngine`, `NodeEncoder`, `NodeMPU`, then the simulators | each works with today's master                      |
| 4    | Layers 1–3 and the CAN driver on P4 v3.x                    | core and API tests pass on the P4                   |
| 5    | Master storage (SD, `fatfs`, files)                         | `.bin` and `.bin.utc` files match the ESP-IDF build |
| 6    | Master network, then master firmware                        | web page, graphs, CSV download work                 |

Step 1 checks first the unknowns that could each stop the project:

- `pio` builds and flashes a C program through the cargo hook (section 4).
- C objects link into the esp-hal image.
- `pio device monitor` shows C text and decoded defmt lines (section 4) together, reliably
  (esp-hal's own tests turn defmt off on the C3 after random failures).
- The symbol list: link every firmware against an empty kernel and record what is missing
  (`nm -u` lists undefined symbols). This is the to-do list for steps 2–6.
- The core test (below) passes.
- Variadics on the board: `double` and `int64_t` arguments, more than seven arguments, and a
  `va_list` handed on to a second function.

Tests:

- **Core test:** the FreeRTOS standard demo tasks (`Demo/Common/Minimal`, e.g. BlockQ,
  GenQTest) and ESP-IDF FreeRTOS kernel tests, copied, for the APIs on the symbol list. They
  must cover where other FreeRTOS imitations got it wrong: preemption on give,
  `xHigherPriorityTaskWoken`, stack sizes in bytes, `vTaskDelete(NULL)`, and critical
  sections on the P4's two cores.
- **API tests:** one C program per component, on the board.
- **Differential:** every C test is built against stock ESP-IDF v5.5.5 (the reference) and
  against the kernel, and the serial outputs are compared (timings within a tolerance).
- **Progress:** one number, the share of tests passing.
- **Bench:** each node and the master match today's build.
- **Host tests:** `master-node/tests`, unchanged, plus `cargo test` on a laptop for the
  kernel's hardware-free parts (C library, `printf`, time, queues, `fatfs` on a disk image).

## 4. Build, flash and logs

- **Build:** `pio run` calls `cargo build` through one `extra_scripts` file, the cargo hook
  (`kernel/pio/cargo.py`, based on embuild's `platformio.cargo.py`), which also installs
  `espflash` and `defmt-print`. The C is compiled with PlatformIO's own RISC-V GCC.
- **Upload:** `platformio.ini` only: `upload_protocol = custom` with an `upload_command` that
  runs `espflash flash`.
- **Logs:** `pio device monitor`, as today. C logs stay plain text. Rust logs use defmt (a
  compact binary format: the strings stay in the ELF, only short IDs go over the wire), sent
  on the same serial port by esp-println's `defmt-espflash` feature. A custom monitor filter
  (`kernel/pio/filter_defmt.py`, built like PlatformIO's own `esp32_exception_decoder`, which
  also reads the ELF) passes C text through and hands defmt frames to `defmt-print`.
- **Teammates install:** `rustup`, on top of PlatformIO. `kernel/rust-toolchain.toml` picks
  the Rust version.

```
kernel/
  Cargo.toml, rust-toolchain.toml, build.rs
  include/             ESP-IDF, FreeRTOS, C library and lwIP address headers
  src/bindings.rs      bindgen output from include/
  src/core/            layer 1
  src/system/          layer 2
  src/drivers/         layer 3
  src/services/        layer 4
  pio/cargo.py         cargo hook
  pio/filter_defmt.py  monitor filter (each project's `monitor/` links to it)
```

## 5. Risks

- Nobody has published a project like this, and similar ports took 2–3× longer than planned.
  Steps that each end on the bench keep the cost visible.
- `esp-rtos` is labelled "unstable" by Espressif. Its API changed in every release so far, and
  0.3 and 0.4 also fixed multicore deadlocks, so the P4's two cores need their own tests.
- Wi-Fi through ESP-Hosted hasn't been published running end to end in Rust, nor on a P4 with
  a C6. Its driver and the new TCP/IP stack it needs (about three months old) are on embassy
  `main` only. The C6 needs ESP-Hosted packet mode, and we still need to confirm which
  ESP-Hosted version the driver supports. The SD card and the C6 link share one SD
  controller.
- `nvs_flash_init` is called only because ESP-IDF's Wi-Fi needs NVS (key-value flash
  storage). The kernel's Wi-Fi doesn't; decide in step 6 whether to implement real NVS or
  document an exception to the no-stub rule.
- ADC readings must match ESP-IDF's calibration; compare them on the bench.
- Log files written by `fatfs` must read the same on a PC and in `decode_log.py` as today's,
  including after power is pulled mid-write.

## Resources

- [ESP-IDF v5.5.5 API, C3](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/index.html) · [P4](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/index.html) · [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/system/freertos.html)
- [FreeRTOS kernel API](https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/00-TaskHandle) · [FreeRTOS standard demo tasks](https://github.com/FreeRTOS/FreeRTOS/tree/main/FreeRTOS/Demo/Common/Minimal) · [ESP-IDF FreeRTOS tests](https://github.com/espressif/esp-idf/tree/v5.5.5/components/freertos/test_apps/freertos)
- [C standard library](https://en.cppreference.com/w/c/header) · [POSIX](https://pubs.opengroup.org/onlinepubs/9799919799/)
- [Rust on ESP Book](https://docs.espressif.com/projects/rust/book/) · [esp-hal](https://docs.espressif.com/projects/rust/esp-hal/latest/) · [esp-rtos](https://docs.espressif.com/projects/rust/esp-rtos/latest/) · [esp-radio-rtos-driver](https://docs.espressif.com/projects/rust/esp-radio-rtos-driver/latest/) · [esp-alloc](https://docs.espressif.com/projects/rust/esp-alloc/latest/)
- [Embassy](https://embassy.dev/book/) · [embassy-net](https://docs.rs/embassy-net) · [embassy-net-esp-hosted](https://docs.embassy.dev/embassy-net-esp-hosted/git/default/index.html) · [edge-http](https://docs.rs/edge-http) · [edge-dhcp](https://docs.rs/edge-dhcp) · [rust-fatfs](https://github.com/rafalh/rust-fatfs)
- [bindgen](https://rust-lang.github.io/rust-bindgen/) · [critical-section](https://docs.rs/critical-section) · [printf-compat](https://github.com/lights0123/printf-compat)
- [Rust 1.99.0 (C variadics)](https://blog.rust-lang.org/2026/10/01/Rust-1.99.0/) · [lwIP threading](https://www.nongnu.org/lwip/2_1_x/multithreading.html)
- [defmt](https://defmt.ferrous-systems.com/) · [defmt-print](https://crates.io/crates/defmt-print) · [esp-println](https://github.com/esp-rs/esp-hal/tree/main/esp-println) · [espflash](https://github.com/esp-rs/espflash) · [ESP-Hosted-MCU](https://github.com/espressif/esp-hosted-mcu)
- [PlatformIO scripting](https://docs.platformio.org/en/latest/scripting/index.html) · [monitor filters](https://docs.platformio.org/en/latest/core/userguide/device/cmd_monitor.html#filters) · [embuild `platformio.cargo.py`](https://github.com/esp-rs/embuild/blob/master/src/pio/resources/platformio.cargo.py.resource) · [`upload_command`](https://docs.platformio.org/en/latest/projectconf/sections/env/options/upload/upload_command.html) · [rustup toolchain file](https://rust-lang.github.io/rustup/overrides.html)
