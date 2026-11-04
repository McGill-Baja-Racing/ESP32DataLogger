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
| CAN | No `can0`, no MCP2515 overlay, no `can-utils` |
| SPI and I2C | Both off |
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

## To set up for CAN

Not done yet. Check [the MCP2515 module](../README.md#check-the-mcp2515-module-before-wiring) first, because `oscillator` must match its crystal and `interrupt` must match the GPIO you wire.

Add this to `/boot/firmware/config.txt`, then reboot:

```text
dtparam=spi=on
dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25
```

Install the tools and bring the bus up in listen-only mode:

```bash
sudo apt install can-utils
sudo ip link set can0 up type can bitrate 1000000 listen-only on
candump can0
```
