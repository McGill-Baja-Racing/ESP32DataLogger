# Baja Logger Cloud Server

This folder contains the first cloud relay for the Baja logger.

Current server target:

```text
Public IPv4: 138.197.132.56
MQTT:        mqtt://138.197.132.56:1883
API:         http://138.197.132.56
```

The first checkpoint is intentionally simple: public MQTT on port `1883` so the
ESP32 and Streamlit dashboard can prove they can reach the same broker from
different networks. After that works, move to authenticated/TLS MQTT and HTTPS.

## Deploy To The Droplet

From this `Master` folder on your Mac:

```bash
scp -r cloud root@138.197.132.56:/opt/baja-logger
```

Then SSH into the Droplet:

```bash
ssh root@138.197.132.56
```

Install Docker if it is not already installed:

```bash
apt update
apt install -y ca-certificates curl git ufw
curl -fsSL https://get.docker.com | sh
apt install -y docker-compose-plugin
```

Open the first test ports:

```bash
ufw allow OpenSSH
ufw allow 1883/tcp
ufw allow 80/tcp
ufw allow 8000/tcp
ufw enable
```

Start the services:

```bash
cd /opt/baja-logger/cloud
docker compose up -d --build
docker compose ps
```

## Test From The Mac

Subscribe to all Baja topics:

```bash
mosquitto_sub -h 138.197.132.56 -p 1883 -t 'baja/logger/master/#' -v
```

Publish a quick test from a second terminal:

```bash
mosquitto_pub -h 138.197.132.56 -p 1883 -t baja/logger/master/test -m hello
```

Check the upload API:

```bash
curl http://138.197.132.56/health
```

## Point The ESP32 Master At The Droplet

In the serial monitor, send:

```text
mqtt mqtt://138.197.132.56:1883
```

Then reboot the master. On boot it should print something like:

```text
Loaded MQTT broker from /sdcard/mqtt_config.json: mqtt://138.197.132.56:1883
MQTT connected
```

## Run The Streamlit Dashboard

From the `Master` folder:

```bash
.venv/bin/streamlit run tools/live_dashboard.py
```

The default broker in the dashboard is now `138.197.132.56`.

## Security TODO

This first checkpoint is open to the internet so we can test quickly. Before
field use, add:

- MQTT username/password.
- MQTT TLS on port `8883`.
- HTTPS for the API on port `443`.
- Firewall rules that close `1883` and `8000`.
- A non-default device token.
