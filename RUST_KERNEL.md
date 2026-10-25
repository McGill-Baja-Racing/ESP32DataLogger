# Rust kernel

Design draft (2026-10-16). `NodeBrake` and `NodeADC` build and link on the C3; nothing has run on
hardware yet. The P4 build needs its `sdkconfig.h`.

## 1. Overview

One Rust kernel replaces ESP-IDF and FreeRTOS on every board: the C3 nodes, the C3 simulators and
the P4 master. The C firmware doesn't change: it calls the same ESP-IDF API, which the kernel
implements.

```
C firmware      unchanged
C API           ESP-IDF v5.5.5, FreeRTOS and C library headers
Layer 4         master services: FAT files, Wi-Fi, network, HTTP
Layer 3         drivers: GPIO, CAN, ADC, I2C, UART, hardware timers, SD
Layer 2         system: errors, logging, clock, C library
Layer 1         core: boot, memory, interrupts, scheduler, FreeRTOS
```

- cargo builds the C with clang and links it into the kernel; a cargo feature picks the node.
- The C API matches ESP-IDF v5.5.5's headers exactly. Each chip's `sdkconfig.h` is ESP-IDF's
  generated one, trimmed to the options the headers and firmware read.
- Only what the firmware links is implemented (about 130 functions). Anything missing is a link
  error, never a stub.
- Only the options the firmware passes are supported. Naming another is a compile error
  (`kernel/include/unsupported.h`); a value only known at run time panics.

## 2. Layers

### Layer 1: core

- **Reuse:** `esp-rtos` 0.4.0 (feature `esp-radio`) for the scheduler, tasks, queues,
  semaphores, mutexes and delays. `esp-alloc` 0.11.0 for `malloc`, `free`, `calloc` and
  `realloc`. `esp-bootloader-esp-idf` 0.6.0 for the app descriptor.
- **We write:** the FreeRTOS functions the headers call (`xQueueGenericSend`,
  `xTaskCreatePinnedToCore`, ...), task deletion and the C3's `__atomic_*`.
  `portENTER_CRITICAL*` uses esp-hal's `critical-section`. No event groups, task notifications,
  stream buffers, queue sets or software timers.
- **Boot:** sets the full CPU clock and runs `app_main` as a task. An RTC watchdog, fed by a
  lowest-priority task, resets the board when any task runs 2 s without blocking.

### Layer 2: system and C library

- **Reuse:** `esp-hal` 1.2.2, `esp-println` 0.18.0, `log` 0.4.34, `printf-compat` 0.5.0 and
  Rust's `core`. The C3's ROM provides `memcpy`, `memset`, `memcmp` and `memmove`, so the kernel
  must not define them. The compiler provides `strlen`.
- **We write:** the rest of the C library (no `math.h` or `scanf`), `esp_err`, `esp_log`,
  `esp_timer_get_time`, time, stdio and the POSIX file calls. stdout writes whole lines per
  task; `fgets` on stdin reads the serial console.
- **Open:** the master may link newlib's C library instead; decided at step 5.

### Layer 3: drivers

- **Reuse:** `esp-hal` for GPIO, I2C, UART, ADC, timers, C3 CAN, and the P4's SD and SDIO.
- **We write:** the ESP-IDF driver APIs on top (`gpio`, `esp_twai`, `adc_oneshot`/`adc_cali`,
  `i2c_master`, `uart`, `gptimer`, `sdmmc_host`/`sdmmc_cmd`), and a P4 CAN driver adapted from
  esp-hal's C6 one.

### Layer 4: master services

- **Reuse:** `fatfs` (rust-fatfs 0.4, commit `2aefc2a`), `embassy-net-esp-hosted` (Wi-Fi via the
  C6 over esp-hal's `sdmmc`; SDIO is unreleased, commit `b74e728`), `embassy-net`, `edge-dhcp`
  and `edge-http`.
- **We write:** the SD block driver, access-point mode, and the ESP-IDF APIs on top:
  `esp_vfs_fat`, `esp_hosted_init`, `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash` and
  `httpd_*`.
- **Design:** the C6 runs esp-hosted-mcu with SDIO streaming mode off. The network runs on one
  task that never blocks and is never called from interrupts. A C call posts a request that owns
  its reply, so a timeout can't leave a dangling pointer. HTTP handlers and FAT get their own
  tasks, so streaming a log doesn't stall the network.

### Rules at the C boundary

- Each C function is a Rust function marked `#[export]` (`kernel/macros/`), which makes it
  `extern "C"` and `#[unsafe(no_mangle)]` and checks it against the header.
- C types come from the headers through bindgen, run at build time (`kernel/sys/build.rs`).
- No ESP-IDF or newlib objects are linked (newlib on the master is open; see layer 2).
- Outputs are written only on success.
- A NULL `&T` argument panics like `configASSERT`. A NULL or non-UTF-8 `&str` reads as `"?"`.
- A panic prints a backtrace (`esp-backtrace` 0.20.0) and resets.
- C assigns pins, as in ESP-IDF; the kernel doesn't track which driver uses one.
- C callbacks run in interrupt context with no lock held. `IRAM_ATTR` works.

## 3. Plan and testing

This branch replaces the C core and stays off `main` until done. Each step moves its projects to
the cargo build and ends with real firmware working on the bench. Steps 4–6 need P4
v3.x boards: `esp-hal` doesn't support our v1.3.

| Step | Work                                                        | Done when                                           |
| ---- | ----------------------------------------------------------- | --------------------------------------------------- |
| 1    | Proof of concept on the C3                                  | the checks below pass                               |
| 2    | What `NodeBrake` needs from layers 1–3                      | `NodeBrake` works with the ESP-IDF master           |
| 3    | `NodeEngine`, `NodeEncoder`, `NodeMPU`, then the simulators | each works with the ESP-IDF master                  |
| 4    | Layers 1–3 and the CAN driver on the P4, one core           | the core test passes on the P4                      |
| 5    | Master storage (SD, `fatfs`, files)                         | `.bin` and `.bin.utc` files match the ESP-IDF build |
| 6    | Master network, then master firmware                        | web page, graphs, CSV download work                 |

Step 1 checks:

- `cargo run` builds and flashes a C program, and the C runs on the board.
- espflash's monitor reliably shows C and Rust log lines together.
- Every firmware is linked against the kernel; its missing symbols are the to-do list for
  steps 2–6.
- The core test passes.
- `printf` works with `double`, `int64_t`, more than seven arguments, and a passed-on `va_list`.

Tests:

- **Core:** the FreeRTOS demo tasks and ESP-IDF's FreeRTOS tests: preemption on give,
  `xHigherPriorityTaskWoken`, stack sizes in bytes, `vTaskDelete(NULL)`. Critical sections on
  the P4's two cores are tested after it works on one.
- **Bench:** each node and the master run on the ESP-IDF build and on the kernel build, and the
  logger's output is compared. A focused test is added only for a bug the logs can't pin down.
- **Host:** `master-node/tests`, plus `cargo test` for the hardware-free code in
  `kernel/portable/`. That code exports nothing to C, so it can't replace the laptop's C library.

## 4. Build, flash and logs

- **Install:** `rustup`, git, LLVM (clang and libclang: `brew install llvm`, `winget install
  LLVM.LLVM` or `apt install clang libclang-dev`) and `cargo install espflash@4.6.0 --locked`.
  Rust 1.99.1 is pinned in `kernel/rust-toolchain.toml` and every crate exactly.
- **Build:** in `kernel/`, `cargo build --release --features <node>`, with one of `brake`, `mpu`,
  `encoder`, `engine`, `engine-bench` and `adc`. `kernel/build.rs` compiles `sensor-node/src`
  with that node's defines and links it into the kernel. Without a node, only `cargo check` and
  `cargo clippy` work.
- **Headers:** on the first build, `kernel/sys/build.rs` fetches ESP-IDF v5.5.5's header folders
  and Espressif's newlib headers with git into cargo's target folder. bindgen and clang read
  them with the same arguments.
- **Flash and logs:** `cargo run --release --features <node>` flashes with espflash and opens its
  monitor. C and Rust logs are both colored by level; Rust logs through the `log` crate and
  `esp-println`.
- **Editor:** `baja-daq.code-workspace` has a build task per node that flashes it. clangd reads the
  last build's `kernel/target/compile_commands.json` through `sensor-node/.clangd`.

```
kernel/
  Cargo.toml           bin crate; chip and node features; workspace root; lints
  .cargo/config.toml   default target (C3), espflash runners
  build.rs             compiles the node's C with clang; links it and esp-hal's linker script
  no_c.x               the error a build without a node gives
  include/             each chip's sdkconfig.h; unsupported.h
  src/main.rs          boot, watchdog and panic
  src/rtos.rs          layer 1 (submodules in src/rtos/, enforced by mod_module_files)
  src/system.rs        layer 2
  src/<driver>.rs      layer 3, one file per ESP-IDF driver (adc.rs, twai.rs)
  src/services.rs      layer 4 (later)
  macros/              the #[export] macro
  sys/                 fetches the headers and runs bindgen at build time; no lints
  portable/            hardware-free code, tested on the laptop (later)
```

## Resources

- [ESP-IDF v5.5.5 API, C3](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/index.html) · [P4](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/index.html) · [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/system/freertos.html)
- [FreeRTOS kernel API](https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/00-TaskHandle) · [FreeRTOS standard demo tasks](https://github.com/FreeRTOS/FreeRTOS/tree/main/FreeRTOS/Demo/Common/Minimal) · [ESP-IDF FreeRTOS tests](https://github.com/espressif/esp-idf/tree/v5.5.5/components/freertos/test_apps/freertos)
- [C standard library](https://en.cppreference.com/w/c/header) · [POSIX](https://pubs.opengroup.org/onlinepubs/9799919799/)
- [Rust on ESP Book](https://docs.espressif.com/projects/rust/book/) · [esp-hal](https://docs.espressif.com/projects/rust/esp-hal/latest/) · [esp-rtos](https://docs.espressif.com/projects/rust/esp-rtos/latest/) · [esp-alloc](https://docs.espressif.com/projects/rust/esp-alloc/latest/) · [esp-backtrace](https://docs.rs/esp-backtrace) · [esp-bootloader-esp-idf](https://docs.rs/esp-bootloader-esp-idf)
- [Embassy](https://embassy.dev/book/) · [embassy-net](https://docs.rs/embassy-net) · [embassy-net-esp-hosted](https://docs.embassy.dev/embassy-net-esp-hosted/git/default/index.html) · [edge-http](https://docs.rs/edge-http) · [edge-dhcp](https://docs.rs/edge-dhcp) · [rust-fatfs](https://github.com/rafalh/rust-fatfs)
- [bindgen](https://rust-lang.github.io/rust-bindgen/) · [critical-section](https://docs.rs/critical-section) · [printf-compat](https://github.com/lights0123/printf-compat) · [lwIP threading](https://www.nongnu.org/lwip/2_1_x/multithreading.html)
- [log](https://docs.rs/log) · [esp-println](https://github.com/esp-rs/esp-hal/tree/main/esp-println) · [espflash](https://github.com/esp-rs/espflash) · [ESP-Hosted-MCU](https://github.com/espressif/esp-hosted-mcu)
- [Cargo build scripts](https://doc.rust-lang.org/cargo/reference/build-scripts.html) · [Clang command line](https://clang.llvm.org/docs/ClangCommandLineReference.html) · [clangd configuration](https://clangd.llvm.org/config)
