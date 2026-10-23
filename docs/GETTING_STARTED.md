# Getting started without hardware

This guide takes you from a new local copy of the repository to a firmware build. No electronics knowledge or connected board is required. Return to the [project introduction](../README.md) at any time.

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

The firmware uses C. You can learn it through small changes to the existing code: pick a focused task, follow a similar example in the project, and ask the team when something is unfamiliar. You do not need to complete a programming course before getting involved. [Contributing](../CONTRIBUTING.md) explains the workflow from an issue to a reviewed change.

## Install tools

Install [Git](https://git-scm.com/downloads/), [VS Code](https://code.visualstudio.com/), and the **PlatformIO IDE** extension inside VS Code. Follow [PlatformIO's installation guide](https://docs.platformio.org/en/latest/integration/ide/vscode.html) if needed. PlatformIO downloads the firmware compiler and required packages; you do not need a separate ESP-IDF installation or C compiler for this build workflow.

This workflow works on Windows, macOS, and Linux. On Linux, follow PlatformIO's installation prerequisites, including your distribution's `python3-venv` package.

## Clone and open in VS Code

Before starting contribution work, ask the **Electronics Lead** to grant your GitHub account access to the team repository.

1. Open VS Code's **Source Control** panel and select **Clone Repository**. You can also open the Command Palette and choose **Git: Clone**.
2. Paste `https://github.com/McGill-Baja-Racing/ESP32DataLogger.git`, choose a local destination, and open the cloned repository.
3. Choose **File → Open Workspace from File…** and select `baja-daq.code-workspace` from the repository.
4. Allow PlatformIO to finish initializing. The workspace opens the master, sensor, and simulator projects together.

Already have a local copy of the repository? Open its workspace file instead of cloning again.

## Build with the bottom toolbar

A build compiles the firmware without putting it on a board. You can do this without hardware. The official [illustrated PlatformIO toolbar guide](https://docs.platformio.org/en/latest/integration/ide/vscode.html#platformio-toolbar) shows where to find the controls. This repository customizes some buttons and contains several projects; follow the project and environment selections below.

1. Open `sensor-node/src/main.c`. The workspace activates the project owning the file you are editing.
2. In the PlatformIO environment selector in the bottom status bar, select **NodeBrake** for the **sensor-node** project.
3. Hover over the **✓** button to confirm its tooltip is **PlatformIO: Build**, then click it.
4. Wait for the terminal output to report **SUCCESS**. Initial package downloads need internet and can take several minutes.

If you cannot find the selector or toolbar, open the PlatformIO panel (ant icon) and use **sensor-node → NodeBrake → General → Build** under Project Tasks. Confirm both project and environment before running an action: this workspace contains several projects.

If the build fails, use [Troubleshooting](TROUBLESHOOTING.md). Collect the selected project/environment, the first error, your OS, and the command or action you ran. Package downloads or tool-installation failures can occur before your source is compiled.

The firmware output is stored in the project's `.pio/` folder. Build configuration is in `platformio.ini`; intentional shared SDK settings belong in SDK defaults rather than generated configuration files.

## What the other buttons do

These are the buttons configured by this repository's workspace:

| Button | Action | When to use it |
|---|---|---|
| ✓ | Build | Compile and check that the selected firmware builds; no board required |
| → | Upload and Monitor | Build, flash the connected board, and open its serial output |
| Plug | Serial Monitor | Read the connected board's serial output without uploading again |
| Trash can | Clean | Remove generated build output when you need a clean rebuild |
| Terminal | New Terminal | Open PlatformIO's terminal for command-line tasks |

With hardware connected, select the matching environment before using **→**. Use [Flashing](FLASHING.md) for board selection, ports, and expected startup behavior. Building is not the same as running: firmware runs on the board after upload, not on your laptop.

## Open a terminal at the repository root

The **repository root** is the folder containing `README.md`, `baja-daq.code-workspace`, `master-node/`, and `sensor-node/`. The workspace names this folder **repo**. In VS Code's Explorer, right-click **repo** and choose **Open in Integrated Terminal**. For Git commands, use this terminal.

Check the location with:

```bash
git rev-parse --show-toplevel
```

This prints the repository's full path. If a terminal starts inside a component such as `sensor-node`, use `cd "FULL_REPOSITORY_PATH"`, replacing the placeholder with that printed path. Quoting the path allows spaces. Git status works in subfolders, but the `pio -d ...` and file paths in these guides assume the root.

## Optional terminal alternative

If you prefer commands, open the PlatformIO terminal, check its folder using the steps above, and run this from the repository root:

```bash
pio run -d sensor-node -e NodeBrake -t buildprog
```

This performs the same build without uploading. The explicit target also avoids the simulators' default upload/monitor actions.

## Optional tests on your computer

Some parsing and CSV checks compile selected functions for your computer rather than the ESP32. They need additional tools, unlike the normal PlatformIO build. You can skip them during initial setup and use [Testing](TESTING.md) when your task needs them. That guide includes Windows setup and current known test limitations.

## Choose your next step

Try the [guided documentation contribution](FIRST_CONTRIBUTION.md), or read the general [contribution workflow](../CONTRIBUTING.md), then the [master](../master-node/README.md), [sensor](../sensor-node/README.md), or [simulator](../simulators/README.md) introduction for your task. Use the [system overview](SYSTEM_OVERVIEW.md) for concepts and the [flashing guide](FLASHING.md) only when a board is available.
