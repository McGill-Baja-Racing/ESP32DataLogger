# Wi-Fi Control Prototype

Standalone proof-of-life project for the Waveshare ESP32-P4-WIFI6 board.

The project follows the known-good `Examples/WIFI/WIFI_Example` PlatformIO setup
for this board, then adds the official ESP-IDF component-manager Wi-Fi Remote /
ESP-Hosted stack for the ESP32-P4 + ESP32-C6 SDIO path.

## Behavior

- Starts a Wi-Fi access point:
  - SSID: `BajaLogger`
  - Password: `bajalogger`
- Starts an HTTP server at:
  - `http://192.168.4.1/`
- Provides placeholder API endpoints matching the logger commands:
  - `GET /api/start`
  - `GET /api/stop`
  - `GET /api/status`
  - `GET /api/files`

This prototype does not control the logger yet. It only proves that AP mode and
HTTP serving work on the board.

## Hosted Wi-Fi Setup

The app component declares these managed dependencies in `src/idf_component.yml`:

- `espressif/esp_wifi_remote`
- `espressif/esp_hosted`

The SDK config disables the older `CONFIG_ESP_HOST_WIFI_ENABLED` path and enables:

- `CONFIG_ESP_WIFI_REMOTE_ENABLED`
- `CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED`
- `CONFIG_SLAVE_IDF_TARGET_ESP32C6`
- `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE`

## Build

From this folder:

```sh
/Users/remylaurendeau/.platformio/penv/bin/pio run -e esp32-p4
```

## Flash And Monitor

```sh
/Users/remylaurendeau/.platformio/penv/bin/pio run -e esp32-p4 -t upload
/Users/remylaurendeau/.platformio/penv/bin/pio device monitor -b 115200
```

After boot, connect a laptop/phone to `BajaLogger`, then open:

```text
http://192.168.4.1/
```
