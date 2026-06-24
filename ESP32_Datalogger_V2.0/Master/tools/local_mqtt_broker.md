# Local MQTT Broker For Field Testing

Running the broker on your laptop removes the public broker and public DNS from
the test loop. The ESP32 master and the dashboard still use MQTT, but both talk
to your computer over the same Wi-Fi/hotspot network.

## 1. Install Mosquitto On macOS

```bash
brew install mosquitto
```

## 2. Start The Broker

From the `Master` project folder:

```bash
mosquitto -c tools/mosquitto_local.conf -v
```

Leave this terminal open while testing.

## 3. Find Your Laptop IP

If your Mac is connected to a phone hotspot or Wi-Fi:

```bash
ipconfig getifaddr en0
```

Use the IP it prints, for example `172.20.10.2` or `192.168.1.42`.
Do not use `localhost` for the ESP32; `localhost` would mean the ESP32 itself.

## 4. Point The Master At Your Laptop

Open the PlatformIO serial monitor and send:

```text
mqtt mqtt://YOUR_LAPTOP_IP:1883
```

Example:

```text
mqtt mqtt://172.20.10.2:1883
```

Then reboot/reset the master. The firmware reads `/sdcard/mqtt_config.json`
only during telemetry startup.

## 5. Point The Dashboard At The Same Broker

In the Streamlit sidebar:

- Broker: `YOUR_LAPTOP_IP`
- Port: `1883`
- Topic: `baja/logger/master/#`
- Command topic: `baja/logger/master/command`

Then click `Reconnect`.

## Notes

- Your Mac firewall may ask whether Mosquitto can accept incoming connections.
  Allow it, or the ESP32 will not be able to connect.
- This setup works only while your laptop and the ESP32 are on the same network.
- For remote internet use later, use an authenticated private broker/VPS instead
  of a public test broker.
