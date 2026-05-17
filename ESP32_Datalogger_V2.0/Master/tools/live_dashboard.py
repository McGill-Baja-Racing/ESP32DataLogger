#!/usr/bin/env python3
"""Live MQTT dashboard for the Baja logger.

This dashboard subscribes to the MQTT topics published by the ESP32 master and
shows a lightweight live view in your browser.

macOS setup from the Master project folder:
    1. Create a local virtual environment if you do not already have one:
           python3 -m venv .venv
    2. Install the dashboard dependencies into that venv:
           .venv/bin/python -m pip install streamlit paho-mqtt pandas

Run:
    .venv/bin/streamlit run tools/live_dashboard.py

Default MQTT topics:
    baja/logger/master/status
    baja/logger/master/can
    baja/logger/master/command
    baja/logger/master/files
    baja/logger/master/download

The ESP32 should be connected to Wi-Fi and publishing to the same broker used by
the firmware. By default that is broker.hivemq.com on port 1883.
"""

from __future__ import annotations

import base64
import json
import queue
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from typing import Any

import pandas as pd
import streamlit as st

try:
    import paho.mqtt.client as mqtt
except ImportError as exc:
    raise SystemExit(
        "Missing dependency: paho-mqtt\n"
        "Install dashboard dependencies with:\n"
        "    .venv/bin/python -m pip install streamlit paho-mqtt pandas"
    ) from exc


DEFAULT_BROKER = "broker.hivemq.com"
DEFAULT_PORT = 1883
DEFAULT_TOPIC = "baja/logger/master/#"
DEFAULT_COMMAND_TOPIC = "baja/logger/master/command"
MAX_POINTS_PER_ID = 300
STALE_AFTER_SECONDS = 3.0


@dataclass
class CanPoint:
    received_at: float
    can_id: int
    id_hex: str
    timestamp: int
    value: int
    dlc: int
    publish_time_us: int | None


@dataclass
class DownloadTransfer:
    name: str = ""
    request_id: int | None = None
    expected_size: int = 0
    received_bytes: int = 0
    expected_chunks: int | None = None
    chunks: dict[int, bytes] = field(default_factory=dict)
    active: bool = False
    complete: bool = False
    error: str = ""
    started_at: float | None = None
    finished_at: float | None = None
    data: bytes | None = None


@dataclass
class DashboardState:
    queue: queue.Queue[tuple[str, dict[str, Any], float]] = field(default_factory=queue.Queue)
    can_points: dict[int, deque[CanPoint]] = field(
        default_factory=lambda: defaultdict(lambda: deque(maxlen=MAX_POINTS_PER_ID))
    )
    latest_can: dict[int, CanPoint] = field(default_factory=dict)
    files: list[dict[str, Any]] = field(default_factory=list)
    sd: dict[str, Any] | None = None
    status: dict[str, Any] | None = None
    download: DownloadTransfer = field(default_factory=DownloadTransfer)
    last_message_at: float | None = None
    last_files_at: float | None = None
    received_times: deque[float] = field(default_factory=lambda: deque(maxlen=500))
    broker: str = DEFAULT_BROKER
    port: int = DEFAULT_PORT
    topic: str = DEFAULT_TOPIC
    command_topic: str = DEFAULT_COMMAND_TOPIC
    connected: bool = False
    error: str = ""
    command_status: str = ""
    files_requested_once: bool = False
    client: mqtt.Client | None = None
    thread_started: bool = False


def get_state() -> DashboardState:
    if "dashboard_state" not in st.session_state:
        st.session_state.dashboard_state = DashboardState()
    return st.session_state.dashboard_state


def parse_json_payload(payload: bytes) -> dict[str, Any] | None:
    try:
        decoded = payload.decode("utf-8")
        parsed = json.loads(decoded)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None

    if not isinstance(parsed, dict):
        return None
    return parsed


def publish_command(state: DashboardState, command: str, **fields: Any) -> None:
    if state.client is None or not state.connected:
        state.command_status = "MQTT is not connected"
        return

    message = {"cmd": command}
    message.update(fields)
    payload = json.dumps(message, separators=(",", ":"))
    info = state.client.publish(state.command_topic, payload=payload, qos=0, retain=False)
    if info.rc == mqtt.MQTT_ERR_SUCCESS:
        state.command_status = f"Sent {command} command"
    else:
        state.command_status = f"Failed to send {command} command: rc={info.rc}"


def start_mqtt_client(state: DashboardState, broker: str, port: int, topic: str) -> None:
    if state.client is not None:
        state.client.loop_stop()
        state.client.disconnect()

    state.broker = broker
    state.port = port
    state.topic = topic
    state.connected = False
    state.error = ""
    state.files_requested_once = False

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

    def on_connect(
        client: mqtt.Client,
        userdata: Any,
        flags: mqtt.ConnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del userdata, flags, properties
        if reason_code == 0:
            state.connected = True
            state.error = ""
            client.subscribe(topic)
        else:
            state.connected = False
            state.error = f"MQTT connect failed: {reason_code}"

    def on_disconnect(
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: mqtt.DisconnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del client, userdata, disconnect_flags, properties
        state.connected = False
        if reason_code != 0:
            state.error = f"MQTT disconnected: {reason_code}"

    def on_message(client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        del client, userdata
        payload = parse_json_payload(message.payload)
        if payload is not None:
            state.queue.put((message.topic, payload, time.time()))

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.connect_async(broker, port, keepalive=30)
    client.loop_start()

    state.client = client
    state.thread_started = True


def reset_download(state: DashboardState, name: str, request_id: int | None) -> None:
    state.download = DownloadTransfer(
        name=name,
        request_id=request_id,
        active=True,
        started_at=time.time(),
    )


def handle_download_message(state: DashboardState, payload: dict[str, Any], received_at: float) -> None:
    event = str(payload.get("event", ""))
    name = str(payload.get("name", ""))
    request_id = payload.get("request_id")
    request_id = int(request_id) if request_id is not None else None

    if event == "begin":
        reset_download(state, name, request_id)
        state.download.expected_size = int(payload.get("size", 0) or 0)
        state.download.error = ""
        return

    download = state.download
    if request_id is not None and download.request_id is not None and request_id != download.request_id:
        return

    if event == "error":
        download.active = False
        download.complete = False
        download.finished_at = received_at
        reason = payload.get("reason", "unknown_error")
        code_name = payload.get("code_name", "")
        download.error = f"{reason} {code_name}".strip()
        return

    if event == "chunk":
        try:
            seq = int(payload["seq"])
            decoded = base64.b64decode(str(payload["data"]), validate=True)
        except (KeyError, TypeError, ValueError):
            download.error = "bad download chunk"
            return

        if not download.active:
            reset_download(state, name, request_id)
            download = state.download

        if seq not in download.chunks:
            download.chunks[seq] = decoded
            download.received_bytes += len(decoded)
        return

    if event == "end":
        download.active = False
        download.finished_at = received_at
        download.expected_chunks = int(payload.get("chunks", 0) or 0)
        expected_bytes = int(payload.get("bytes", download.expected_size) or 0)

        missing = [
            seq for seq in range(download.expected_chunks)
            if seq not in download.chunks
        ]
        if missing:
            download.complete = False
            download.error = f"missing {len(missing)} chunk(s)"
            return

        data = b"".join(download.chunks[seq] for seq in range(download.expected_chunks))
        if expected_bytes and len(data) != expected_bytes:
            download.complete = False
            download.error = f"size mismatch: got {len(data)} bytes, expected {expected_bytes}"
            return

        download.data = data
        download.received_bytes = len(data)
        download.complete = True
        download.error = ""


def drain_messages(state: DashboardState) -> int:
    count = 0
    while True:
        try:
            topic, payload, received_at = state.queue.get_nowait()
        except queue.Empty:
            break

        count += 1
        state.last_message_at = received_at
        state.received_times.append(received_at)

        if topic.endswith("/status"):
            state.status = payload
            continue

        if topic.endswith("/files"):
            files = payload.get("files", [])
            state.files = files if isinstance(files, list) else []
            sd = payload.get("sd", None)
            state.sd = sd if isinstance(sd, dict) else None
            state.last_files_at = received_at
            continue

        if topic.endswith("/download"):
            handle_download_message(state, payload, received_at)
            continue

        if topic.endswith("/can"):
            try:
                can_id = int(payload["id"])
                point = CanPoint(
                    received_at=received_at,
                    can_id=can_id,
                    id_hex=str(payload.get("id_hex", f"0x{can_id:03X}")),
                    timestamp=int(payload.get("timestamp", 0)),
                    value=int(payload.get("value", 0)),
                    dlc=int(payload.get("dlc", 0)),
                    publish_time_us=(
                        int(payload["publish_time_us"])
                        if payload.get("publish_time_us") is not None
                        else None
                    ),
                )
            except (KeyError, TypeError, ValueError):
                continue

            state.can_points[can_id].append(point)
            state.latest_can[can_id] = point

    return count


def message_rate(state: DashboardState, window_seconds: float = 5.0) -> float:
    now = time.time()
    recent = [t for t in state.received_times if now - t <= window_seconds]
    return len(recent) / window_seconds


def age_text(timestamp: float | None) -> str:
    if timestamp is None:
        return "never"
    age = time.time() - timestamp
    if age < 1:
        return f"{age * 1000:.0f} ms ago"
    return f"{age:.1f} s ago"


def logger_state(status: dict[str, Any] | None) -> tuple[str, str, int]:
    if not status:
        return "unknown", "", 0
    logger = status.get("logger", {})
    if not isinstance(logger, dict):
        return "unknown", "", 0
    return (
        str(logger.get("state", "unknown")),
        str(logger.get("file", "")),
        int(logger.get("blocks", 0) or 0),
    )


def render_status_cards(state: DashboardState) -> None:
    log_state, log_file, blocks = logger_state(state.status)
    active_nodes = 0
    node_count = 0

    if state.status:
        nodes = state.status.get("nodes", [])
        if isinstance(nodes, list):
            node_count = len(nodes)
            active_nodes = sum(1 for node in nodes if isinstance(node, dict) and node.get("active"))

    stale = (
        state.last_message_at is None
        or time.time() - state.last_message_at > STALE_AFTER_SECONDS
    )
    link_state = "stale" if stale else "live"

    col1, col2, col3, col4 = st.columns(4)
    col1.metric("MQTT", "connected" if state.connected else "offline")
    col2.metric("Feed", link_state, age_text(state.last_message_at))
    col3.metric("Logger", log_state, log_file or "no file")
    col4.metric("Nodes", f"{active_nodes}/{node_count}", f"{blocks} blocks")

    if state.error:
        st.warning(state.error)


def render_controls(state: DashboardState) -> None:
    log_state, _, _ = logger_state(state.status)
    connected = state.connected
    start_disabled = (not connected) or log_state in {"running", "stopping"}
    stop_disabled = (not connected) or log_state != "running"

    col1, col2, col3 = st.columns([1, 1, 4])
    if col1.button("Start", disabled=start_disabled, use_container_width=True):
        publish_command(state, "start")

    if col2.button("Stop", disabled=stop_disabled, use_container_width=True):
        publish_command(state, "stop")

    if state.command_status:
        col3.info(state.command_status)


def render_files(state: DashboardState) -> None:
    log_state, _, _ = logger_state(state.status)
    header_cols = st.columns([1, 5])
    if header_cols[0].button("Refresh Files", disabled=not state.connected, use_container_width=True):
        publish_command(state, "files")

    if state.last_files_at:
        header_cols[1].caption(f"Last updated {age_text(state.last_files_at)}")
    else:
        header_cols[1].caption("No SD file list received yet")

    if state.sd:
        sd_cols = st.columns(3)
        sd_cols[0].metric("SD Used", f"{state.sd.get('used_gb', 0):.2f} GB")
        sd_cols[1].metric("SD Free", f"{state.sd.get('free_gb', 0):.2f} GB")
        sd_cols[2].metric("SD Total", f"{state.sd.get('total_gb', 0):.2f} GB")

    if not state.files:
        st.info("Press Refresh Files to list SD card files.")
        return

    rows = []
    for file_info in state.files:
        if not isinstance(file_info, dict):
            continue
        size_bytes = int(file_info.get("size_bytes", 0) or 0)
        rows.append(
            {
                "File": str(file_info.get("name", "")),
                "Size MB": round(size_bytes / 1_000_000.0, 3),
                "Size bytes": size_bytes,
            }
        )

    st.dataframe(pd.DataFrame(rows), use_container_width=True, hide_index=True)

    filenames = [row["File"] for row in rows if row["File"]]
    if not filenames:
        return

    selected_file = st.selectbox("File to download", filenames)
    download_disabled = (not state.connected) or log_state != "idle"
    if st.button("Download Selected", disabled=download_disabled, use_container_width=True):
        request_id = time.time_ns() & 0xFFFFFFFF
        reset_download(state, selected_file, request_id)
        publish_command(state, "download", file=selected_file, request_id=request_id)

    if log_state != "idle":
        st.caption("Stop logging before downloading a file.")

    download = state.download
    if download.name:
        st.caption(f"Download: {download.name}")

    if download.active:
        expected = download.expected_size or 1
        progress = min(download.received_bytes / expected, 1.0)
        st.progress(
            progress,
            text=f"{download.received_bytes}/{download.expected_size or '?'} bytes",
        )

    if download.error:
        st.warning(f"Download failed: {download.error}")

    if download.complete and download.data is not None:
        st.success(f"Download ready: {download.name} ({len(download.data)} bytes)")
        st.download_button(
            "Save Downloaded File",
            data=download.data,
            file_name=download.name,
            mime="application/octet-stream",
            use_container_width=True,
        )


def render_can_table(state: DashboardState) -> None:
    rows: list[dict[str, Any]] = []
    for point in sorted(state.latest_can.values(), key=lambda item: item.can_id):
        rows.append(
            {
                "CAN ID": point.id_hex,
                "Value": point.value,
                "Timestamp": point.timestamp,
                "DLC": point.dlc,
                "Last seen": age_text(point.received_at),
            }
        )

    if rows:
        st.dataframe(pd.DataFrame(rows), use_container_width=True, hide_index=True)
    else:
        st.info("Waiting for CAN telemetry on baja/logger/master/can")


def render_chart(state: DashboardState) -> None:
    if not state.can_points:
        return

    options = {
        f"{point.id_hex} ({can_id})": can_id
        for can_id, point in sorted(state.latest_can.items())
    }
    selected_label = st.selectbox("CAN signal", options.keys())
    selected_id = options[selected_label]

    points = list(state.can_points[selected_id])
    if not points:
        return

    first_timestamp = points[0].timestamp
    chart_data = pd.DataFrame(
        {
            "time_s": [(point.timestamp - first_timestamp) / 1000.0 for point in points],
            "value": [point.value for point in points],
        }
    )
    chart_data = chart_data.set_index("time_s")
    st.line_chart(chart_data, use_container_width=True, height=320)


def render_nodes(status: dict[str, Any] | None) -> None:
    if not status:
        return

    nodes = status.get("nodes", [])
    if not isinstance(nodes, list) or not nodes:
        return

    rows: list[dict[str, Any]] = []
    for node in nodes:
        if not isinstance(node, dict):
            continue
        rows.append(
            {
                "Node": node.get("name", ""),
                "ID": node.get("node_id", ""),
                "Active": bool(node.get("active", False)),
                "Low power ack": bool(node.get("low_power_ack_seen", False)),
                "Last seen us": node.get("last_seen_rx_us", 0),
                "State CAN ID": node.get("state_can_id_hex", ""),
            }
        )

    if rows:
        st.dataframe(pd.DataFrame(rows), use_container_width=True, hide_index=True)


def main() -> None:
    st.set_page_config(page_title="Baja Logger Live", layout="wide")
    state = get_state()

    with st.sidebar:
        st.title("Baja Logger")
        broker = st.text_input("Broker", state.broker)
        port = st.number_input("Port", min_value=1, max_value=65535, value=state.port)
        topic = st.text_input("Topic", state.topic)
        state.command_topic = st.text_input("Command topic", state.command_topic)
        refresh_ms = st.slider("Refresh", 100, 2000, 250, step=50)

        if st.button("Reconnect", use_container_width=True):
            start_mqtt_client(state, broker, int(port), topic)

        st.caption(f"Message rate: {message_rate(state):.1f}/s")

    if not state.thread_started:
        start_mqtt_client(state, state.broker, state.port, state.topic)

    drain_messages(state)

    if state.connected and not state.files_requested_once:
        publish_command(state, "files")
        state.files_requested_once = True

    st.title("Baja Logger Live")
    render_status_cards(state)
    render_controls(state)

    left, right = st.columns([2, 1])
    with left:
        st.subheader("Live Signal")
        render_chart(state)

    with right:
        st.subheader("Latest CAN")
        render_can_table(state)

    st.subheader("Nodes")
    render_nodes(state.status)

    st.subheader("SD Files")
    render_files(state)

    time.sleep(refresh_ms / 1000.0)
    st.rerun()


if __name__ == "__main__":
    main()
