# Raspberry Pi status

The specs and current state of the Pi used for the [driver display](../README.md). Measured on 2026-11-04. This is a snapshot, so check the Pi itself before relying on it.

## Specs

| Item | Value |
|---|---|
| Model | Raspberry Pi 3 Model B+ Rev 1.3 (`a020d3`) |
| CPU | 4 Cortex-A53 cores at 1.4 GHz, 64-bit |
| RAM | 905 MiB usable, plus 904 MiB of compressed swap |
| Storage | 29 GB root partition, 25 GB free |
| OS | Debian 13 (trixie), Raspberry Pi OS image from 2026-09-15, no desktop |
| Kernel | `6.18.50+rpt-rpi-v8` |
| Hostname | `baja` |

## Connecting

Plug in an ethernet cable and run `ssh baja@baja.local`. Ask the team for the password. Do not write it in this repository.

The Pi's ethernet uses a link-local IPv6 address that does not change, and it still asks for an IPv4 address in case it is plugged into a router.

Without this setup, the Pi took its ethernet connection down for 5 minutes after every 3 minutes without a DHCP server, which killed SSH sessions. The fix is already applied:

```bash
sudo nmcli connection modify netplan-eth0 ipv6.method link-local ipv4.dhcp-timeout 2147483647
```

## Current state

| Area | State |
|---|---|
| Display | KMS driver `vc4` active with `/dev/dri/card0`. HDMI showed as disconnected, so the screen has not been tested on this Pi. `/dev/fb0` does not exist while no screen is connected |
| Graphics software | No X11, Wayland or browser. No Python UI libraries. `libdrm2` is installed but `libdrm-dev` is not, so [`deploy.sh`](../deploy.sh) brings the headers |
| CAN | `can0` from the MCP2515 overlay, 8 MHz crystal setting. Set up on 2026-11-27, see below. No `can-utils`: the repository's tools need only Python and `ip` |
| SPI and I2C | SPI on, I2C off |
| Internet | None over the ethernet cable to a laptop |
| Wi-Fi | Radio on, country `CA`, not connected. Two saved phone hotspots |
| Clock | About a month behind. It has no battery-backed clock and needs internet to sync |
| Power | No undervoltage flagged, but only checked about a minute after boot |
| Compiler tools | `gcc` 14.2, `make`, `pkg-config`, Python 3.13 with `venv` |
| Missing tools | `git`, `cmake`, `pip`, `node` |

A wrong clock can break `apt` and HTTPS until it syncs, so connect to a hotspot before installing anything.

## Connectivity monitor

The Pi still has `baja-monitor`, an older test of phone internet quality, with 43 recordings in `/var/lib/baja-monitor/recordings/`. The service is stopped and disabled.

The installed program is newer than the `setup.sh` and `update.sh` scripts in the Pi's home folder. Running either script would replace it with an older version.

## CAN setup

Done on 2026-11-27 (the Pi's own clock read 2026-10-03). The module's wiring, bench results and every change are recorded in [the CAN module README](../hardware/can-module/README.md#current-state-on-the-pi).

`/boot/firmware/config.txt` has SPI on and, at the end under `[all]`:

```text
dtoverlay=mcp2515-can0,oscillator=8000000,interrupt=25,spimaxfrequency=1000000
```

The copy from before the overlay line is `/boot/firmware/config.txt.bak-can`. `oscillator` must match the module's crystal: after fitting the 16 MHz crystal the car needs, change it to `oscillator=16000000` and reboot.

Bring the bus up listen-only at every power-up with `make can-on` (1 Mbit/s), or `make can-on BITRATE=500000` while the 8 MHz crystal is fitted. `./build/can-monitor` then shows the bus; see [Read the CAN bus](../README.md#read-the-can-bus). To check the module itself, run `sudo python3 tools/can_check.py`, with `--no-tx` on the car.
