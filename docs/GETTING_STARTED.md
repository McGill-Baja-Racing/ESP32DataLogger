# Getting started without hardware

This guide takes you from a new checkout to a firmware build. No electronics knowledge or connected board is required. Return to the [project introduction](../README.md) at any time.

## Learn just enough to start

| Term | Meaning here |
|---|---|
| Git | Tracks file changes and lets contributors work on branches |
| Repository / clone | The project and its history / a local copy of it |
| Terminal | A window where you type commands; its current folder determines relative paths |
| C | The programming language used for the firmware |
| Firmware / microcontroller | The board's program / the small computer running it |
| Build / upload | Compile code into firmware / copy that firmware onto a board |
| ESP-IDF | Espressif's framework for drivers, networking, and operating-system services |
| PlatformIO | Tooling that downloads toolchains and builds/uploads a selected project profile |
| Environment | A named build profile such as `NodeBrake`, not a connected board |

For your first task, learn Git's clone, branch, diff, commit, and pull-request workflow from [GitHub's Hello World](https://docs.github.com/en/get-started/using-github/hello-world). Before editing firmware, become comfortable with C functions, structs, pointers, and integer types; [Beej's Guide to C](https://beej.us/guide/bgc/) is a free introduction. You can contribute documentation before mastering C.

## Install tools

Install Git, [VS Code](https://code.visualstudio.com/), and the **PlatformIO IDE** extension inside VS Code. Use [PlatformIO's installation guide](https://docs.platformio.org/en/latest/integration/ide/vscode.html). PlatformIO supplies the firmware toolchain; a separate ESP-IDF installation is not needed for this workflow.

Host checks also need Python 3.9 or newer and a C compiler available as `cc`. On macOS, Apple's Command Line Tools provide `cc`; on Linux, install your distribution's C development tools. On Windows, use a Linux environment such as WSL for the host checks; native Windows host-test compatibility is not established here. Building firmware through PlatformIO is a separate step.

## Clone and open

From a terminal in the folder where you want to keep projects:

```bash
git clone https://github.com/McGill-Baja-Racing/ESP32DataLogger.git
cd ESP32DataLogger
```

Existing checkout? Open that folder instead of cloning again. In VS Code choose **File → Open Workspace from File…**, then select `baja-daq.code-workspace`.

Open a **PlatformIO Core CLI** terminal through PlatformIO in VS Code. Ordinary terminals may not have `pio` on their command search path. Set the terminal's working directory to the repository root before using the commands below.

```bash
git --version
pio --version
python3 --version
cc --version
```

Expect version information for each tool. Python and `cc` are needed only for host tests, not the initial firmware build. See [troubleshooting](TROUBLESHOOTING.md) if a command is missing.

## Make a first build

From the repository root, with PlatformIO installed and internet available for initial downloads:

```bash
pio run -d sensor-node -e NodeBrake -t buildprog
```

This selects the brake firmware and compiles it. It does **not** upload or need a board. The first build downloads packages and can take several minutes. Success ends with PlatformIO reporting `SUCCESS` and produces build artifacts under the project's `.pio/` folder.

In the PlatformIO project tasks panel, the equivalent action is **sensor-node → NodeBrake → Build**. Avoid Upload until you are working with hardware.

Build configuration comes from `platformio.ini`; generated SDK configurations are not the place to make shared configuration changes. Read [Contributing](../CONTRIBUTING.md) before changing files.

## Run a hardware-free check

With Python and `cc` installed, from the repository root:

```bash
python3 master-node/tests/test_csv_export.py
python3 master-node/tests/test_paired_csv.py
```

These compile selected firmware functions for your computer and check CSV output. Expect successful test results and exit status zero with a working host compiler. They do not start the full firmware or simulate a CAN bus. [Testing](TESTING.md) also describes the GPS/time suite and its currently known harness failure. If setup fails, check the [known local tool issues](TROUBLESHOOTING.md#known-validation-issues) before treating it as a firmware bug.

## Choose your next step

Read [First contribution](../CONTRIBUTING.md), then the [master](../master-node/README.md), [sensor](../sensor-node/README.md), or [simulator](../simulators/README.md) introduction for your task. Use the [system overview](SYSTEM_OVERVIEW.md) for concepts and the [flashing guide](FLASHING.md) only when a board is available.
