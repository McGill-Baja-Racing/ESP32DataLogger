import socket
import json
import datetime
import os
import requests
import time

# --- CONFIGURATION ---
UDP_IP = "0.0.0.0"
UDP_PORT = 3333

# THINGSBOARD
TB_URL = "https://thingsboard.cloud/api/v1/uv3cqj93nuqxod49ddig/telemetry"

# --- LOCAL FILE ---
home_dir = os.environ['HOME']
LOG_FILE = os.path.join(home_dir, "storage/downloads/baja_telemetry.json")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))
sock.settimeout(1.0) # Don't block forever

print(f"--- BAJA HYBRID RELAY ---")
print(f"Logging at MAX SPEED to: {LOG_FILE}")
print(f"Uploading at 1Hz to: ThingsBoard")

last_cloud_upload = 0

while True:
    try:
        # 1. Receive Data (Fast)
        try:
            data, addr = sock.recvfrom(1024)
        except socket.timeout:
            continue # Loop back if no data received

        raw_message = data.decode("utf-8").strip()
        timestamp = datetime.datetime.now().isoformat()
        
        # Try to convert to number for the Cloud
        try:
            val_float = float(raw_message)
        except:
            val_float = 0.0

        # 2. SAVE LOCAL (Always, Fast)
        # We save 'raw_message' now so you can see the text from ESP32 in the file
        record = {"time": timestamp, "rpm": val_float, "raw": raw_message}
        with open(LOG_FILE, "a") as f:
            f.write(json.dumps(record) + "\n")
            f.flush()
            os.fsync(f.fileno()) # <--- Fixed the crash here

        # 3. SEND TO CLOUD (Only once per second)
        current_time = time.time()
        if current_time - last_cloud_upload >= 1.0:
            # Only send to cloud if we actually have a number, otherwise we send 0
            tb_payload = {"rpm": val_float}
            try:
                requests.post(TB_URL, json=tb_payload, timeout=0.5)
                print(f"Cloud Upload: {val_float} (Local: {raw_message})")
                last_cloud_upload = current_time
            except Exception as e:
                print(f"Cloud Skip: {e}")

    except KeyboardInterrupt:
        break
    except Exception as e:
        print(f"Error: {e}")