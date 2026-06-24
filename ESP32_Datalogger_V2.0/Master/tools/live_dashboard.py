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
    baja/logger/master/config
    baja/logger/master/health

The ESP32 should be connected to Wi-Fi and publishing to the same broker used by
the firmware. The current Baja cloud test broker is 138.197.132.56 on port 1883.

Code map:
    Dataclasses: in-memory dashboard state and transfer state.
    MQTT connection: connect, publish commands, and enqueue incoming messages.
    Download decoding: resumable chunks, binary log -> CSV, and debug report.
    Message reducers: turn MQTT messages into DashboardState.
    Render helpers/pages: Live, Health, Files, Config, and Nodes.
"""

from __future__ import annotations

import base64
import binascii
import copy
import csv
import io
import json
import math
import queue
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict, deque
from dataclasses import dataclass, field, replace
from pathlib import Path
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


DEFAULT_BROKER = "138.197.132.56"
DEFAULT_PORT = 1883
DEFAULT_CLOUD_API = "http://138.197.132.56"
DEFAULT_TOPIC = "baja/logger/master/#"
DEFAULT_COMMAND_TOPIC = "baja/logger/master/command"
DEFAULT_COMMAND_TOKEN = "baja_logger_test_v1"
MQTT_QUEUE_MAX = 1000
MAX_POINTS_PER_ID = 600
MAX_HEALTH_POINTS = 45
LIVE_CHART_DISPLAY_POINTS = 120
HEALTH_CHART_DISPLAY_POINTS = 30
DRAIN_MESSAGE_LIMIT = 500
LOCAL_SESSION_DIR = Path("tools/local_sessions")
STALE_AFTER_SECONDS = 12.0
CONTROL_CONFIRM_TIMEOUT_SECONDS = 15.0
DOWNLOAD_WINDOW_CHUNKS = 4
DOWNLOAD_RETRY_AFTER_SECONDS = 2.0
DOWNLOAD_STALL_RETRY_SECONDS = 10.0
LOG_MODE_OPTIONS = ["off", "master", "status", "node", "samples", "all"]
SENSOR_FUNCTION_OPTIONS = [
    "sim",
    "adc",
    "rpm",
    "old_rpm",
    "front_brake",
    "rear_brake",
    "gps_speed",
    "gps_latitude",
    "gps_longitude",
]
CHART_PICKER_KEY = "live_chart_signals"
PENDING_CHART_REMOVE_KEY = "pending_chart_remove_id"
SESSION_REVIEW_PICKER_KEY = "session_review_signals"
HEALTH_BOARD_PICKER_KEY = "health_boards_to_graph"
HEALTH_METRIC_PICKER_KEY = "health_metric_to_graph"


# --------------------- Dataclasses / in-memory state ---------------------


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
    chunk_raw_bytes: int = 192
    chunks: dict[int, bytes] = field(default_factory=dict)
    active: bool = False
    complete: bool = False
    error: str = ""
    started_at: float | None = None
    finished_at: float | None = None
    data: bytes | None = None
    csv_data: bytes | None = None
    csv_name: str = ""
    cloud_file_id: str = ""
    cloud_csv_url: str = ""
    cloud_bin_url: str = ""
    http_upload: bool = False
    debug_report: list[str] = field(default_factory=list)
    last_request_at: float | None = None
    last_progress_at: float | None = None
    next_seq_hint: int = 0
    next_window_seq: int = 0
    forward_scan_complete: bool = False
    ready_for_next_window: bool = False
    debug_events: deque[str] = field(default_factory=lambda: deque(maxlen=20))


@dataclass
class LocalSessionCapture:
    active: bool = False
    completed: bool = False
    log_file: str = ""
    started_at: float | None = None
    stopped_at: float | None = None
    points: dict[int, list[CanPoint]] = field(default_factory=lambda: defaultdict(list))
    total_points: int = 0
    csv_path: str = ""
    error: str = ""


@dataclass
class DashboardState:
    queue: queue.Queue[tuple[str, dict[str, Any], float]] = field(
        default_factory=lambda: queue.Queue(maxsize=MQTT_QUEUE_MAX)
    )
    can_points: dict[int, deque[CanPoint]] = field(
        default_factory=lambda: defaultdict(lambda: deque(maxlen=MAX_POINTS_PER_ID))
    )
    latest_can: dict[int, CanPoint] = field(default_factory=dict)
    latest_gps: dict[str, Any] | None = None
    last_gps_at: float | None = None
    health_latest: dict[str, dict[str, Any]] = field(default_factory=dict)
    health_history: dict[str, deque[dict[str, Any]]] = field(
        default_factory=lambda: defaultdict(lambda: deque(maxlen=MAX_HEALTH_POINTS))
    )
    files: list[dict[str, Any]] = field(default_factory=list)
    sd: dict[str, Any] | None = None
    files_error: str = ""
    cloud_files: list[dict[str, Any]] = field(default_factory=list)
    cloud_storage: dict[str, Any] | None = None
    cloud_cleanup: dict[str, Any] | None = None
    cloud_error: str = ""
    last_cloud_files_at: float | None = None
    status: dict[str, Any] | None = None
    config_text: str = ""
    config_status: str = ""
    pending_config_save_text: str = ""
    download: DownloadTransfer = field(default_factory=DownloadTransfer)
    last_message_at: float | None = None
    last_files_at: float | None = None
    last_config_at: float | None = None
    received_times: deque[float] = field(default_factory=lambda: deque(maxlen=500))
    broker: str = DEFAULT_BROKER
    port: int = DEFAULT_PORT
    topic: str = DEFAULT_TOPIC
    command_topic: str = DEFAULT_COMMAND_TOPIC
    command_token: str = DEFAULT_COMMAND_TOKEN
    connected: bool = False
    error: str = ""
    command_status: str = ""
    pending_control_command: str = ""
    pending_control_target: str = ""
    pending_control_sent_at: float | None = None
    local_session: LocalSessionCapture = field(default_factory=LocalSessionCapture)
    selected_chart_ids: list[int] = field(default_factory=list)
    preview_chart_ids: tuple[int, ...] | None = None
    live_chart_window_s: float = 5.0
    live_chart_y_ranges: dict[int, tuple[float | None, float | None]] = field(default_factory=dict)
    brake_pressure_offsets: dict[str, int] = field(default_factory=dict)
    files_requested_once: bool = False
    config_requested_once: bool = False
    client: mqtt.Client | None = None
    thread_started: bool = False


# --------------------- Streamlit setup and MQTT connection ---------------------


def get_state() -> DashboardState:
    if "dashboard_state" not in st.session_state:
        st.session_state.dashboard_state = DashboardState()
    return st.session_state.dashboard_state


def hide_streamlit_chrome() -> None:
    st.markdown(
        """
        <style>
        [data-testid="stToolbar"],
        [data-testid="stStatusWidget"],
        [data-testid="stDecoration"],
        #MainMenu,
        footer {
            visibility: hidden;
            height: 0;
        }
        header {
            visibility: hidden;
        }
        </style>
        """,
        unsafe_allow_html=True,
    )


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

    message = {"cmd": command, "token": state.command_token}
    message.update(fields)
    payload = json.dumps(message, separators=(",", ":"))
    info = state.client.publish(state.command_topic, payload=payload, qos=0, retain=False)
    if info.rc == mqtt.MQTT_ERR_SUCCESS:
        state.command_status = f"Sent {command} command"
    else:
        state.command_status = f"Failed to send {command} command: rc={info.rc}"


def enqueue_mqtt_message(state: DashboardState, topic: str, payload: dict[str, Any]) -> None:
    item = (topic, payload, time.time())
    try:
        state.queue.put_nowait(item)
        return
    except queue.Full:
        pass

    try:
        state.queue.get_nowait()
    except queue.Empty:
        pass

    try:
        state.queue.put_nowait(item)
    except queue.Full:
        pass


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
    state.config_requested_once = False
    state.preview_chart_ids = None

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
            state.preview_chart_ids = None
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
            enqueue_mqtt_message(state, message.topic, payload)

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.connect_async(broker, port, keepalive=10)
    client.loop_start()

    state.client = client
    state.thread_started = True


# --------------------- Resumable download and log decoding ---------------------


def reset_download(state: DashboardState, name: str, request_id: int | None) -> None:
    now = time.time()
    state.download = DownloadTransfer(
        name=name,
        request_id=request_id,
        active=True,
        started_at=now,
        last_request_at=now,
        last_progress_at=now,
    )
    state.download.debug_events.append(
        f"{time.strftime('%H:%M:%S')} reset name={name} request={request_id}"
    )


def request_download_window(state: DashboardState, start_seq: int, reason: str) -> None:
    download = state.download
    if not download.name:
        return

    download.active = True
    download.ready_for_next_window = False
    download.next_window_seq = max(download.next_window_seq, start_seq)
    download.last_request_at = time.time()
    download.error = ""
    download.debug_events.append(
        f"{time.strftime('%H:%M:%S')} request start_seq={start_seq} reason={reason} "
        f"next_window={download.next_window_seq} next_hint={download.next_seq_hint} "
        f"chunks={len(download.chunks)}/{download.expected_chunks or '?'} "
        f"forward_done={download.forward_scan_complete}"
    )
    publish_command(
        state,
        "download",
        file=download.name,
        request_id=download.request_id,
        start_seq=start_seq,
        max_chunks=DOWNLOAD_WINDOW_CHUNKS,
    )


def download_stalled(download: DownloadTransfer) -> bool:
    last_progress = max(
        timestamp
        for timestamp in (
            download.started_at,
            download.last_request_at,
            download.last_progress_at,
        )
        if timestamp is not None
    )
    return time.time() - last_progress > DOWNLOAD_STALL_RETRY_SECONDS


def first_missing_download_seq(download: DownloadTransfer) -> int | None:
    if download.expected_chunks is None:
        return None
    for seq in range(download.expected_chunks):
        if seq not in download.chunks:
            return seq
    return None


def next_download_request_seq(download: DownloadTransfer) -> int | None:
    if download.expected_chunks is None:
        return max(download.next_window_seq, download.next_seq_hint)

    if not download.forward_scan_complete and download.next_window_seq < download.expected_chunks:
        return download.next_window_seq

    download.forward_scan_complete = True
    return first_missing_download_seq(download)


def complete_download_if_ready(state: DashboardState) -> None:
    download = state.download
    if download.complete or download.expected_chunks is None:
        return

    if first_missing_download_seq(download) is not None:
        return

    data = b"".join(download.chunks[seq] for seq in range(download.expected_chunks))
    if download.expected_size and len(data) != download.expected_size:
        download.error = f"size mismatch: got {len(data)} bytes, expected {download.expected_size}"
        return

    download.data = data
    download.csv_data, download.csv_name, download.debug_report = debug_and_convert_log(
        data,
        download.name,
        state.status,
    )
    download.received_bytes = len(data)
    download.active = False
    download.complete = True
    download.error = ""
    download.finished_at = time.time()


def csv_filename_for_log(name: str) -> str:
    if name.lower().endswith(".bin"):
        return f"{name[:-4]}.csv"
    return f"{name}.csv"


def safe_local_session_name(name: str) -> str:
    stem = Path(name or "mqtt_session").stem or "mqtt_session"
    safe = "".join(char if char.isalnum() or char in {"-", "_"} else "_" for char in stem)
    return safe[:80] or "mqtt_session"


def clear_live_session_preview(state: DashboardState) -> None:
    state.can_points = defaultdict(lambda: deque(maxlen=MAX_POINTS_PER_ID))
    state.latest_can = {}
    state.latest_gps = None
    state.last_gps_at = None


def start_local_session_capture(state: DashboardState, log_file: str) -> None:
    session = state.local_session
    if session.active and session.log_file == log_file:
        return

    clear_live_session_preview(state)
    state.local_session = LocalSessionCapture(
        active=True,
        completed=False,
        log_file=log_file,
        started_at=time.time(),
    )


def append_local_session_points(state: DashboardState, points: list[CanPoint]) -> None:
    session = state.local_session
    if not session.active or not points:
        return

    for point in points:
        session.points[point.can_id].append(point)
        session.total_points += 1


def local_session_points(session: LocalSessionCapture) -> list[CanPoint]:
    points: list[CanPoint] = []
    for signal_points in session.points.values():
        points.extend(signal_points)
    return sorted(points, key=lambda point: (point.timestamp, point.received_at, point.can_id))


def write_local_session_csv(
    session: LocalSessionCapture,
    status: dict[str, Any] | None,
) -> tuple[str, str]:
    LOCAL_SESSION_DIR.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime(
        "%Y%m%d_%H%M%S",
        time.localtime(session.started_at or time.time()),
    )
    csv_path = LOCAL_SESSION_DIR / f"{timestamp}_{safe_local_session_name(session.log_file)}_preview.csv"
    metadata = sensor_metadata_by_can_id(status)

    with csv_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "sample_index",
                "can_id",
                "can_id_hex",
                "signal",
                "node",
                "timestamp_ms",
                "value",
                "units",
                "received_at_unix",
                "publish_time_us",
            ]
        )
        for index, point in enumerate(local_session_points(session)):
            sensor = metadata.get(point.can_id, {})
            value = display_value(point.value, sensor)
            writer.writerow(
                [
                    index,
                    point.can_id,
                    point.id_hex,
                    sensor.get("name", ""),
                    sensor.get("node", ""),
                    point.timestamp,
                    value,
                    display_units(sensor),
                    f"{point.received_at:.6f}",
                    point.publish_time_us if point.publish_time_us is not None else "",
                ]
            )

    return str(csv_path), ""


def finish_local_session_capture(state: DashboardState) -> None:
    session = state.local_session
    if not session.active:
        return

    session.active = False
    session.completed = True
    session.stopped_at = time.time()

    if session.total_points == 0:
        session.error = "No MQTT preview samples were received during this session."
        return

    try:
        session.csv_path, session.error = write_local_session_csv(session, state.status)
    except OSError as exc:
        session.error = f"Could not save local session CSV: {exc}"


def sync_local_session_capture_from_status(state: DashboardState) -> None:
    log_state, log_file, _ = logger_state(state.status)
    if log_state == "running":
        start_local_session_capture(state, log_file)
        return

    if state.local_session.active and log_state in {"idle", "unknown"}:
        finish_local_session_capture(state)


def fetch_cloud_file(url: str, timeout_s: float = 30.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "baja-logger-dashboard"})
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return response.read()


def fetch_cloud_json(path: str, timeout_s: float = 10.0) -> dict[str, Any]:
    url = f"{DEFAULT_CLOUD_API}{path}"
    request = urllib.request.Request(url, headers={"User-Agent": "baja-logger-dashboard"})
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return json.loads(response.read().decode("utf-8"))


def delete_cloud_upload(file_id: str, token: str, timeout_s: float = 10.0) -> dict[str, Any]:
    safe_file_id = urllib.parse.quote(str(file_id).strip(), safe="")
    safe_token = urllib.parse.quote(str(token), safe="")
    headers = {"User-Agent": "baja-logger-dashboard"}
    errors: list[str] = []

    for method, path in (
        ("DELETE", f"/files/{safe_file_id}?token={safe_token}"),
        ("POST", f"/files/{safe_file_id}/delete?token={safe_token}"),
        ("POST", f"/delete-upload?file_id={safe_file_id}&token={safe_token}"),
    ):
        url = f"{DEFAULT_CLOUD_API}{path}"
        request = urllib.request.Request(url, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout_s) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace")
            errors.append(f"{method} {url} -> HTTP {exc.code}: {body}")
            if exc.code == 404 and "unknown file id" in body.lower():
                return {"ok": True, "file_id": file_id, "already_deleted": True}
            if exc.code not in {404, 405}:
                raise

    raise RuntimeError("; ".join(errors))


def refresh_cloud_files(state: DashboardState) -> None:
    try:
        payload = fetch_cloud_json("/files")
    except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
        state.cloud_error = str(exc)
        return

    state.cloud_files = list(payload.get("files", []) or [])
    state.cloud_storage = payload.get("storage")
    state.cloud_cleanup = payload.get("cleanup")
    state.cloud_error = ""
    state.last_cloud_files_at = time.time()


def format_mb(size_bytes: Any) -> str:
    try:
        return f"{int(size_bytes or 0) / 1_000_000:.2f} MB"
    except (TypeError, ValueError):
        return "0.00 MB"


def signed_u32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def debug_and_convert_log(data: bytes, name: str, status: dict[str, Any] | None) -> tuple[bytes, str, list[str]]:
    metadata = sensor_metadata_by_can_id(status)
    report: list[str] = []
    record_size = 16

    if not data:
        return b"", csv_filename_for_log(name), ["File is empty."]

    full_records = len(data) // record_size
    trailing_bytes = len(data) % record_size
    if trailing_bytes:
        report.append(
            f"File has {trailing_bytes} trailing byte(s); only complete 16-byte records were decoded."
        )

    if full_records == 0:
        return b"", csv_filename_for_log(name), report + ["No complete log records found."]

    counts_by_id: dict[int, int] = defaultdict(int)
    last_timestamp_by_id: dict[int, int] = {}
    first_timestamp: int | None = None
    last_timestamp: int | None = None
    timestamp_resets = 0
    duplicate_timestamps = 0
    large_gaps = 0

    text = io.StringIO()
    writer = csv.writer(text)
    writer.writerow(
        [
            "sample_index",
            "can_id",
            "can_id_hex",
            "signal",
            "node",
            "timestamp_ms",
            "value",
            "raw_data",
        ]
    )

    for index in range(full_records):
        offset = index * record_size
        can_id_raw, packed_raw = struct.unpack_from("<qq", data, offset)
        can_id = int(can_id_raw) & 0x7FF
        packed = int(packed_raw) & 0xFFFFFFFFFFFFFFFF
        value = signed_u32(packed)
        timestamp = (packed >> 32) & 0xFFFFFFFF
        sensor = metadata.get(can_id, {})
        decoded_value = value
        if is_gps_speed_sensor(sensor):
            decoded_value = value / 100.0
            sensor = {**sensor, "units": "km/h"}
        first_timestamp = timestamp if first_timestamp is None else min(first_timestamp, timestamp)
        last_timestamp = timestamp if last_timestamp is None else max(last_timestamp, timestamp)

        previous_timestamp = last_timestamp_by_id.get(can_id)
        if previous_timestamp is not None:
            if timestamp < previous_timestamp:
                timestamp_resets += 1
            elif timestamp == previous_timestamp:
                duplicate_timestamps += 1
            elif timestamp - previous_timestamp > 1000:
                large_gaps += 1

        last_timestamp_by_id[can_id] = timestamp
        counts_by_id[can_id] += 1

        writer.writerow(
            [
                index,
                can_id,
                f"0x{can_id:03X}",
                sensor.get("name", ""),
                sensor.get("node", ""),
                timestamp,
                decoded_value,
                packed,
            ]
        )

    duration_ms = (
        last_timestamp - first_timestamp
        if first_timestamp is not None and last_timestamp is not None
        else 0
    )

    report.append(f"Decoded {full_records} complete sample record(s).")
    id_summary = ", ".join(f"0x{k:03X}={v}" for k, v in sorted(counts_by_id.items()))
    report.append(f"Found {len(counts_by_id)} CAN ID(s): {id_summary}.")
    if duration_ms > 0:
        report.append(f"Approximate timestamp span from latest per-signal samples: {duration_ms} ms.")
    if timestamp_resets:
        report.append(f"Warning: {timestamp_resets} timestamp decrease(s) seen within a CAN ID.")
    if duplicate_timestamps:
        report.append(f"Note: {duplicate_timestamps} duplicate timestamp(s) seen within a CAN ID.")
    if large_gaps:
        report.append(f"Note: {large_gaps} gap(s) over 1000 ms seen within a CAN ID.")
    if not any(item.startswith("Warning:") for item in report):
        report.append("No obvious binary decode problems found.")

    return text.getvalue().encode("utf-8"), csv_filename_for_log(name), report


def handle_download_message(state: DashboardState, payload: dict[str, Any], received_at: float) -> None:
    event = str(payload.get("event", ""))
    name = str(payload.get("name", ""))
    request_id = payload.get("request_id")
    request_id = int(request_id) if request_id is not None else None

    if event == "begin":
        download = state.download
        same_transfer = (
            download.name == name
            and request_id is not None
            and download.request_id == request_id
        )
        if not same_transfer:
            reset_download(state, name, request_id)
            download = state.download

        download.expected_size = int(payload.get("size", 0) or 0)
        download.expected_chunks = int(payload.get("chunks", 0) or 0) or None
        download.chunk_raw_bytes = int(payload.get("chunk_raw_bytes", download.chunk_raw_bytes) or 192)
        download.active = True
        download.complete = False
        download.ready_for_next_window = False
        if download.expected_chunks is not None and download.next_window_seq >= download.expected_chunks:
            download.forward_scan_complete = True
        download.error = ""
        download.last_progress_at = received_at
        return

    download = state.download
    if request_id is not None and download.request_id is not None and request_id != download.request_id:
        return

    if event == "upload_progress":
        if not download.name:
            reset_download(state, name, request_id)
            download = state.download

        download.http_upload = True
        download.active = True
        download.complete = False
        download.cloud_file_id = str(payload.get("file_id", download.cloud_file_id))
        download.expected_size = int(payload.get("size", download.expected_size) or 0)
        download.received_bytes = int(payload.get("uploaded", download.received_bytes) or 0)
        download.last_progress_at = received_at
        download.error = ""
        download.debug_events.append(
            f"{time.strftime('%H:%M:%S')} http progress "
            f"{download.received_bytes}/{download.expected_size or '?'} file_id={download.cloud_file_id}"
        )
        return

    if event == "upload_end":
        download.http_upload = True
        download.active = False
        download.complete = True
        download.finished_at = received_at
        download.cloud_file_id = str(payload.get("file_id", download.cloud_file_id))
        download.cloud_csv_url = str(payload.get("csv_url", ""))
        download.cloud_bin_url = str(payload.get("bin_url", ""))
        download.expected_size = int(payload.get("bytes", download.expected_size) or 0)
        download.received_bytes = download.expected_size
        download.last_progress_at = received_at
        download.error = ""
        download.debug_events.append(
            f"{time.strftime('%H:%M:%S')} http upload complete file_id={download.cloud_file_id}"
        )

        if download.cloud_csv_url:
            try:
                download.csv_data = fetch_cloud_file(download.cloud_csv_url)
                download.csv_name = csv_filename_for_log(download.name)
                download.debug_report = ["HTTP upload completed; CSV generated by the Droplet."]
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                download.complete = False
                download.error = f"CSV fetch failed: {exc}"
        return

    if event == "error":
        download.active = False
        download.complete = False
        download.ready_for_next_window = False
        download.finished_at = received_at
        reason = payload.get("reason", "unknown_error")
        code_name = payload.get("code_name", "")
        download.error = f"{reason} {code_name}".strip()
        download.last_progress_at = received_at
        return

    if event == "chunk":
        try:
            seq = int(payload["seq"])
            decoded = base64.b64decode(str(payload["data"]), validate=True)
        except (KeyError, TypeError, ValueError, binascii.Error):
            download.error = "bad download chunk"
            return

        if not download.active:
            reset_download(state, name, request_id)
            download = state.download

        if seq not in download.chunks:
            download.chunks[seq] = decoded
            download.received_bytes += len(decoded)
            download.next_seq_hint = max(download.next_seq_hint, seq + 1)
            download.last_progress_at = received_at
        return

    if event == "partial":
        download.active = False
        download.complete = False
        download.ready_for_next_window = True
        download.finished_at = received_at
        download.expected_chunks = int(payload.get("chunks", download.expected_chunks or 0) or 0) or download.expected_chunks
        download.next_seq_hint = int(payload.get("next_seq", download.next_seq_hint) or download.next_seq_hint)
        download.next_window_seq = max(download.next_window_seq, download.next_seq_hint)
        if download.expected_chunks is not None and download.next_window_seq >= download.expected_chunks:
            download.forward_scan_complete = True
        download.error = ""
        download.last_progress_at = received_at
        return

    if event == "end":
        download.active = False
        download.ready_for_next_window = False
        download.finished_at = received_at
        download.last_progress_at = received_at
        download.expected_chunks = int(payload.get("chunks", 0) or 0)
        download.next_window_seq = max(download.next_window_seq, download.expected_chunks)
        download.forward_scan_complete = True
        end_bytes = int(payload.get("bytes", 0) or 0)
        expected_bytes = download.expected_size or end_bytes
        if expected_bytes and end_bytes != expected_bytes:
            download.complete = False
            download.error = f"incomplete transfer: got {end_bytes} bytes, expected {expected_bytes}"
            return

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

        if download.expected_size > 0 and not data:
            download.complete = False
            download.error = f"empty download: expected {download.expected_size} bytes"
            return

        download.data = data
        download.csv_data, download.csv_name, download.debug_report = debug_and_convert_log(
            data,
            download.name,
            state.status,
        )
        download.received_bytes = len(data)
        download.complete = True
        download.error = ""


# --------------------- MQTT message reducers ---------------------


def health_board_key(payload: dict[str, Any]) -> str:
    source = str(payload.get("source", "board"))
    node_id = payload.get("node_id", payload.get("board_id", "unknown"))
    return f"{source}:{node_id}"


def handle_health_message(state: DashboardState, payload: dict[str, Any], received_at: float) -> None:
    key = health_board_key(payload)
    entry = dict(payload)
    entry["_received_at"] = received_at
    state.health_latest[key] = entry
    state.health_history[key].append(entry)


def can_point_from_payload(payload: dict[str, Any], received_at: float) -> CanPoint | None:
    try:
        can_id = int(payload["id"])
        return CanPoint(
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
        return None


def can_points_from_payload(payload: dict[str, Any], received_at: float) -> list[CanPoint]:
    frames = payload.get("frames")
    if isinstance(frames, list):
        points: list[CanPoint] = []
        for frame in frames:
            if not isinstance(frame, dict):
                continue
            frame_payload = frame
            if "publish_time_us" not in frame_payload and payload.get("publish_time_us") is not None:
                frame_payload = dict(frame_payload)
                frame_payload["publish_time_us"] = payload.get("publish_time_us")

            point = can_point_from_payload(frame_payload, received_at)
            if point is not None:
                points.append(point)
        return points

    point = can_point_from_payload(payload, received_at)
    return [point] if point is not None else []


def drain_messages(state: DashboardState, max_messages: int = DRAIN_MESSAGE_LIMIT) -> int:
    count = 0
    latest_health: dict[str, tuple[dict[str, Any], float]] = {}
    latest_gps: tuple[dict[str, Any], float] | None = None

    while count < max_messages:
        try:
            topic, payload, received_at = state.queue.get_nowait()
        except queue.Empty:
            break

        count += 1
        state.last_message_at = received_at
        state.received_times.append(received_at)

        if topic.endswith("/status"):
            state.status = payload
            update_pending_control_from_status(state)
            sync_local_session_capture_from_status(state)
            continue

        if topic.endswith("/files"):
            if payload.get("ok") is False:
                state.files_error = str(payload.get("error", "file list request failed"))
                state.last_files_at = received_at
                continue
            files = payload.get("files", [])
            state.files = files if isinstance(files, list) else []
            sd = payload.get("sd", None)
            state.sd = sd if isinstance(sd, dict) else None
            state.files_error = ""
            state.last_files_at = received_at
            continue

        if topic.endswith("/download"):
            handle_download_message(state, payload, received_at)
            continue

        if topic.endswith("/config"):
            event = str(payload.get("event", ""))
            ok = bool(payload.get("ok", False))
            if event == "config" and ok:
                config_text = str(payload.get("config_text", ""))
                if state.pending_config_save_text:
                    if config_text == state.pending_config_save_text:
                        state.pending_config_save_text = ""
                        state.config_text = config_text
                        state.last_config_at = received_at
                        state.config_status = f"Saved config to {payload.get('source', 'master')}"
                    else:
                        state.config_status = "Ignoring stale config while save is pending"
                    continue

                state.config_text = config_text
                state.last_config_at = received_at
                state.config_status = f"Loaded config from {payload.get('source', 'master')}"
            else:
                code_name = str(payload.get("code_name", ""))
                if event == "save":
                    if ok:
                        byte_count = payload.get("bytes", "")
                        suffix = f" ({byte_count} bytes)" if byte_count != "" else ""
                        state.config_status = f"save: ok{suffix}, waiting for updated config"
                    else:
                        state.pending_config_save_text = ""
                        state.config_status = f"save failed: {code_name}"
                    continue
                state.config_status = f"{event or 'config'}: {'ok' if ok else code_name}"
            continue

        if topic.endswith("/health"):
            latest_health[health_board_key(payload)] = (payload, received_at)
            continue

        if topic.endswith("/gps"):
            latest_gps = (payload, received_at)
            continue

        if topic.endswith("/can"):
            can_points = can_points_from_payload(payload, received_at)
            for point in can_points:
                state.can_points[point.can_id].append(point)
                state.latest_can[point.can_id] = point
            append_local_session_points(state, can_points)

    for payload, received_at in latest_health.values():
        handle_health_message(state, payload, received_at)

    if latest_gps is not None:
        state.latest_gps, state.last_gps_at = latest_gps

    return count


# --------------------- Small formatting and lookup helpers ---------------------


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


def feed_is_stale(state: DashboardState) -> bool:
    return (
        state.last_message_at is None
        or time.time() - state.last_message_at > STALE_AFTER_SECONDS
    )


def command_path_ready(state: DashboardState) -> bool:
    return state.connected


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


def clear_pending_control(state: DashboardState) -> None:
    state.pending_control_command = ""
    state.pending_control_target = ""
    state.pending_control_sent_at = None


def mark_pending_control(state: DashboardState, command: str, target_state: str) -> None:
    state.pending_control_command = command
    state.pending_control_target = target_state
    state.pending_control_sent_at = time.time()


def update_pending_control_from_status(state: DashboardState) -> None:
    if not state.pending_control_command:
        return

    log_state, _, _ = logger_state(state.status)
    if log_state == state.pending_control_target:
        state.command_status = (
            f"{state.pending_control_command.title()} confirmed: logger is {log_state}"
        )
        clear_pending_control(state)
        return

    sent_at = state.pending_control_sent_at
    if sent_at is not None and time.time() - sent_at > CONTROL_CONFIRM_TIMEOUT_SECONDS:
        state.command_status = (
            f"{state.pending_control_command.title()} not confirmed after "
            f"{CONTROL_CONFIRM_TIMEOUT_SECONDS:.0f}s; logger is {log_state}"
        )
        clear_pending_control(state)


def logger_log_mode(status: dict[str, Any] | None) -> str:
    if not status:
        return "off"

    logger = status.get("logger", {})
    if not isinstance(logger, dict):
        return "off"

    mode = str(logger.get("log_mode", "off"))
    return mode if mode in LOG_MODE_OPTIONS else "off"


def sensor_metadata_by_can_id(status: dict[str, Any] | None) -> dict[int, dict[str, str]]:
    if not status:
        return {}

    metadata: dict[int, dict[str, str]] = {}
    master_sensors = status.get("master_sensors", [])
    if isinstance(master_sensors, list):
        for sensor in master_sensors:
            if not isinstance(sensor, dict):
                continue
            try:
                can_id = int(sensor.get("can_id", 0))
            except (TypeError, ValueError):
                continue
            metadata[can_id] = {
                "name": str(sensor.get("name", f"0x{can_id:03X}")),
                "units": str(sensor.get("units", "")),
                "node": "master",
                "function": str(sensor.get("function", "")),
            }

    nodes = status.get("nodes", [])
    if not isinstance(nodes, list):
        return metadata

    for node in nodes:
        if not isinstance(node, dict):
            continue
        node_name = str(node.get("name", ""))
        sensors = node.get("sensors", [])
        if not isinstance(sensors, list):
            continue
        for sensor in sensors:
            if not isinstance(sensor, dict):
                continue
            try:
                can_id = int(sensor.get("can_id", 0))
            except (TypeError, ValueError):
                continue
            metadata[can_id] = {
                "name": str(sensor.get("name", f"0x{can_id:03X}")),
                "units": str(sensor.get("units", "")),
                "node": node_name,
                "function": str(sensor.get("function", "")),
            }

    return metadata


def is_gps_speed_sensor(sensor: dict[str, str]) -> bool:
    return (
        str(sensor.get("function", "")).lower() == "gps_speed"
        or str(sensor.get("units", "")).lower() == "kph_x100"
    )


def display_units(sensor: dict[str, str]) -> str:
    if is_gps_speed_sensor(sensor):
        return "km/h"
    return str(sensor.get("units", ""))


def display_value(value: int | float, sensor: dict[str, str]) -> int | float:
    if is_gps_speed_sensor(sensor):
        return float(value) / 100.0
    return value


def format_display_value(value: int | float, sensor: dict[str, str]) -> str:
    if is_gps_speed_sensor(sensor):
        return f"{float(value):.2f}"
    return f"{value:g}" if isinstance(value, float) else str(value)


def brake_pressure_side(sensor: dict[str, str]) -> str:
    function_name = sensor.get("function", "").lower()
    name = sensor.get("name", "").lower()
    if (
        function_name == "front_brake"
        or "front_brake" in name
        or ("front" in name and "brake" in name)
    ):
        return "front"
    if (
        function_name in {"rear_brake", "rear_br"}
        or "rear_brake" in name
        or ("rear" in name and "brake" in name)
    ):
        return "rear"
    return ""


def adjusted_brake_pressure_value(
    value: int,
    sensor: dict[str, str],
    offsets: dict[str, int],
) -> int:
    side = brake_pressure_side(sensor)
    if not side:
        return value
    return value - int(offsets.get(side, 0))


def adjusted_brake_pressure_points(
    points: list[CanPoint],
    sensor: dict[str, str],
    offsets: dict[str, int],
) -> list[CanPoint]:
    side = brake_pressure_side(sensor)
    if not side:
        return points

    offset = int(offsets.get(side, 0))
    if offset == 0:
        return points
    return [replace(point, value=point.value - offset) for point in points]


# --------------------- Shared page header and controls ---------------------


def render_status_cards(state: DashboardState) -> None:
    log_state, log_file, blocks = logger_state(state.status)
    active_nodes = 0
    node_count = 0

    if state.status:
        nodes = state.status.get("nodes", [])
        if isinstance(nodes, list):
            node_count = len(nodes)
            active_nodes = sum(1 for node in nodes if isinstance(node, dict) and node.get("active"))

    stale = feed_is_stale(state)
    link_state = "stale" if stale else "live"

    col1, col2, col3, col4 = st.columns(4)
    col1.metric("MQTT", "connected" if state.connected else "offline")
    col2.metric("Feed", link_state, age_text(state.last_message_at))
    col3.metric("Logger", log_state, log_file or "no file")
    col4.metric("Nodes", f"{active_nodes}/{node_count}", f"{blocks} blocks")

    if state.error:
        st.warning(state.error)

    if state.connected and stale:
        st.warning(
            "ESP32 feed is stale. Remote commands may not reach the master; "
            "commands will still be sent through the broker."
        )


def render_controls(state: DashboardState) -> None:
    update_pending_control_from_status(state)
    log_state, _, _ = logger_state(state.status)
    ready = command_path_ready(state)
    control_pending = bool(state.pending_control_command)
    start_disabled = control_pending or (not ready) or log_state in {"running", "stopping"}
    stop_disabled = control_pending or (not ready) or log_state != "running"

    col1, col2, col3 = st.columns([1, 1, 4])
    if col1.button("Start", disabled=start_disabled, use_container_width=True):
        publish_command(state, "start")
        if state.command_status.startswith("Sent start"):
            mark_pending_control(state, "start", "running")
            state.command_status = "Start sent; waiting for logger to report running"

    if col2.button("Stop", disabled=stop_disabled, use_container_width=True):
        publish_command(state, "stop")
        if state.command_status.startswith("Sent stop"):
            mark_pending_control(state, "stop", "idle")
            state.command_status = "Stop sent; waiting for logger to report idle"

    if state.command_status:
        col3.info(state.command_status)

    with st.expander("Debug Logging", expanded=False):
        current_mode = logger_log_mode(state.status)
        mode = st.selectbox(
            "Mode",
            LOG_MODE_OPTIONS,
            index=LOG_MODE_OPTIONS.index(current_mode),
            key="debug_log_mode",
        )
        st.caption(
            "off keeps tests quiet. master prints master RX samples. status prints low-rate node status. "
            "node prints node TX samples. samples prints master RX + node TX samples. all enables every debug stream."
        )
        if st.button("Apply Log Mode", disabled=not ready, use_container_width=True):
            publish_command(state, "log", mode=mode)


# --------------------- Files page ---------------------


def render_download_panel(state: DashboardState, ready: bool, log_state: str) -> None:
    download = state.download
    if not download.name:
        return

    st.caption(f"Download: {download.name}")
    with st.expander("Download Debug", expanded=False):
        st.write(
            {
                "request_id": download.request_id,
                "active": download.active,
                "complete": download.complete,
                "ready_for_next_window": download.ready_for_next_window,
                "forward_scan_complete": download.forward_scan_complete,
                "next_window_seq": download.next_window_seq,
                "next_seq_hint": download.next_seq_hint,
                "expected_chunks": download.expected_chunks,
                "http_upload": download.http_upload,
                "cloud_file_id": download.cloud_file_id,
                "cloud_csv_url": download.cloud_csv_url,
                "received_chunks": len(download.chunks),
                "received_bytes": download.received_bytes,
                "last_request_age_s": (
                    round(time.time() - download.last_request_at, 2)
                    if download.last_request_at is not None
                    else None
                ),
                "last_progress_age_s": (
                    round(time.time() - download.last_progress_at, 2)
                    if download.last_progress_at is not None
                    else None
                ),
            }
        )
        for event in reversed(download.debug_events):
            st.code(event)

    complete_download_if_ready(state)

    if (
        ready
        and download.name
        and not download.http_upload
        and not download.complete
        and log_state == "idle"
    ):
        request_seq = next_download_request_seq(download)
        now = time.time()
        can_request_next_window = (
            not download.active
            and download.ready_for_next_window
            and download.last_request_at is not None
            and now - download.last_request_at > DOWNLOAD_RETRY_AFTER_SECONDS
        )
        can_recover_stalled_window = download.active and download_stalled(download)

        if request_seq is not None and (can_request_next_window or can_recover_stalled_window):
            if can_recover_stalled_window:
                download.active = False
                download.error = "download stalled; retrying from missing chunk"
            request_download_window(
                state,
                request_seq,
                "stall" if can_recover_stalled_window else "next-window",
            )

    if download.active:
        expected = download.expected_size or 1
        progress = min(download.received_bytes / expected, 1.0)
        st.progress(
            progress,
            text=f"{download.received_bytes}/{download.expected_size or '?'} bytes",
        )
        if download.http_upload:
            st.caption("ESP32 is uploading the log to the Droplet over HTTP.")
        else:
            st.caption("Download resumes automatically if the MQTT connection drops.")

    if download.error:
        st.warning(f"Download failed: {download.error}")

    if download.complete and (download.data or download.csv_data or download.cloud_csv_url):
        st.success(f"Download ready: {download.name} ({download.received_bytes} bytes)")

        if download.debug_report:
            with st.expander("Debug Report", expanded=True):
                for line in download.debug_report:
                    st.write(line)

        if download.csv_data:
            st.download_button(
                "Download CSV",
                data=download.csv_data,
                file_name=download.csv_name or csv_filename_for_log(download.name),
                mime="text/csv",
                use_container_width=True,
            )

        if download.cloud_csv_url and not download.csv_data:
            st.link_button("Open CSV From Server", download.cloud_csv_url, use_container_width=True)

        if download.data:
            with st.expander("Original Binary", expanded=False):
                st.download_button(
                    "Download Original .bin",
                    data=download.data,
                    file_name=download.name,
                    mime="application/octet-stream",
                    use_container_width=True,
                )
        elif download.cloud_bin_url:
            with st.expander("Original Binary", expanded=False):
                st.link_button("Open .bin From Server", download.cloud_bin_url, use_container_width=True)


def render_download_fragment(state: DashboardState, ready: bool, log_state: str) -> None:
    drain_messages(state)
    render_download_panel(state, ready, log_state)


def render_files(state: DashboardState) -> None:
    log_state, _, _ = logger_state(state.status)
    ready = command_path_ready(state)
    header_cols = st.columns([1, 1, 4])
    if header_cols[0].button("Refresh Files", disabled=not ready, use_container_width=True):
        publish_command(state, "files")

    if header_cols[1].button("Refresh Server", use_container_width=True):
        refresh_cloud_files(state)

    if state.last_files_at:
        header_cols[2].caption(f"SD updated {age_text(state.last_files_at)}")
    else:
        header_cols[2].caption("No SD file list received yet")

    if state.files_error:
        st.warning(f"SD file list failed: {state.files_error}")

    if state.sd:
        sd_cols = st.columns(3)
        sd_cols[0].metric("SD Used", f"{state.sd.get('used_gb', 0):.2f} GB")
        sd_cols[1].metric("SD Free", f"{state.sd.get('free_gb', 0):.2f} GB")
        sd_cols[2].metric("SD Total", f"{state.sd.get('total_gb', 0):.2f} GB")

    if not state.last_cloud_files_at:
        refresh_cloud_files(state)

    with st.expander("Server Storage", expanded=False):
        if state.cloud_error:
            st.warning(f"Could not load server files: {state.cloud_error}")
        if state.cloud_storage:
            storage = state.cloud_storage
            cols = st.columns(4)
            cols[0].metric("Server Uploads", format_mb(storage.get("uploads_bytes")))
            cols[1].metric("Server Free", format_mb(storage.get("disk_free_bytes")))
            cols[2].metric("Disk Used", f"{storage.get('disk_used_percent', 0):.1f}%")
            cols[3].metric("Retention", f"{storage.get('upload_retention_days', 0)} days")
            st.caption(
                f"Automatic cleanup keeps at most {storage.get('max_uploads', 0)} uploads "
                f"and deletes uploads older than {storage.get('upload_retention_days', 0)} days."
            )
        if state.cloud_cleanup and state.cloud_cleanup.get("deleted_count"):
            st.caption(
                f"Cleanup removed {state.cloud_cleanup.get('deleted_count')} upload(s), "
                f"freeing {format_mb(state.cloud_cleanup.get('freed_bytes'))}."
            )

        if state.cloud_files:
            cloud_rows = []
            for item in state.cloud_files:
                started_at = float(item.get("started_at", 0) or 0)
                cloud_rows.append(
                    {
                        "File": item.get("filename", ""),
                        "File ID": item.get("file_id", ""),
                        "Status": item.get("status", ""),
                        "Storage": format_mb(item.get("storage_bytes")),
                        "Uploaded": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(started_at))
                        if started_at
                        else "",
                    }
                )
            st.dataframe(pd.DataFrame(cloud_rows), use_container_width=True, hide_index=True)

            delete_options = [row["File ID"] for row in cloud_rows if row["File ID"]]
            selected_upload = st.selectbox("Server upload to delete", delete_options, key="server_delete_upload")
            if st.button("Delete Selected Server Upload", use_container_width=True):
                try:
                    result = delete_cloud_upload(selected_upload, state.command_token)
                    refresh_cloud_files(state)
                    if result.get("already_deleted"):
                        st.info("That server upload was already gone; refreshed the list.")
                    else:
                        st.success("Server upload deleted.")
                    st.rerun()
                except (RuntimeError, urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
                    refresh_cloud_files(state)
                    st.warning(f"Delete failed: {exc}")
        else:
            st.caption("No server-side uploads listed yet.")

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
    download_disabled = (not ready) or log_state != "idle"
    if st.button("Download Selected", disabled=download_disabled, use_container_width=True):
        request_id = time.time_ns() & 0xFFFFFFFF
        reset_download(state, selected_file, request_id)
        selected_size = next(
            (int(row["Size bytes"]) for row in rows if row["File"] == selected_file),
            0,
        )
        state.download.expected_size = selected_size
        request_download_window(state, 0, "button")
        st.rerun()

    if log_state != "idle":
        st.caption("Stop logging before downloading a file.")

    if state.download.name:
        st.fragment(run_every=1.0)(render_download_fragment)(state, ready, log_state)


# --------------------- Config page helpers ---------------------


def format_can_id(value: Any, fallback: str = "0x000") -> str:
    if value is None:
        return fallback

    try:
        parsed = int(str(value).strip(), 0)
    except (TypeError, ValueError):
        return str(value).strip() or fallback

    return f"0x{parsed:03X}"


def config_to_pretty_json(config: dict[str, Any]) -> str:
    return json.dumps(config, indent=2)


SENSOR_EDITOR_COLUMNS = [
    "enabled",
    "sensor_id",
    "name",
    "can_id",
    "units",
    "sample_rate_hz",
    "function",
    "port",
    "preview_enabled",
]


def sensor_rows_from_config(node: dict[str, Any]) -> pd.DataFrame:
    sensors = node.get("sensors", [])
    if not isinstance(sensors, list):
        return pd.DataFrame(columns=SENSOR_EDITOR_COLUMNS)

    rows: list[dict[str, Any]] = []
    for sensor in sensors:
        if not isinstance(sensor, dict):
            continue
        rows.append(
            {
                "enabled": bool(sensor.get("enabled", True)),
                "sensor_id": int(sensor.get("sensor_id", 0) or 0),
                "name": str(sensor.get("name", "")),
                "can_id": format_can_id(sensor.get("can_id", "")),
                "units": str(sensor.get("units", "")),
                "sample_rate_hz": int(sensor.get("sample_rate_hz", 10) or 10),
                "function": str(sensor.get("function", "sim") or "sim"),
                "port": str(sensor.get("port", "0") or "0"),
                "preview_enabled": bool(sensor.get("preview_enabled", True)),
            }
        )

    return pd.DataFrame(rows, columns=SENSOR_EDITOR_COLUMNS)


def sensor_rows_to_config(rows: Any, existing_sensors: Any = None) -> list[dict[str, Any]]:
    if isinstance(rows, pd.DataFrame):
        rows = rows.to_dict("records")

    if not isinstance(rows, list):
        return []

    existing_by_id: dict[int, dict[str, Any]] = {}
    if isinstance(existing_sensors, list):
        for sensor in existing_sensors:
            if not isinstance(sensor, dict):
                continue
            try:
                sensor_id = int(sensor.get("sensor_id", 0) or 0)
            except (TypeError, ValueError):
                continue
            existing_by_id[sensor_id] = sensor

    sensors: list[dict[str, Any]] = []
    for row in rows:
        try:
            sensor_id = int(row.get("sensor_id", 0) or 0)
        except (TypeError, ValueError):
            continue

        name = str(row.get("name", "")).strip()
        can_id = str(row.get("can_id", "")).strip()
        if sensor_id <= 0 or not name or not can_id:
            continue

        try:
            sample_rate_hz = int(row.get("sample_rate_hz", 10) or 10)
        except (TypeError, ValueError):
            sample_rate_hz = 10

        sample_rate_hz = max(1, min(sample_rate_hz, 1000))
        function = str(row.get("function", "sim") or "sim").strip().lower()
        if function not in SENSOR_FUNCTION_OPTIONS:
            function = "sim"

        port_text = str(row.get("port", "0") or "0").strip().upper()
        if port_text.startswith("GPIO"):
            port_text = port_text[4:]
        try:
            port = max(0, min(int(port_text), 255))
        except ValueError:
            port = 0

        sensor_config = copy.deepcopy(existing_by_id.get(sensor_id, {}))
        sensor_config.update(
            {
                "sensor_id": sensor_id,
                "name": name,
                "enabled": bool(row.get("enabled", True)),
                "can_id": format_can_id(can_id),
                "units": str(row.get("units", "raw")).strip() or "raw",
                "sample_rate_hz": sample_rate_hz,
                "function": function,
                "port": port,
                "preview_enabled": bool(row.get("preview_enabled", True)),
            }
        )
        sensors.append(sensor_config)

    return sensors


def next_node_id(config: dict[str, Any]) -> int:
    nodes = config.get("nodes", [])
    if not isinstance(nodes, list):
        return 1

    used = {
        int(node.get("node_id", 0) or 0)
        for node in nodes
        if isinstance(node, dict)
    }
    node_id = 1
    while node_id in used:
        node_id += 1
    return node_id


def add_node_template(config: dict[str, Any]) -> dict[str, Any]:
    updated = copy.deepcopy(config)
    nodes = updated.setdefault("nodes", [])
    if not isinstance(nodes, list):
        updated["nodes"] = []
        nodes = updated["nodes"]

    node_id = next_node_id(updated)
    nodes.append(
        {
            "node_id": node_id,
            "name": f"sensor_node_{node_id}",
            "enabled": True,
            "active": False,
            "state_ack_can_id": format_can_id(0x0C0 + node_id),
            "sensors": [],
        }
    )
    return updated


def render_parameter_editor(
    state: DashboardState,
    config: dict[str, Any],
    disabled: bool,
) -> dict[str, Any]:
    updated = copy.deepcopy(config)
    nodes = updated.setdefault("nodes", [])
    if not isinstance(nodes, list):
        updated["nodes"] = []
        nodes = updated["nodes"]

    widget_version = st.session_state.get("config_editor_loaded_at", "local")
    node_count = len(nodes)
    master_sensors = updated.setdefault("master_sensors", [])
    if not isinstance(master_sensors, list):
        updated["master_sensors"] = []
        master_sensors = updated["master_sensors"]
    summary = (
        f"{node_count} physical node{'s' if node_count != 1 else ''}, "
        f"{len(master_sensors)} master sensor{'s' if len(master_sensors) != 1 else ''}"
    )
    st.caption(summary)

    with st.expander("Master Sensors", expanded=True):
        edited_master_sensors = st.data_editor(
            sensor_rows_from_config({"sensors": master_sensors}),
            key=f"master_sensors_{widget_version}",
            hide_index=True,
            use_container_width=True,
            num_rows="dynamic",
            disabled=disabled,
            column_config={
                "enabled": st.column_config.CheckboxColumn("Enabled"),
                "sensor_id": st.column_config.NumberColumn(
                    "Sensor ID",
                    min_value=1,
                    max_value=255,
                    step=1,
                ),
                "name": st.column_config.TextColumn("Signal Name"),
                "can_id": st.column_config.TextColumn("Signal ID"),
                "units": st.column_config.TextColumn("Units"),
                "sample_rate_hz": st.column_config.NumberColumn(
                    "Rate Hz",
                    min_value=1,
                    max_value=1000,
                    step=1,
                ),
                "function": st.column_config.SelectboxColumn(
                    "Function",
                    options=SENSOR_FUNCTION_OPTIONS,
                ),
                "port": st.column_config.TextColumn("Port"),
                "preview_enabled": st.column_config.CheckboxColumn("Graph"),
            },
        )
        updated["master_sensors"] = sensor_rows_to_config(edited_master_sensors, master_sensors)
        st.caption("Use Graph only for signals you want in live charts; GPS position can stay log-only.")

    if st.button("Add Node", disabled=disabled, use_container_width=True):
        updated = add_node_template(updated)
        st.session_state.config_editor_text = config_to_pretty_json(updated)
        st.rerun()

    for index, node in enumerate(nodes):
        if not isinstance(node, dict):
            continue

        node_id_default = int(node.get("node_id", index + 1) or index + 1)
        expander_label = f"Node {node_id_default}: {node.get('name', 'unnamed_node')}"
        with st.expander(expander_label, expanded=True):
            col1, col2, col3, col4 = st.columns([1, 1, 2, 1.4])
            node_enabled = col1.checkbox(
                "Enabled",
                value=bool(node.get("enabled", True)),
                key=f"node_enabled_{widget_version}_{index}",
                disabled=disabled,
            )
            node_id = col2.number_input(
                "Node ID",
                min_value=1,
                max_value=255,
                value=node_id_default,
                step=1,
                key=f"node_id_{widget_version}_{index}",
                disabled=disabled,
            )
            node_name = col3.text_input(
                "Name",
                value=str(node.get("name", "")),
                key=f"node_name_{widget_version}_{index}",
                disabled=disabled,
            )
            state_ack_can_id = col4.text_input(
                "State ACK CAN ID",
                value=format_can_id(node.get("state_ack_can_id", 0x0C0 + node_id_default)),
                key=f"node_state_ack_{widget_version}_{index}",
                disabled=disabled,
            )

            sensor_rows = sensor_rows_from_config(node)
            edited_sensors = st.data_editor(
                sensor_rows,
                key=f"sensors_{widget_version}_{index}",
                hide_index=True,
                use_container_width=True,
                num_rows="dynamic",
                disabled=disabled,
                column_config={
                    "enabled": st.column_config.CheckboxColumn("Enabled"),
                    "sensor_id": st.column_config.NumberColumn(
                        "Sensor ID",
                        min_value=1,
                        max_value=255,
                        step=1,
                    ),
                    "name": st.column_config.TextColumn("Signal Name"),
                    "can_id": st.column_config.TextColumn("CAN ID"),
                    "units": st.column_config.TextColumn("Units"),
                    "sample_rate_hz": st.column_config.NumberColumn(
                        "Rate Hz",
                        min_value=1,
                        max_value=1000,
                        step=1,
                    ),
                    "function": st.column_config.SelectboxColumn(
                        "Function",
                        options=SENSOR_FUNCTION_OPTIONS,
                    ),
                    "port": st.column_config.TextColumn("Port"),
                    "preview_enabled": st.column_config.CheckboxColumn("Graph"),
                },
            )

            node["enabled"] = node_enabled
            node["node_id"] = int(node_id)
            node["name"] = node_name.strip() or f"sensor_node_{int(node_id)}"
            node["state_ack_can_id"] = format_can_id(state_ack_can_id)
            node["sensors"] = sensor_rows_to_config(edited_sensors, node.get("sensors", []))

            enabled_sensors = sum(1 for sensor in node["sensors"] if sensor.get("enabled", True))
            st.caption(f"{enabled_sensors}/{len(node['sensors'])} sensors enabled")

    return updated


def render_config_editor(state: DashboardState) -> None:
    log_state, _, _ = logger_state(state.status)
    idle = log_state == "idle"
    ready = command_path_ready(state)

    pending_editor_text = st.session_state.pop("config_editor_text_pending", None)
    if pending_editor_text is not None:
        st.session_state.config_editor_text = pending_editor_text

    if state.last_config_at is not None:
        loaded_at_key = "config_editor_loaded_at"
        if st.session_state.get(loaded_at_key) != state.last_config_at:
            st.session_state.config_editor_text = state.config_text
            st.session_state[loaded_at_key] = state.last_config_at
    elif "config_editor_text" not in st.session_state:
        st.session_state.config_editor_text = state.config_text

    col1, col2, col3 = st.columns([1, 1, 4])
    if col1.button("Load Config", disabled=not ready, use_container_width=True):
        state.pending_config_save_text = ""
        publish_command(state, "config_get")

    if col2.button("Reload SD", disabled=(not ready) or not idle, use_container_width=True):
        state.pending_config_save_text = ""
        publish_command(state, "config_reload")

    if state.config_status:
        col3.info(state.config_status)

    current_text = st.session_state.get("config_editor_text", "")
    try:
        parsed_config = json.loads(current_text) if current_text.strip() else None
        parse_error = ""
    except json.JSONDecodeError as exc:
        parsed_config = None
        parse_error = f"JSON error on line {exc.lineno}, column {exc.colno}: {exc.msg}"

    if parse_error:
        st.warning(parse_error)

    config_save_pending = bool(state.pending_config_save_text)
    save_disabled = (
        config_save_pending
        or (not ready)
        or (not idle)
        or bool(parse_error)
        or parsed_config is None
    )

    parameter_tab, raw_tab = st.tabs(["Parameters", "Raw JSON"])

    with parameter_tab:
        if isinstance(parsed_config, dict):
            parameter_config = render_parameter_editor(
                state,
                parsed_config,
                disabled=(not idle),
            )
            col_a, col_b = st.columns(2)
            if col_a.button(
                "Update JSON Preview",
                disabled=config_save_pending or not idle,
                use_container_width=True,
            ):
                st.session_state.config_editor_text = config_to_pretty_json(parameter_config)
                st.rerun()

            if col_b.button(
                "Save + Apply Parameters",
                disabled=save_disabled,
                use_container_width=True,
            ):
                save_text = config_to_pretty_json(parameter_config)
                st.session_state.config_editor_text = save_text
                state.pending_config_save_text = save_text
                publish_command(state, "config_save", config_text=save_text, apply=True)
        else:
            st.info("Load a valid nodes_config.json before using the parameter editor.")

    with raw_tab:
        edited_text = st.text_area(
            "nodes_config.json",
            height=360,
            key="config_editor_text",
        )

        try:
            raw_config = json.loads(edited_text) if edited_text.strip() else None
            raw_error = ""
        except json.JSONDecodeError as exc:
            raw_config = None
            raw_error = f"JSON error on line {exc.lineno}, column {exc.colno}: {exc.msg}"

        if raw_error:
            st.warning(raw_error)

        raw_save_disabled = (
            config_save_pending
            or
            (not ready)
            or (not idle)
            or bool(raw_error)
            or raw_config is None
        )

        if st.button("Save + Apply Raw JSON", disabled=raw_save_disabled, use_container_width=True):
            save_text = config_to_pretty_json(raw_config)
            st.session_state.config_editor_text_pending = save_text
            state.pending_config_save_text = save_text
            publish_command(state, "config_save", config_text=save_text, apply=True)
            st.rerun()

    if not idle:
        st.caption("Stop logging before saving or reloading node config.")


# --------------------- Live page helpers ---------------------


def render_gps_card(state: DashboardState) -> None:
    gps = state.latest_gps
    if not gps:
        st.info("Waiting for GPS telemetry on baja/logger/master/gps")
        return

    valid = bool(gps.get("valid", False))
    has_location = bool(gps.get("has_location", False))
    latitude = gps.get("latitude")
    longitude = gps.get("longitude")
    satellites = gps.get("satellites", "")
    hdop = gps.get("hdop_x100", "")
    speed = gps.get("speed_kph_x100", "")

    hdop_text = ""
    if hdop != "":
        try:
            hdop_text = f"{float(hdop) / 100.0:.2f}"
        except (TypeError, ValueError):
            hdop_text = str(hdop)

    speed_text = ""
    if speed != "":
        try:
            speed_text = f"{float(speed) / 100.0:.2f} km/h"
        except (TypeError, ValueError):
            speed_text = str(speed)

    col1, col2, col3, col4 = st.columns(4)
    col1.metric("GPS", "fix" if valid else "no fix", age_text(state.last_gps_at))
    col2.metric("Satellites", satellites)
    col3.metric("HDOP", hdop_text)
    col4.metric("Speed", speed_text)

    if has_location:
        st.code(f"{latitude}, {longitude}", language=None)
    else:
        st.caption("GPS is publishing, but no latitude/longitude yet.")


def render_can_table(state: DashboardState) -> None:
    sensor_metadata = sensor_metadata_by_can_id(state.status)
    rows: list[dict[str, Any]] = []
    for point in sorted(state.latest_can.values(), key=lambda item: item.can_id):
        sensor = sensor_metadata.get(point.can_id, {})
        value = adjusted_brake_pressure_value(point.value, sensor, state.brake_pressure_offsets)
        value = display_value(value, sensor)
        side = brake_pressure_side(sensor)
        rows.append(
            {
                "Signal": sensor.get("name", ""),
                "CAN ID": point.id_hex,
                "Value": format_display_value(value, sensor),
                "Units": display_units(sensor),
                "Raw": point.value if side else "",
                "Offset": state.brake_pressure_offsets.get(side, "") if side else "",
                "Timestamp": point.timestamp,
                "DLC": point.dlc,
                "Last seen": age_text(point.received_at),
            }
        )

    if rows:
        st.dataframe(pd.DataFrame(rows), use_container_width=True, hide_index=True)
    else:
        st.info("Waiting for CAN telemetry on baja/logger/master/can")


def render_latest_signal_cards(state: DashboardState) -> None:
    sensor_metadata = sensor_metadata_by_can_id(state.status)
    points = sorted(state.latest_can.values(), key=lambda item: item.can_id)
    if not points:
        st.info("Waiting for CAN telemetry on baja/logger/master/can")
        return

    columns = st.columns(min(3, len(points)))
    for index, point in enumerate(points):
        sensor = sensor_metadata.get(point.can_id, {})
        name = sensor.get("name", point.id_hex)
        units = display_units(sensor)
        side = brake_pressure_side(sensor)
        offset = int(state.brake_pressure_offsets.get(side, 0)) if side else 0
        display_reading = adjusted_brake_pressure_value(point.value, sensor, state.brake_pressure_offsets)
        display_reading = display_value(display_reading, sensor)
        value_text = f"{format_display_value(display_reading, sensor)} {units}".strip()
        column = columns[index % len(columns)]
        column.metric(
            label=f"{name} ({point.id_hex})",
            value=value_text,
            delta=age_text(point.received_at),
        )
        if side:
            button_cols = column.columns(2)
            if button_cols[0].button(
                f"Zero {side}",
                key=f"zero_brake_{side}_{point.can_id}",
                use_container_width=True,
            ):
                state.brake_pressure_offsets[side] = point.value
                st.rerun()
            if button_cols[1].button(
                "Clear zero",
                key=f"clear_brake_{side}_{point.can_id}",
                disabled=offset == 0,
                use_container_width=True,
            ):
                state.brake_pressure_offsets.pop(side, None)
                st.rerun()
            column.caption(f"raw {point.value} {units} · offset {offset} {units}".strip())


def chart_label(can_id: int, point: CanPoint | None, sensor_metadata: dict[int, dict[str, str]]) -> str:
    sensor = sensor_metadata.get(can_id, {})
    id_hex = point.id_hex if point is not None else f"0x{can_id:03X}"
    name = sensor.get("name", id_hex)
    units = display_units(sensor)
    if units:
        return f"{name} - {id_hex} ({units})"
    return f"{name} - {id_hex}"


def parse_optional_float(text: str) -> float | None:
    text = text.strip()
    if not text:
        return None
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


def chart_y_domain(
    values: list[int | float],
    y_min: float | None,
    y_max: float | None,
) -> list[float] | None:
    finite_values = [float(value) for value in values if math.isfinite(float(value))]
    if not finite_values:
        return None

    data_min = min(finite_values)
    data_max = max(finite_values)
    domain_min = float(y_min) if y_min is not None else data_min
    domain_max = float(y_max) if y_max is not None else data_max

    if domain_min > domain_max:
        domain_min, domain_max = domain_max, domain_min
    if domain_min == domain_max:
        padding = max(abs(domain_min) * 0.05, 1.0)
        domain_min -= padding
        domain_max += padding

    return [domain_min, domain_max]


def downsample_points(points: list[CanPoint], max_points: int) -> list[CanPoint]:
    if len(points) <= max_points:
        return points

    step = len(points) / max_points
    return [points[min(int(index * step), len(points) - 1)] for index in range(max_points)]


def render_signal_chart(
    points: list[CanPoint],
    title: str,
    window_s: float,
    y_min: float | None,
    y_max: float | None,
    sensor: dict[str, str] | None = None,
) -> None:
    if not points:
        return

    newest_timestamp = points[-1].timestamp
    if window_s > 0:
        oldest_timestamp = newest_timestamp - int(window_s * 1000)
        points = [point for point in points if point.timestamp >= oldest_timestamp]
    points = downsample_points(points, LIVE_CHART_DISPLAY_POINTS)
    if not points:
        return

    chart_data = pd.DataFrame(
        {
            "time_s": [(point.timestamp - newest_timestamp) / 1000.0 for point in points],
            "value": [display_value(point.value, sensor or {}) for point in points],
        }
    )
    if title:
        st.caption(title)

    if y_min is None and y_max is None:
        st.line_chart(
            chart_data.set_index("time_s"),
            use_container_width=True,
            height=220,
        )
        return

    y_scale: dict[str, Any] = {"zero": False}
    if y_min is not None or y_max is not None:
        values = chart_data["value"].tolist()
        domain = chart_y_domain(values, y_min, y_max)
        if domain is not None:
            y_scale["domain"] = domain

    spec = {
        "mark": {"type": "line", "point": False},
        "encoding": {
            "x": {
                "field": "time_s",
                "type": "quantitative",
                "title": "Seconds from now",
                "scale": {"domain": [-window_s, 0]} if window_s > 0 else {},
            },
            "y": {
                "field": "value",
                "type": "quantitative",
                "title": "Value",
                "scale": y_scale,
            },
            "tooltip": [
                {"field": "time_s", "type": "quantitative", "title": "s", "format": ".2f"},
                {"field": "value", "type": "quantitative", "title": display_units(sensor or {}) or "value"},
            ],
        },
        "height": 220,
    }
    st.vega_lite_chart(chart_data, spec, use_container_width=True)


def render_charts(state: DashboardState) -> None:
    sensor_metadata = sensor_metadata_by_can_id(state.status)
    available_ids = sorted(set(sensor_metadata.keys()) | set(state.latest_can.keys()))
    if not available_ids:
        st.info("Waiting for configured sensors or CAN telemetry on baja/logger/master/can")
        return

    options = {
        chart_label(can_id, state.latest_can.get(can_id), sensor_metadata): can_id
        for can_id in available_ids
    }
    labels_by_id = {can_id: label for label, can_id in options.items()}

    available_id_set = set(options.values())
    state.selected_chart_ids = [
        can_id for can_id in state.selected_chart_ids if can_id in available_id_set
    ]

    pending_remove_id = st.session_state.pop(PENDING_CHART_REMOVE_KEY, None)
    if pending_remove_id is not None:
        try:
            pending_remove_id = int(pending_remove_id)
        except (TypeError, ValueError):
            pending_remove_id = None

    if pending_remove_id is not None:
        state.selected_chart_ids = [
            can_id for can_id in state.selected_chart_ids if can_id != pending_remove_id
        ]

    selected_labels = [
        label for label, can_id in options.items() if can_id in state.selected_chart_ids
    ]

    picker_labels = st.session_state.get(CHART_PICKER_KEY, selected_labels)
    if not isinstance(picker_labels, list):
        picker_labels = selected_labels
    else:
        picker_labels = [label for label in picker_labels if label in options]

    if pending_remove_id is not None and pending_remove_id in labels_by_id:
        remove_label = labels_by_id[pending_remove_id]
        picker_labels = [label for label in picker_labels if label != remove_label]

    st.session_state[CHART_PICKER_KEY] = picker_labels

    chosen_labels = st.multiselect(
        "Signals",
        options=list(options.keys()),
        placeholder="Add signals to chart",
        key=CHART_PICKER_KEY,
    )
    state.selected_chart_ids = [options[label] for label in chosen_labels]
    sync_preview_selection(state, state.selected_chart_ids)

    if not state.selected_chart_ids:
        st.info("Add one or more signals to see stacked live charts.")
        return

    state.live_chart_window_s = float(st.number_input(
        "Time axis for all live charts (seconds)",
        min_value=1.0,
        max_value=60.0,
        value=float(state.live_chart_window_s),
        step=1.0,
        format="%.0f",
        key="live_chart_window_s",
    ))

    for can_id in state.selected_chart_ids:
        point = state.latest_can.get(can_id)

        col_title, col_remove = st.columns([5, 1])
        title = chart_label(can_id, point, sensor_metadata)
        col_title.markdown(f"**{title}**")
        if col_remove.button(
            "Remove",
            key=f"remove_chart_{can_id}",
            use_container_width=True,
        ):
            state.selected_chart_ids = [
                selected_id for selected_id in state.selected_chart_ids if selected_id != can_id
            ]
            st.session_state[PENDING_CHART_REMOVE_KEY] = can_id
            st.rerun()

        saved_y_min, saved_y_max = state.live_chart_y_ranges.get(can_id, (None, None))
        y_col1, y_col2 = st.columns(2)
        y_min_text = y_col1.text_input(
            "Y min",
            value="" if saved_y_min is None else f"{saved_y_min:g}",
            key=f"live_y_min_{can_id}",
            placeholder="auto",
        )
        y_max_text = y_col2.text_input(
            "Y max",
            value="" if saved_y_max is None else f"{saved_y_max:g}",
            key=f"live_y_max_{can_id}",
            placeholder="auto",
        )
        y_min = parse_optional_float(y_min_text)
        y_max = parse_optional_float(y_max_text)
        if y_min is not None and y_max is not None and y_min > y_max:
            y_min, y_max = y_max, y_min
        state.live_chart_y_ranges[can_id] = (y_min, y_max)

        points = list(state.can_points[can_id])
        if points:
            sensor = sensor_metadata.get(can_id, {})
            points = adjusted_brake_pressure_points(
                points,
                sensor,
                state.brake_pressure_offsets,
            )
            render_signal_chart(points, "", state.live_chart_window_s, y_min, y_max, sensor)
        else:
            st.info("Waiting for samples from this signal.")


def render_session_signal_chart(
    points: list[CanPoint],
    sensor: dict[str, str],
    offsets: dict[str, int],
) -> None:
    if not points:
        return

    points = adjusted_brake_pressure_points(points, sensor, offsets)
    points = downsample_points(points, LIVE_CHART_DISPLAY_POINTS)
    first_timestamp = points[0].timestamp
    chart_data = pd.DataFrame(
        {
            "time_s": [(point.timestamp - first_timestamp) / 1000.0 for point in points],
            "value": [display_value(point.value, sensor) for point in points],
        }
    )
    st.line_chart(chart_data.set_index("time_s"), use_container_width=True, height=220)


def render_local_session_review(state: DashboardState) -> None:
    session = state.local_session
    if not session.active and not session.completed:
        st.info("Start logging to record a local MQTT preview session for quick review after stop.")
        return

    all_points = local_session_points(session)
    signal_count = len(session.points)
    if all_points:
        duration_ms = max(point.timestamp for point in all_points) - min(
            point.timestamp for point in all_points
        )
    else:
        duration_ms = 0

    cols = st.columns(4)
    cols[0].metric("Local Review", "recording" if session.active else "ready")
    cols[1].metric("Samples", f"{session.total_points:,}")
    cols[2].metric("Signals", signal_count)
    cols[3].metric("Span", f"{duration_ms / 1000.0:.1f} s")

    if session.log_file:
        st.caption(f"Session source: {session.log_file}")

    if session.error:
        st.warning(session.error)

    if session.csv_path:
        csv_path = Path(session.csv_path)
        if csv_path.exists():
            st.caption(f"Saved local preview CSV: {csv_path}")
            st.download_button(
                "Download Local Preview CSV",
                data=csv_path.read_bytes(),
                file_name=csv_path.name,
                mime="text/csv",
                use_container_width=True,
            )

    if not all_points:
        return

    sensor_metadata = sensor_metadata_by_can_id(state.status)
    available_ids = sorted(session.points.keys())
    options = {
        chart_label(can_id, session.points[can_id][-1] if session.points[can_id] else None, sensor_metadata): can_id
        for can_id in available_ids
    }
    default_labels = list(options.keys())[: min(4, len(options))]
    picker_labels = st.session_state.get(SESSION_REVIEW_PICKER_KEY, default_labels)
    if not isinstance(picker_labels, list):
        picker_labels = default_labels
    picker_labels = [label for label in picker_labels if label in options]
    st.session_state[SESSION_REVIEW_PICKER_KEY] = picker_labels

    chosen_labels = st.multiselect(
        "Review signals",
        options=list(options.keys()),
        key=SESSION_REVIEW_PICKER_KEY,
        placeholder="Choose signals to review",
    )

    for label in chosen_labels:
        can_id = options[label]
        sensor = sensor_metadata.get(can_id, {})
        st.markdown(f"**{label}**")
        render_session_signal_chart(
            list(session.points[can_id]),
            sensor,
            state.brake_pressure_offsets,
        )

    with st.expander("Recent Local Samples", expanded=False):
        rows: list[dict[str, Any]] = []
        for point in all_points[-200:]:
            sensor = sensor_metadata.get(point.can_id, {})
            value = adjusted_brake_pressure_value(point.value, sensor, state.brake_pressure_offsets)
            rows.append(
                {
                    "Signal": sensor.get("name", ""),
                    "CAN ID": point.id_hex,
                    "Value": format_display_value(display_value(value, sensor), sensor),
                    "Units": display_units(sensor),
                    "Timestamp": point.timestamp,
                    "Received": time.strftime("%H:%M:%S", time.localtime(point.received_at)),
                }
            )
        st.dataframe(pd.DataFrame(rows), use_container_width=True, hide_index=True)

    if not session.active and st.button("Clear Local Review Session", use_container_width=True):
        state.local_session = LocalSessionCapture()
        st.session_state.pop(SESSION_REVIEW_PICKER_KEY, None)
        st.rerun()


def sync_preview_selection(state: DashboardState, can_ids: list[int]) -> None:
    desired = tuple(sorted(set(can_ids))[:8])
    if not command_path_ready(state):
        state.preview_chart_ids = None
        return
    if state.preview_chart_ids == desired:
        return

    publish_command(state, "preview_config", can_ids=list(desired))
    state.preview_chart_ids = desired


# --------------------- Nodes page ---------------------


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


# --------------------- Health page helpers ---------------------


def health_value(payload: dict[str, Any], *names: str, default: Any = "") -> Any:
    for name in names:
        value = payload.get(name)
        if value is not None:
            return value
    return default


def health_number(payload: dict[str, Any], *names: str, default: float = 0.0) -> float:
    value = health_value(payload, *names, default=default)
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def health_board_label(payload: dict[str, Any]) -> str:
    name = str(payload.get("name", "")).strip()
    source = str(payload.get("source", "board")).strip()
    node_id = payload.get("node_id", "")

    if name:
        return name
    if source == "node" and node_id != "":
        return f"node_{node_id}"
    return source or "board"


def health_state(payload: dict[str, Any], received_at: float) -> str:
    age = time.time() - received_at
    if age > STALE_AFTER_SECONDS:
        return "stale"

    load = health_task_load_percent(payload)
    can_load = health_can_bus_load_percent(payload)
    late_ms = health_timing_ms(payload)
    missed = health_number(payload, "missed_deadlines")
    queue_pressure = health_queue_pressure_percent(payload)
    drops = health_drop_count(payload)
    failures = health_failure_count(payload)

    if (
        missed > 0
        or late_ms > 25
        or load > 85
        or can_load > 70
        or queue_pressure > 80
        or drops > 0
        or failures > 0
    ):
        return "check"
    return "ok"


def health_task_load_percent(payload: dict[str, Any]) -> float:
    return health_number(
        payload,
        "task_load_percent",
        "cpu_load_percent",
        "load_percent",
    )


def health_pressure_percent(payload: dict[str, Any]) -> float:
    if "pressure_percent" in payload:
        return health_number(payload, "pressure_percent")
    return health_queue_pressure_percent(payload)


def health_can_bus_load_percent(payload: dict[str, Any]) -> float:
    if "can_bus_load_percent" in payload:
        return health_number(payload, "can_bus_load_percent")
    if "can_bus_load_x100" in payload:
        return health_number(payload, "can_bus_load_x100") / 100.0
    return 0.0


def health_can_frames_per_sec(payload: dict[str, Any]) -> float:
    return health_number(payload, "can_frames_per_sec_x100") / 100.0


def health_can_kbps(payload: dict[str, Any]) -> float:
    return health_number(payload, "can_bits_per_sec") / 1000.0


def health_timing_ms(payload: dict[str, Any]) -> float:
    if "max_lateness_ms" in payload:
        return health_number(payload, "max_lateness_ms")
    return health_number(payload, "sd_write_last_ms")


def health_timing_label(payload: dict[str, Any]) -> str:
    if "max_lateness_ms" in payload:
        return "Late"
    if "sd_write_last_ms" in payload:
        return "SD write"
    return "Timing"


def health_queue_depths(payload: dict[str, Any]) -> dict[str, int]:
    depths: dict[str, int] = {}
    for label, field_name in (
        ("rx", "rx_queue_depth"),
        ("sample", "sample_queue_depth"),
        ("heartbeat", "heartbeat_queue_depth"),
    ):
        if field_name in payload:
            depths[label] = int(health_number(payload, field_name))
    return depths


def health_queue_summary(payload: dict[str, Any]) -> str:
    depths = health_queue_depths(payload)
    if not depths:
        return ""

    if set(depths) == {"sample"}:
        return str(depths["sample"])

    return " / ".join(f"{name} {depth}" for name, depth in depths.items())


def health_queue_pressure_percent(payload: dict[str, Any]) -> float:
    queue_pairs = (
        ("rx_queue_depth", "rx_queue_capacity"),
        ("sample_queue_depth", "sample_queue_capacity"),
        ("heartbeat_queue_depth", "heartbeat_queue_capacity"),
    )

    pressures: list[float] = []
    for depth_name, capacity_name in queue_pairs:
        if depth_name not in payload:
            continue
        depth = health_number(payload, depth_name)
        capacity = health_number(payload, capacity_name)
        if capacity > 0:
            pressures.append(min((depth / capacity) * 100.0, 100.0))
        else:
            pressures.append(min(depth, 100.0))

    return max(pressures) if pressures else 0.0


def health_drop_count(payload: dict[str, Any]) -> int:
    drops = 0
    for field_name in ("sample_queue_drops", "heartbeat_queue_drops"):
        drops += int(health_number(payload, field_name))

    if payload.get("sample_drops_seen"):
        drops += 1

    return drops


def health_failure_count(payload: dict[str, Any]) -> int:
    return int(
        health_number(payload, "tx_fail_count")
        + health_number(payload, "sd_write_failures")
    )


def health_notes(payload: dict[str, Any], received_at: float) -> str:
    notes: list[str] = []

    if time.time() - received_at > STALE_AFTER_SECONDS:
        notes.append("stale")

    load = health_task_load_percent(payload)
    can_load = health_can_bus_load_percent(payload)
    timing_ms = health_timing_ms(payload)
    missed = int(health_number(payload, "missed_deadlines"))
    queue_pressure = health_queue_pressure_percent(payload)
    drops = health_drop_count(payload)
    failures = health_failure_count(payload)

    if load > 85:
        notes.append("high task load")
    if can_load > 70:
        notes.append("high CAN bus load")
    if timing_ms > 25:
        notes.append(f"{health_timing_label(payload).lower()} high")
    if missed > 0:
        notes.append("missed deadlines")
    if queue_pressure > 80:
        notes.append("queue near full")
    if drops > 0:
        notes.append("drops seen")
    if failures > 0:
        notes.append("failures seen")

    return ", ".join(notes) if notes else "ok"


def health_history_value(payload: dict[str, Any], metric_key: str) -> float:
    if metric_key == "task_load_percent":
        return health_task_load_percent(payload)
    if metric_key == "pressure_percent":
        return health_pressure_percent(payload)
    if metric_key == "can_bus_load_percent":
        return health_can_bus_load_percent(payload)
    if metric_key == "can_frames_per_sec":
        return health_can_frames_per_sec(payload)
    if metric_key == "timing_ms":
        return health_timing_ms(payload)
    if metric_key == "queue_pressure_percent":
        return health_queue_pressure_percent(payload)
    if metric_key == "queue_depth":
        depths = health_queue_depths(payload)
        return float(max(depths.values())) if depths else 0.0
    if metric_key == "free_heap_kb":
        return health_number(payload, "free_heap_kb")
    if metric_key == "drops_failures":
        return float(health_drop_count(payload) + health_failure_count(payload))
    return 0.0


def render_health_summary(rows: list[dict[str, Any]]) -> None:
    total = len(rows)
    stale = sum(1 for row in rows if row["Status"] == "stale")
    check = sum(1 for row in rows if row["Status"] == "check")
    ok = sum(1 for row in rows if row["Status"] == "ok")
    max_load = max((float(row["Task load %"]) for row in rows), default=0.0)
    max_timing = max((float(row["Timing ms"]) for row in rows), default=0.0)
    max_can = max((float(row["CAN bus %"]) for row in rows), default=0.0)

    col1, col2, col3, col4 = st.columns(4)
    col1.metric("Boards Reporting", total)
    col2.metric("OK / Check / Stale", f"{ok} / {check} / {stale}")
    col3.metric("Highest Task Load", f"{max_load:.0f}%")
    col4.metric("CAN / Timing", f"{max_can:.2f}%", f"{max_timing:.0f} ms worst")


def render_health_help() -> None:
    with st.expander("What These Health Numbers Mean", expanded=False):
        st.markdown(
            """
            - **Task load %**: estimated time spent inside the board's measured logger/sampler tasks. This is not full CPU load unless FreeRTOS runtime stats are enabled.
            - **Pressure %**: master-only bottleneck estimate from queue fill and SD write time.
            - **CAN bus %**: master-only estimated CAN utilization from observed frames, frame sizes, and configured bitrate.
            - **CAN fps / kbps**: estimated bus message rate and bit rate seen by the master.
            - **Timing ms**: node lateness for sensor sampling, or master SD write duration.
            - **Queue pressure %**: how full the busiest receive/sample queue is.
            - **Missed / Drops / Failures**: missed sensor deadlines, dropped queued frames, failed CAN TX or SD writes.
            - **Free heap KB**: remaining dynamic memory on the board.
            """
        )


def render_health_charts(state: DashboardState, board_labels: dict[str, str]) -> None:
    if not board_labels:
        return

    board_options = list(board_labels.values())
    saved_boards = st.session_state.get(HEALTH_BOARD_PICKER_KEY, board_options)
    if not isinstance(saved_boards, list):
        saved_boards = board_options
    saved_boards = [label for label in saved_boards if label in board_options]
    st.session_state[HEALTH_BOARD_PICKER_KEY] = saved_boards or board_options

    selected_labels = st.multiselect(
        "Boards",
        options=board_options,
        key=HEALTH_BOARD_PICKER_KEY,
    )
    selected_keys = {
        key for key, label in board_labels.items() if label in selected_labels
    }

    metric_options = {
        "Task load %": "task_load_percent",
        "Pressure %": "pressure_percent",
        "CAN bus %": "can_bus_load_percent",
        "CAN frames/sec": "can_frames_per_sec",
        "Timing ms": "timing_ms",
        "Queue pressure %": "queue_pressure_percent",
        "Queue depth": "queue_depth",
        "Free heap KB": "free_heap_kb",
        "Drops + failures": "drops_failures",
    }
    if st.session_state.get(HEALTH_METRIC_PICKER_KEY) not in metric_options:
        st.session_state[HEALTH_METRIC_PICKER_KEY] = "Task load %"

    metric_label = st.selectbox(
        "Metric",
        options=list(metric_options.keys()),
        key=HEALTH_METRIC_PICKER_KEY,
    )
    metric_key = metric_options[metric_label]

    history_times = [
        float(item.get("_received_at", time.time()) or time.time())
        for key in selected_keys
        for item in state.health_history.get(key, [])
    ]
    first_time = min(history_times) if history_times else time.time()

    history_rows: list[dict[str, Any]] = []
    for key in selected_keys:
        label = board_labels.get(key, key)
        for item in list(state.health_history.get(key, []))[-HEALTH_CHART_DISPLAY_POINTS:]:
            received_at = float(item.get("_received_at", first_time) or first_time)
            history_rows.append(
                {
                    "time_s": received_at - first_time,
                    "board": label,
                    metric_label: health_history_value(item, metric_key),
                }
            )

    if not history_rows:
        st.info("Waiting for health history.")
        return

    history = pd.DataFrame(history_rows)
    chart = history.pivot_table(
        index="time_s",
        columns="board",
        values=metric_label,
        aggfunc="last",
    )
    st.line_chart(chart, use_container_width=True, height=260)


def render_health(state: DashboardState, show_charts: bool) -> None:
    if not state.health_latest:
        st.info("Waiting for board health telemetry on baja/logger/master/health")
        return

    rows: list[dict[str, Any]] = []
    board_labels: dict[str, str] = {}

    for key, payload in sorted(state.health_latest.items()):
        received_at = float(payload.get("_received_at", 0.0) or 0.0)
        label = health_board_label(payload)
        board_labels[key] = label
        state_text = health_state(payload, received_at)
        timing_ms = health_timing_ms(payload)

        rows.append(
            {
                "Board": label,
                "Type": payload.get("source", ""),
                "Status": state_text,
                "Active": bool(payload.get("active", False)),
                "Task load %": round(health_task_load_percent(payload), 1),
                "Pressure %": round(health_pressure_percent(payload), 1),
                "CAN bus %": round(health_can_bus_load_percent(payload), 2),
                "CAN fps": round(health_can_frames_per_sec(payload), 1),
                "CAN kbps": round(health_can_kbps(payload), 1),
                "Timing": health_timing_label(payload),
                "Timing ms": round(timing_ms, 1),
                "Queue": health_queue_summary(payload),
                "Queue pressure %": round(health_queue_pressure_percent(payload), 1),
                "Missed": health_value(payload, "missed_deadlines", default=""),
                "Drops": health_drop_count(payload),
                "Failures": health_failure_count(payload),
                "Free heap KB": health_value(payload, "free_heap_kb", default=""),
                "Last seen": age_text(received_at),
                "Notes": health_notes(payload, received_at),
            }
        )

    render_health_summary(rows)
    render_health_help()
    st.dataframe(
        pd.DataFrame(rows),
        use_container_width=True,
        hide_index=True,
        column_config={
            "Task load %": st.column_config.ProgressColumn(
                "Task load %",
                min_value=0,
                max_value=100,
                format="%.0f%%",
            ),
            "Pressure %": st.column_config.ProgressColumn(
                "Pressure %",
                min_value=0,
                max_value=100,
                format="%.0f%%",
            ),
            "CAN bus %": st.column_config.NumberColumn(
                "CAN bus %",
                format="%.2f%%",
            ),
            "Queue pressure %": st.column_config.ProgressColumn(
                "Queue pressure %",
                min_value=0,
                max_value=100,
                format="%.0f%%",
            ),
        },
    )

    if not show_charts:
        return

    st.subheader("Health Chart")
    render_health_charts(state, board_labels)


# --------------------- Page router and app entry ---------------------


def render_dashboard_content(
    state: DashboardState,
    view: str,
    show_live_charts: bool,
    show_health_charts: bool,
) -> None:
    drain_messages(state)

    ready = command_path_ready(state)

    if ready and view == "Files" and not state.files_requested_once:
        publish_command(state, "files")
        state.files_requested_once = True

    if ready and view == "Config" and not state.config_requested_once:
        publish_command(state, "config_get")
        state.config_requested_once = True

    st.title("Baja Logger Live")
    render_status_cards(state)
    render_controls(state)

    if view == "Live":
        st.subheader("GPS")
        render_gps_card(state)

        st.subheader("Latest Signals")
        render_latest_signal_cards(state)

        if show_live_charts:
            st.subheader("Live Signal Charts")
            render_charts(state)
        else:
            sync_preview_selection(state, [])

        st.subheader("Local Session Review")
        render_local_session_review(state)

        with st.expander("Latest CAN Table", expanded=False):
            render_can_table(state)

        with st.expander("Nodes", expanded=False):
            render_nodes(state.status)

    elif view == "Health":
        st.subheader("Board Health")
        render_health(state, show_health_charts)

    elif view == "Files":
        st.subheader("SD Files")
        render_files(state)

    elif view == "Config":
        st.subheader("Master Node Config")
        render_config_editor(state)

    elif view == "Nodes":
        st.subheader("Configured Nodes")
        render_nodes(state.status)


def main() -> None:
    st.set_page_config(page_title="Baja Logger Live", layout="wide")
    hide_streamlit_chrome()
    state = get_state()

    with st.sidebar:
        st.title("Baja Logger")
        broker = st.text_input("Broker", state.broker)
        port = st.number_input("Port", min_value=1, max_value=65535, value=state.port)
        topic = st.text_input("Topic", state.topic)
        state.command_topic = st.text_input("Command topic", state.command_topic)
        view = st.radio(
            "Page",
            ["Live", "Health", "Files", "Config", "Nodes"],
            index=0,
            key="dashboard_page",
        )
        auto_refresh = st.toggle("Auto refresh", value=True)
        refresh_ms = st.slider("Refresh", 500, 3000, 1000, step=100)
        show_live_charts = False
        show_health_charts = False
        if view == "Live":
            show_live_charts = st.checkbox("Show live charts", value=False)
        elif view == "Health":
            show_health_charts = st.checkbox("Show health chart", value=False)
        auto_refresh_enabled = auto_refresh and not (view == "Files" and bool(state.download.name))

        if st.button("Reconnect", use_container_width=True):
            start_mqtt_client(state, broker, int(port), topic)

        if st.button("Refresh Now", use_container_width=True):
            drain_messages(state, max_messages=MQTT_QUEUE_MAX)

        st.caption(f"Message rate: {message_rate(state):.1f}/s")
        st.caption(f"Pending UI messages: {state.queue.qsize()}")

    if not state.thread_started:
        start_mqtt_client(state, state.broker, state.port, state.topic)

    def render_body() -> None:
        page = st.empty()
        with page.container():
            render_dashboard_content(state, view, show_live_charts, show_health_charts)

    if auto_refresh_enabled:
        st.fragment(run_every=refresh_ms / 1000.0)(render_body)()
    else:
        render_body()


if __name__ == "__main__":
    main()
