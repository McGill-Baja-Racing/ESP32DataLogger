# Rust kernel

Design draft (2026-10-16). Nothing is built yet.

## 1. Overview

One Rust kernel replaces ESP-IDF and FreeRTOS on every board: the C3 nodes, the C3 simulators
and the P4 master. It is built inside out, in four layers:

```
C firmware      unchanged
C API           ESP-IDF v5.5.5, FreeRTOS and C library headers, unchanged
Layer 4         services: files, network, HTTP, Wi-Fi
Layer 3         drivers: GPIO, CAN, ADC, I2C, UART, hardware timers, SD
Layer 2         system: errors, logging, esp_timer, C library
Layer 1         core: boot, memory, interrupts, scheduler, FreeRTOS
```

Decisions:

- **The C API doesn't change:** ESP-IDF v5.5.5's own headers, every documented function under
  its own name, behaving as the official docs say.
- **Full coverage:** the whole documented API of each component used (~830 functions), plus
  the full C standard library.
- **Not Rust:** the bootloaders, the C6's ESP-Hosted firmware, and C functions taking `...`
  (stable Rust can't define them).
- **`web_server.c` stays C.** PlatformIO keeps working. Stable Rust ≥ 1.95.
- **Master boards:** buy P4 v3.2 (esp-hal doesn't support our v1.3).

## 2. Layers

### Layer 1: core

Mostly `esp-rtos` 0.4.0 used directly (via `esp-radio-rtos-driver` 0.4.2, feature
`esp-radio`, no Wi-Fi). Thin C-ABI wrappers give it the FreeRTOS names.

- **From `esp-rtos`:** the scheduler, tasks, queues, semaphores, mutexes, timers, core
  pinning. It already behaves like today's FreeRTOS (priorities, 100 Hz tick, priority
  inheritance, switch after an ISR wake-up). Template: `esp-radio/src/compat/`.
- **We write:** event groups, task notifications, stream/message buffers, queue sets,
  per-task storage.
- **Glue:** boot (`app_main` as a task, app descriptor), `esp-alloc` 0.11.0 behind
  `malloc`/`free`, C callbacks from Rust interrupt handlers, `__atomic_*` on the C3.

### Layer 2: system and C library

Mostly existing code, behind the ESP-IDF and C library names.

- **From existing code:** `esp-hal` (time, reset reason), `c-scape` 0.22.3 (string, ctype,
  number parsing, `qsort`; copied, because as a dependency it needs Linux), `libm` 0.2.16
  (`math.h`), `eyalroz/printf` v6.4.0 (`printf`, C), picolibc's `vfscanf` (`scanf`, C).
- **We write:** `esp_err`, `esp_log`, `esp_timer`, time, stdio, files, and the rest of the C
  library.
- **Glue:** thin C wrappers for the other `...` functions (`open`, `fcntl`, `ioctl`,
  `esp_log*`). stdout writes whole lines per task.

### Layer 3: drivers

Mostly `esp-hal` 1.2.0 used directly, behind the ESP-IDF driver APIs.

- **From `esp-hal`:** GPIO, I2C, UART, ADC (same calibration as today), timers, C3 CAN, P4 SD
  card and SDIO.
- **We write:** the ESP-IDF driver APIs on top. P4 CAN: switch esp-hal's C6 driver on for the
  P4 (same hardware) and add its third controller.
- **Glue:** C callbacks run in interrupt context.

### Layer 4: master services

Mostly existing crates used directly, behind the ESP-IDF APIs.

- **From existing crates:** `embedded-fatfs` (FAT with long filenames), `embassy-net-esp-hosted`
  (Wi-Fi via the C6), `embassy-net` (TCP/IP), `edge-dhcp`, `edge-http`.
- **We write:** files and clock on top of FAT, access-point mode for the Wi-Fi driver, and the
  ESP-IDF APIs (`esp_vfs_fat`, `esp_wifi`, `esp_netif`, `httpd_*`) on top.
- **Glue:** the services run async on one thread; a C call blocks its task until the reply.

## 3. Plan and testing

Each layer is finished to its full API before the next starts.

| Step | Work                                   | Done when                                  |
| ---- | -------------------------------------- | ------------------------------------------ |
| 1    | Layer 1 on the C3 (proof of concept)   | `pio` builds and flashes; core test passes |
| 2    | Layer 2 on the C3                      | API tests pass                             |
| 3    | Layer 3 on the C3                      | API tests pass on the bench                |
| 4    | Node and simulator firmware, unchanged | nodes work with today's master             |
| 5    | Buy P4 v3.2; layers 1–3 on the P4      | API tests pass on the P4                   |
| 6    | Master storage (SD, FAT, files)        | log files match the ESP-IDF build          |
| 7    | Master network, then master firmware   | web page, graphs, CSV download work        |

- **Core test (proof of concept):** a C program written only from the FreeRTOS docs (tasks,
  queues, a mutex, an ISR send) behaves as documented.
- **API tests:** one C program per component, on the board.
- **Bench:** each node and the master match today's build.
- **Host tests:** `master-node/tests`, unchanged.

## 4. Build, flash and logs

- **Build:** `pio run` calls `cargo build` through one `extra_scripts` file
  (`kernel/pio/cargo.py`), which also installs `espflash`. The C is compiled with
  PlatformIO's own RISC-V GCC.
- **Upload:** `platformio.ini` only: `upload_protocol = custom` with `espflash flash`.
- **Monitor:** unchanged; C output is plain text on serial.
- **Rust logs:** defmt, read with `probe-rs` (developers only).
- **Teammates install:** `rustup`, plus `probe-rs` to read Rust logs.

```
kernel/
  Cargo.toml, rust-toolchain.toml, build.rs
  include/        ESP-IDF, FreeRTOS, C library headers
  src/core/       boot, scheduler glue, atomics
  src/bindings/   the C API functions
  src/libc/       the C library
  src/services/   files, network, HTTP, Wi-Fi
  c/              printf, scanf, variadic wrappers
  pio/cargo.py    PlatformIO build hook
```

## 5. Risks

- The C master doesn't boot on v3.2 until its `sdkconfig.defaults` changes; keep the v1.3
  boards for it meanwhile.
- C6: needs ESP-Hosted packet mode, and we still need to confirm which ESP-Hosted version the
  Rust Wi-Fi driver supports.
- Wi-Fi hasn't run end to end in Rust; its driver is on embassy `main` only.
- Not tried yet, so step 1 checks them: the PlatformIO hook, and `probe-rs` and the serial
  monitor sharing the C3's USB port.
- ~830 functions plus the C library is a lot; one layer at a time keeps it manageable.
- `esp-rtos` is labelled "unstable" by Espressif. That means its API may change between
  releases, not that it crashes. Some crates are only on git, not crates.io. Pinning exact
  versions and commits keeps both steady.

## Resources

- [ESP-IDF v5.5.5 API, C3](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/index.html) · [P4](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/index.html) · [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/system/freertos.html)
- [FreeRTOS kernel API](https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/00-TaskHandle)
- [C standard library](https://en.cppreference.com/w/c/header) · [POSIX](https://pubs.opengroup.org/onlinepubs/9799919799/)
- [Rust on ESP Book](https://docs.espressif.com/projects/rust/book/) · [esp-hal](https://docs.espressif.com/projects/rust/esp-hal/latest/) · [esp-rtos](https://docs.espressif.com/projects/rust/esp-rtos/latest/) · [esp-radio-rtos-driver](https://docs.espressif.com/projects/rust/esp-radio-rtos-driver/latest/) · [esp-alloc](https://docs.espressif.com/projects/rust/esp-alloc/latest/)
- [Embassy](https://embassy.dev/book/) · [embassy-net](https://docs.rs/embassy-net) · [embassy-net-esp-hosted](https://docs.embassy.dev/embassy-net-esp-hosted/git/default/index.html) · [edge-http](https://docs.rs/edge-http) · [edge-dhcp](https://docs.rs/edge-dhcp) · [embedded-fatfs](https://github.com/MabezDev/embedded-fatfs)
- [c-ward / c-scape](https://github.com/sunfishcode/c-ward) · [libm](https://docs.rs/libm) · [eyalroz/printf](https://github.com/eyalroz/printf) · [picolibc](https://github.com/picolibc/picolibc)
- [defmt](https://defmt.ferrous-systems.com/) · [probe-rs](https://probe.rs/docs/) · [espflash](https://github.com/esp-rs/espflash) · [ESP-Hosted-MCU](https://github.com/espressif/esp-hosted-mcu)
- [PlatformIO scripting](https://docs.platformio.org/en/latest/scripting/index.html) · [`upload_command`](https://docs.platformio.org/en/latest/projectconf/sections/env/options/upload/upload_command.html) · [rustup toolchain file](https://rust-lang.github.io/rustup/overrides.html)
