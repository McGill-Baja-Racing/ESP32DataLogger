#!/usr/bin/env python3
"""Lightweight live dashboard for the Baja logger.

This is a faster alternative to the Streamlit dashboard. It keeps MQTT handling
in Python, serves a plain browser page, and pushes updates with Server-Sent
Events so the page does not rerun Python every frame.

Run from the Master project folder:
    .venv/bin/python tools/light_dashboard.py

Then open:
    http://localhost:8765/live

Available pages:
    /live    live signal cards and charts
    /health  board load, timing, queue, and heap
    /files   SD file list and download
    /nodes   configured node status
    /config  edit nodes_config.json on the master SD card

Useful options:
    .venv/bin/python tools/light_dashboard.py --broker 138.197.132.56 --web-port 8765
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import queue
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse

try:
    import paho.mqtt.client as mqtt
except ImportError as exc:
    raise SystemExit(
        "Missing dependency: paho-mqtt\n"
        "Install dashboard dependencies with:\n"
        "    .venv/bin/python -m pip install paho-mqtt"
    ) from exc


DEFAULT_BROKER = "138.197.132.56"
DEFAULT_MQTT_PORT = 1883
DEFAULT_TOPIC = "baja/logger/master/#"
DEFAULT_COMMAND_TOPIC = "baja/logger/master/command"
DEFAULT_COMMAND_TOKEN = "baja_logger_test_v1"
DEFAULT_WEB_HOST = "127.0.0.1"
DEFAULT_WEB_PORT = 8765
MAX_HISTORY = 600
LIVE_CHART_DISPLAY_POINTS = 160
SSE_PERIOD_SECONDS = 0.25
HEALTH_SSE_PERIOD_SECONDS = 0.5
SLOW_SSE_PERIOD_SECONDS = 1.0
VALID_VIEWS = {"live", "health", "files", "nodes", "config"}


INDEX_HTML = r"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Baja Logger</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f7f8fa;
      --panel: #ffffff;
      --text: #171b22;
      --muted: #667085;
      --line: #d6dae2;
      --accent: #1769aa;
      --good: #138a45;
      --warn: #b76e00;
      --bad: #b42318;
      --shadow: 0 1px 2px rgba(16, 24, 40, 0.08);
    }

    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--bg);
      color: var(--text);
    }

    header {
      position: sticky;
      top: 0;
      z-index: 3;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      padding: 10px 16px;
      border-bottom: 1px solid var(--line);
      background: rgba(255, 255, 255, 0.94);
      backdrop-filter: blur(8px);
    }

    header a {
      color: inherit;
    }

    h1 {
      margin: 0;
      font-size: 18px;
      line-height: 1.2;
    }

    main {
      padding: 14px;
      max-width: 1480px;
      margin: 0 auto;
    }

    section, .toolbar, .signal-card, .health-card {
      border: 1px solid var(--line);
      background: var(--panel);
      box-shadow: var(--shadow);
    }

    section {
      padding: 12px;
      border-radius: 8px;
    }

    h2 {
      margin: 0 0 10px;
      font-size: 15px;
    }

    button {
      height: 34px;
      padding: 0 12px;
      border: 1px solid var(--line);
      border-radius: 6px;
      background: #fff;
      color: var(--text);
      font: inherit;
      cursor: pointer;
    }

    button.primary {
      border-color: var(--accent);
      background: var(--accent);
      color: #fff;
    }

    button:disabled {
      cursor: not-allowed;
      opacity: 0.55;
    }

    nav {
      display: flex;
      align-items: center;
      gap: 4px;
      flex-wrap: wrap;
    }

    nav a {
      min-height: 30px;
      padding: 5px 9px;
      border-radius: 6px;
      color: var(--muted);
      text-decoration: none;
      font-size: 13px;
    }

    nav a.active {
      color: var(--accent);
      background: #eef6ff;
    }

    .top-left, .top-right, .controls, .header-stack {
      display: flex;
      align-items: center;
      gap: 10px;
      flex-wrap: wrap;
    }

    .header-stack {
      align-items: flex-start;
      flex-direction: column;
      gap: 6px;
    }

    .page-layout {
      display: grid;
      grid-template-columns: minmax(0, 1fr);
      gap: 14px;
    }

    .classic-two {
      grid-template-columns: minmax(0, 1.5fr) minmax(340px, 0.85fr);
      align-items: start;
    }

    .config-layout {
      grid-template-columns: minmax(0, 1.15fr) minmax(360px, 0.85fr);
      align-items: start;
    }

    .pill {
      display: inline-flex;
      align-items: center;
      min-height: 26px;
      padding: 3px 9px;
      border-radius: 999px;
      border: 1px solid var(--line);
      color: var(--muted);
      background: #fff;
      font-size: 12px;
      white-space: nowrap;
    }

    .pill.good { color: var(--good); border-color: rgba(19, 138, 69, 0.35); }
    .pill.warn { color: var(--warn); border-color: rgba(183, 110, 0, 0.35); }
    .pill.bad { color: var(--bad); border-color: rgba(180, 35, 24, 0.35); }

    .grid {
      display: grid;
      gap: 10px;
    }

    .latest-grid {
      grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
    }

    .signal-card, .health-card {
      border-radius: 8px;
      padding: 10px;
    }

    .signal-title, .health-title {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      gap: 10px;
      margin-bottom: 6px;
    }

    .label {
      font-weight: 650;
      overflow-wrap: anywhere;
    }

    .subtle {
      color: var(--muted);
      font-size: 12px;
    }

    .value {
      font-size: 28px;
      font-weight: 720;
      letter-spacing: 0;
    }

    .signal-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 6px;
      margin-top: 8px;
    }

    .signal-actions button {
      height: 28px;
      padding: 0 8px;
      font-size: 12px;
    }

    .charts {
      display: grid;
      gap: 12px;
    }

    .chart-card {
      padding: 10px;
      border: 1px solid var(--line);
      border-radius: 8px;
      background: #fff;
    }

    canvas {
      width: 100%;
      height: 160px;
      display: block;
    }

    .chart-head {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      margin-bottom: 4px;
    }

    .chart-controls {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      align-items: end;
      margin: 8px 0;
    }

    .chart-controls label {
      display: grid;
      gap: 3px;
      font-size: 12px;
      color: var(--muted);
    }

    .chart-controls input {
      width: 96px;
      height: 30px;
      padding: 0 8px;
      border: 1px solid var(--line);
      border-radius: 6px;
      font: inherit;
      color: var(--text);
      background: #fff;
    }

    .chart-controls input[type="checkbox"] {
      width: 18px;
      height: 18px;
      padding: 0;
    }

    .remove {
      height: 28px;
      padding: 0 8px;
      font-size: 12px;
    }

    .section-head {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      margin-bottom: 10px;
    }

    .section-head h2 {
      margin: 0;
    }

    .selector {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(190px, 1fr));
      gap: 8px;
      margin-bottom: 10px;
    }

    label.check {
      display: flex;
      align-items: center;
      gap: 7px;
      min-height: 30px;
      font-size: 13px;
      color: var(--text);
    }

    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
    }

    th, td {
      padding: 7px 6px;
      border-bottom: 1px solid var(--line);
      text-align: left;
      vertical-align: top;
    }

    th { color: var(--muted); font-weight: 650; }

    .bar {
      height: 8px;
      width: 100%;
      overflow: hidden;
      border-radius: 999px;
      background: #eaecf0;
      margin-top: 6px;
    }

    .bar span {
      display: block;
      height: 100%;
      width: 0;
      background: var(--good);
    }

    .bar span.warn { background: var(--warn); }
    .bar span.bad { background: var(--bad); }

    .sd-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
      margin-bottom: 10px;
    }

    .metric {
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 8px;
      background: #fff;
    }

    .metric strong {
      display: block;
      font-size: 16px;
      margin-top: 2px;
    }

    .download-box {
      margin-top: 10px;
      padding-top: 10px;
      border-top: 1px solid var(--line);
    }

    .progress {
      height: 10px;
      overflow: hidden;
      border-radius: 999px;
      background: #eaecf0;
      margin: 6px 0;
    }

    .progress span {
      display: block;
      height: 100%;
      width: 0;
      background: var(--accent);
    }

    .download-link {
      display: inline-flex;
      align-items: center;
      height: 34px;
      padding: 0 12px;
      border-radius: 6px;
      background: var(--accent);
      color: #fff;
      text-decoration: none;
    }

    .config-actions {
      display: flex;
      align-items: center;
      gap: 8px;
      flex-wrap: wrap;
      margin-bottom: 10px;
    }

    .config-card {
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 10px;
      margin-top: 10px;
      background: #fff;
    }

    .config-row {
      display: grid;
      grid-template-columns: 96px 90px minmax(160px, 1fr) 150px auto;
      gap: 8px;
      align-items: end;
      margin-bottom: 10px;
    }

    .sensor-row {
      display: grid;
      grid-template-columns: 84px 86px minmax(140px, 1fr) 112px 90px 96px auto;
      gap: 8px;
      align-items: end;
      margin-top: 8px;
    }

    .field {
      display: grid;
      gap: 4px;
      font-size: 12px;
      color: var(--muted);
    }

    input, textarea {
      width: 100%;
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 7px 8px;
      color: var(--text);
      font: inherit;
      background: #fff;
    }

    input[type="checkbox"] {
      width: auto;
      justify-self: start;
    }

    textarea {
      min-height: 300px;
      resize: vertical;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 12px;
      line-height: 1.45;
    }

    .config-error {
      color: var(--bad);
      font-size: 13px;
      margin: 8px 0;
    }

    details {
      margin-top: 12px;
      border-top: 1px solid var(--line);
      padding-top: 8px;
    }

    summary {
      cursor: pointer;
      color: var(--muted);
      font-size: 13px;
    }

    @media (max-width: 900px) {
      header { align-items: flex-start; }
      .classic-two, .config-layout { grid-template-columns: 1fr; }
      .config-row, .sensor-row { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <header>
    <div class="header-stack">
      <div class="top-left">
        <h1>Baja Logger</h1>
        <span id="mqtt" class="pill">MQTT offline</span>
        <span id="feed" class="pill">feed waiting</span>
        <span id="logger" class="pill">logger unknown</span>
      </div>
      <nav>
        <a href="/live" data-page="live">Live</a>
        <a href="/health" data-page="health">Health</a>
        <a href="/files" data-page="files">Files</a>
        <a href="/nodes" data-page="nodes">Nodes</a>
        <a href="/config" data-page="config">Config</a>
      </nav>
    </div>
    <div class="controls">
      <button id="start" class="primary">Start</button>
      <button id="stop">Stop</button>
      <button id="pause">Pause</button>
    </div>
  </header>

  <main id="page"></main>

  <script>
    const selectedSignals = new Set(JSON.parse(localStorage.getItem("selectedSignals") || "[]"));
    const chartColors = ["#1769aa", "#138a45", "#b76e00", "#7a5af8", "#b42318", "#067647"];
    const validPages = new Set(["live", "health", "files", "nodes", "config"]);
    const sensorFunctionOptions = [
      "sim",
      "adc",
      "rpm",
      "old_rpm",
      "front_brake",
      "rear_brake",
      "gps_speed",
      "gps_latitude",
      "gps_longitude",
    ];
    const pathPage = window.location.pathname.replace("/", "") || "live";
    const currentPage = validPages.has(pathPage) ? pathPage : "live";
    let lastSnapshot = null;
    let paused = false;
    let filesRequestedOnce = false;
    let configRequestedOnce = false;
    let configText = "";
    let configLoadedAt = null;
    let configDirty = false;
    let lastPreviewSignature = null;
    let chartTimeWindowSeconds = Number(localStorage.getItem("chartTimeWindowSeconds") || "5");
    let chartYRanges = JSON.parse(localStorage.getItem("chartYRanges") || "{}");
    let brakePressureOffsets = JSON.parse(localStorage.getItem("brakePressureOffsets") || "{}");
    let highRatePreview = localStorage.getItem("highRatePreview") === "true";
    let smoothPlayback = localStorage.getItem("smoothPlayback") !== "false";
    let playbackDelayMs = Number(localStorage.getItem("playbackDelayMs") || "500");
    let playbackSnapshot = null;
    const playbackById = new Map();
    let playbackAnimation = null;
    let events = null;

    const $ = (id) => document.getElementById(id);

    function setupPage() {
      document.querySelectorAll("nav a").forEach((link) => {
        link.classList.toggle("active", link.dataset.page === currentPage);
      });

      const page = $("page");
      if (currentPage === "live") {
        page.innerHTML = `
          <div class="page-layout classic-two">
            <div class="grid">
              <section>
                <h2>Signal Charts</h2>
                <div class="chart-controls">
                  <label>
                    Time axis
                    <input id="chart-window" type="number" min="1" max="60" step="1" value="${chartTimeWindowSeconds}">
                  </label>
                  <label>
                    Smooth playback
                    <input id="smooth-playback" type="checkbox" ${smoothPlayback ? "checked" : ""}>
                  </label>
                  <label>
                    All samples
                    <input id="high-rate-preview" type="checkbox" ${highRatePreview ? "checked" : ""}>
                  </label>
                  <label>
                    Delay ms
                    <input id="playback-delay" type="number" min="0" max="5000" step="50" value="${playbackDelayMs}">
                  </label>
                </div>
                <div id="signal-selector" class="selector"></div>
                <div id="charts" class="charts"></div>
              </section>
            </div>
            <div class="grid">
              <section>
                <h2>GPS</h2>
                <div id="gps"></div>
              </section>
              <section>
                <h2>Latest Signals</h2>
                <div id="latest" class="grid latest-grid"></div>
              </section>
            </div>
          </div>
        `;
        $("chart-window")?.addEventListener("change", (event) => {
          const value = Number(event.target.value || 5);
          chartTimeWindowSeconds = Math.min(60, Math.max(1, Number.isFinite(value) ? value : 5));
          event.target.value = String(chartTimeWindowSeconds);
          localStorage.setItem("chartTimeWindowSeconds", String(chartTimeWindowSeconds));
          renderCharts(lastSnapshot);
        });
        $("smooth-playback")?.addEventListener("change", (event) => {
          smoothPlayback = Boolean(event.target.checked);
          localStorage.setItem("smoothPlayback", smoothPlayback ? "true" : "false");
          resetPlayback();
          render(lastSnapshot);
        });
        $("high-rate-preview")?.addEventListener("change", (event) => {
          highRatePreview = Boolean(event.target.checked);
          localStorage.setItem("highRatePreview", highRatePreview ? "true" : "false");
          lastPreviewSignature = null;
          syncPreviewSignals();
        });
        $("playback-delay")?.addEventListener("change", (event) => {
          const value = Number(event.target.value || 500);
          playbackDelayMs = Math.min(5000, Math.max(0, Number.isFinite(value) ? value : 500));
          event.target.value = String(playbackDelayMs);
          localStorage.setItem("playbackDelayMs", String(playbackDelayMs));
          resetPlayback();
        });
      } else if (currentPage === "health") {
        page.innerHTML = `
          <div class="page-layout">
            <section>
              <h2>Board Health</h2>
              <div id="health" class="grid"></div>
            </section>
          </div>
        `;
      } else if (currentPage === "files") {
        page.innerHTML = `
          <div class="page-layout">
            <section>
              <div class="section-head">
                <h2>SD Files</h2>
                <button id="refresh-files">Refresh</button>
              </div>
              <div id="files"></div>
            </section>
          </div>
        `;
        $("refresh-files").addEventListener("click", () => sendCommand("files"));
      } else if (currentPage === "config") {
        page.innerHTML = `
          <div class="page-layout config-layout">
            <section>
              <div class="section-head">
                <h2>Node Config</h2>
                <span id="config-status" class="subtle">No config loaded yet.</span>
              </div>
              <div class="config-actions">
                <button id="load-config">Load Config</button>
                <button id="reload-config">Reload SD</button>
                <button id="add-node">Add Node</button>
                <button id="format-config">Format JSON</button>
                <button id="save-config" class="primary">Save + Apply</button>
              </div>
              <div id="config-error" class="config-error"></div>
              <div id="config-fields"></div>
            </section>
            <section>
              <h2>Raw JSON</h2>
              <details open>
                <summary>Advanced editor</summary>
                <textarea id="config-raw" spellcheck="false"></textarea>
              </details>
            </section>
          </div>
        `;
        $("load-config").addEventListener("click", () => {
          if (!configDirty || confirm("Discard unsaved config edits and reload from the master?")) {
            configDirty = false;
            sendCommand("config_get");
          }
        });
        $("reload-config").addEventListener("click", () => sendCommand("config_reload"));
        $("add-node").addEventListener("click", addConfigNode);
        $("format-config").addEventListener("click", formatConfigJson);
        $("save-config").addEventListener("click", saveConfig);
        $("config-raw").addEventListener("input", (event) => {
          configText = event.target.value;
          configDirty = true;
          updateConfigControls(lastSnapshot);
        });
      } else {
        page.innerHTML = `
          <div class="page-layout">
            <section>
              <h2>Nodes</h2>
              <div id="nodes"></div>
            </section>
          </div>
        `;
      }
    }

    function saveSelectedSignals() {
      localStorage.setItem("selectedSignals", JSON.stringify([...selectedSignals]));
    }

    function connectEvents() {
      if (events) events.close();
      const selected = [...selectedSignals].join(",");
      const params = new URLSearchParams({view: currentPage});
      params.set("selected", selected || "-");
      events = new EventSource(`/events?${params.toString()}`);
      events.onmessage = (event) => {
        if (paused) return;
        render(JSON.parse(event.data));
      };
      events.onerror = () => setPill("feed", "dashboard disconnected", "bad");
    }

    function syncPreviewSignals() {
      const maxSignals = highRatePreview ? 2 : 8;
      const canIds = [...selectedSignals].slice(0, maxSignals).map((id) => Number(id));
      const signature = JSON.stringify({canIds, highRatePreview});
      if (signature === lastPreviewSignature) return;
      lastPreviewSignature = signature;
      sendCommand("preview_config", {can_ids: canIds, all_samples: highRatePreview}).then((ok) => {
        if (!ok && lastPreviewSignature === signature) lastPreviewSignature = null;
      });
    }

    function ageText(seconds) {
      if (seconds == null) return "never";
      if (seconds < 1) return `${Math.round(seconds * 1000)} ms ago`;
      return `${seconds.toFixed(1)} s ago`;
    }

    function setPill(id, text, level) {
      const el = $(id);
      el.textContent = text;
      el.className = `pill ${level || ""}`.trim();
    }

    function escapeHtml(value) {
      return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
    }

    function formatBytes(bytes) {
      const value = Number(bytes || 0);
      if (value >= 1_000_000_000) return `${(value / 1_000_000_000).toFixed(2)} GB`;
      if (value >= 1_000_000) return `${(value / 1_000_000).toFixed(2)} MB`;
      if (value >= 1_000) return `${(value / 1_000).toFixed(1)} KB`;
      return `${value} B`;
    }

    async function sendCommand(cmd, fields = {}) {
      const res = await fetch("/command", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({cmd, ...fields}),
      });
      const payload = await res.json();
      if (!payload.ok) {
        alert(payload.error || `Failed to send ${cmd}`);
      }
      return Boolean(payload.ok);
    }

    function parseCanId(value, fallback = 0) {
      if (typeof value === "number") return Number.isFinite(value) ? value : fallback;
      const text = String(value || "").trim();
      if (!text) return fallback;
      const parsed = Number.parseInt(text, text.toLowerCase().startsWith("0x") ? 16 : 10);
      return Number.isFinite(parsed) ? parsed : fallback;
    }

    function formatCanId(value, fallback = "0x000") {
      const parsed = parseCanId(value, NaN);
      if (!Number.isFinite(parsed)) return fallback;
      return `0x${parsed.toString(16).toUpperCase().padStart(3, "0")}`;
    }

    function parseConfigText() {
      const text = configText.trim();
      if (!text) return {config: null, error: "No config loaded yet."};
      try {
        const config = JSON.parse(text);
        if (!config || typeof config !== "object" || Array.isArray(config)) {
          return {config: null, error: "Config must be a JSON object."};
        }
        if (!Array.isArray(config.nodes)) {
          return {config: null, error: "Config must contain a nodes array."};
        }
        return {config, error: ""};
      } catch (err) {
        return {config: null, error: err.message};
      }
    }

    function configToText(config) {
      return JSON.stringify(config, null, 2);
    }

    function setConfigText(text, dirty = false, redraw = true) {
      configText = text || "";
      configDirty = dirty;
      const raw = $("config-raw");
      if (raw && raw.value !== configText) raw.value = configText;
      updateConfigControls(lastSnapshot);
      if (redraw) renderConfigFields();
    }

    function nextNodeId(config) {
      const used = new Set((config.nodes || []).map((node) => Number(node.node_id || 0)));
      let id = 1;
      while (used.has(id)) id += 1;
      return id;
    }

    function nextSensorId(config) {
      let id = 1;
      for (const node of config.nodes || []) {
        for (const sensor of node.sensors || []) {
          id = Math.max(id, Number(sensor.sensor_id || 0) + 1);
        }
      }
      return id;
    }

    function nextCanId(config) {
      let canId = 0x0B3;
      for (const node of config.nodes || []) {
        for (const sensor of node.sensors || []) {
          canId = Math.max(canId, parseCanId(sensor.can_id, 0) + 1);
        }
      }
      return canId;
    }

    function commitConfig(config, redraw = false) {
      setConfigText(configToText(config), true, redraw);
    }

    function addConfigNode() {
      const parsed = parseConfigText();
      const config = parsed.config || {version: 1, nodes: []};
      if (!Array.isArray(config.nodes)) config.nodes = [];
      const nodeId = nextNodeId(config);
      config.nodes.push({
        node_id: nodeId,
        name: `sensor_node_${nodeId}`,
        enabled: true,
        active: false,
        state_ack_can_id: formatCanId(0x0C0 + nodeId),
        sensors: [],
      });
      commitConfig(config, true);
    }

    function addConfigSensor(nodeIndex) {
      const parsed = parseConfigText();
      if (!parsed.config) return;
      const config = parsed.config;
      const node = config.nodes[nodeIndex];
      if (!node) return;
      if (!Array.isArray(node.sensors)) node.sensors = [];
      const sensorId = nextSensorId(config);
      node.sensors.push({
        sensor_id: sensorId,
        name: `sensor_${sensorId}`,
        enabled: true,
        can_id: formatCanId(nextCanId(config)),
        units: "raw",
        sample_rate_hz: 10,
        function: "sim",
        port: 0,
      });
      commitConfig(config, true);
    }

    function removeConfigNode(nodeIndex) {
      const parsed = parseConfigText();
      if (!parsed.config) return;
      parsed.config.nodes.splice(nodeIndex, 1);
      commitConfig(parsed.config, true);
    }

    function removeConfigSensor(nodeIndex, sensorIndex) {
      const parsed = parseConfigText();
      if (!parsed.config) return;
      const sensors = parsed.config.nodes[nodeIndex]?.sensors;
      if (!Array.isArray(sensors)) return;
      sensors.splice(sensorIndex, 1);
      commitConfig(parsed.config, true);
    }

    function updateConfigNode(nodeIndex, key, value) {
      const parsed = parseConfigText();
      if (!parsed.config) return;
      const node = parsed.config.nodes[nodeIndex];
      if (!node) return;
      node[key] = value;
      commitConfig(parsed.config, false);
    }

    function updateConfigSensor(nodeIndex, sensorIndex, key, value) {
      const parsed = parseConfigText();
      if (!parsed.config) return;
      const sensor = parsed.config.nodes[nodeIndex]?.sensors?.[sensorIndex];
      if (!sensor) return;
      sensor[key] = value;
      commitConfig(parsed.config, false);
    }

    function fieldHtml(label, value, attrs = "") {
      return `
        <label class="field">
          <span>${label}</span>
          <input value="${escapeHtml(value)}" ${attrs}>
        </label>
      `;
    }

    function selectHtml(label, value, options, attrs = "") {
      const selectedValue = String(value || options[0] || "");
      const optionHtml = options.map((option) => {
        const selected = option === selectedValue ? "selected" : "";
        return `<option value="${escapeHtml(option)}" ${selected}>${escapeHtml(option)}</option>`;
      }).join("");
      return `
        <label class="field">
          <span>${label}</span>
          <select ${attrs}>${optionHtml}</select>
        </label>
      `;
    }

    function renderConfigFields() {
      const fields = $("config-fields");
      if (!fields) return;
      const {config, error} = parseConfigText();
      if (!config) {
        fields.innerHTML = `<div class="subtle">${escapeHtml(error)}</div>`;
        updateConfigControls(lastSnapshot);
        return;
      }

      fields.innerHTML = "";
      config.nodes.forEach((node, nodeIndex) => {
        const card = document.createElement("div");
        card.className = "config-card";
        const sensors = Array.isArray(node.sensors) ? node.sensors : [];
        card.innerHTML = `
          <div class="config-row">
            <label class="field"><span>Enabled</span><input data-kind="node" data-key="enabled" type="checkbox" ${node.enabled !== false ? "checked" : ""}></label>
            ${fieldHtml("Node ID", Number(node.node_id || nodeIndex + 1), 'data-kind="node" data-key="node_id" type="number" min="1" max="255"')}
            ${fieldHtml("Name", node.name || "", 'data-kind="node" data-key="name"')}
            ${fieldHtml("State ACK CAN ID", node.state_ack_can_id || formatCanId(0x0C0 + Number(node.node_id || nodeIndex + 1)), 'data-kind="node" data-key="state_ack_can_id"')}
            <button data-action="remove-node">Remove</button>
          </div>
          <div class="subtle">${sensors.filter((sensor) => sensor.enabled !== false).length}/${sensors.length} sensors enabled</div>
          <div class="sensor-list"></div>
          <button data-action="add-sensor">Add Sensor</button>
        `;

        const sensorList = card.querySelector(".sensor-list");
        sensors.forEach((sensor, sensorIndex) => {
          const row = document.createElement("div");
          row.className = "sensor-row";
          row.innerHTML = `
            <label class="field"><span>Enabled</span><input data-kind="sensor" data-key="enabled" data-sensor="${sensorIndex}" type="checkbox" ${sensor.enabled !== false ? "checked" : ""}></label>
            ${fieldHtml("Sensor ID", Number(sensor.sensor_id || sensorIndex + 1), `data-kind="sensor" data-key="sensor_id" data-sensor="${sensorIndex}" type="number" min="1" max="255"`)}
            ${fieldHtml("Name", sensor.name || "", `data-kind="sensor" data-key="name" data-sensor="${sensorIndex}"`)}
            ${fieldHtml("CAN ID", sensor.can_id || "", `data-kind="sensor" data-key="can_id" data-sensor="${sensorIndex}"`)}
            ${fieldHtml("Units", sensor.units || "raw", `data-kind="sensor" data-key="units" data-sensor="${sensorIndex}"`)}
            ${fieldHtml("Rate Hz", Number(sensor.sample_rate_hz || 10), `data-kind="sensor" data-key="sample_rate_hz" data-sensor="${sensorIndex}" type="number" min="1" max="255"`)}
            ${selectHtml("Function", sensor.function || "sim", sensorFunctionOptions, `data-kind="sensor" data-key="function" data-sensor="${sensorIndex}"`)}
            ${fieldHtml("Port", sensor.port ?? 0, `data-kind="sensor" data-key="port" data-sensor="${sensorIndex}" type="number" min="0" max="255"`)}
            <button data-action="remove-sensor" data-sensor="${sensorIndex}">Remove</button>
          `;
          sensorList.appendChild(row);
        });

        card.addEventListener("change", (event) => {
          const target = event.target;
          if (!(target instanceof HTMLInputElement) && !(target instanceof HTMLSelectElement)) return;
          const kind = target.dataset.kind;
          const key = target.dataset.key;
          let value = target instanceof HTMLInputElement && target.type === "checkbox" ? target.checked : target.value;
          if (["node_id", "sensor_id", "sample_rate_hz"].includes(key)) {
            value = Math.max(1, Math.min(255, Number.parseInt(value || "1", 10) || 1));
          }
          if (key === "port") {
            value = Math.max(0, Math.min(255, Number.parseInt(value || "0", 10) || 0));
          }
          if (key === "can_id" || key === "state_ack_can_id") {
            value = formatCanId(value);
            target.value = value;
          }
          if (kind === "node") updateConfigNode(nodeIndex, key, value);
          if (kind === "sensor") updateConfigSensor(nodeIndex, Number(target.dataset.sensor), key, value);
        });

        card.addEventListener("click", (event) => {
          const target = event.target;
          if (!(target instanceof HTMLButtonElement)) return;
          const action = target.dataset.action;
          if (action === "add-sensor") addConfigSensor(nodeIndex);
          if (action === "remove-node" && confirm(`Remove node ${node.name || node.node_id}?`)) removeConfigNode(nodeIndex);
          if (action === "remove-sensor") removeConfigSensor(nodeIndex, Number(target.dataset.sensor));
        });

        fields.appendChild(card);
      });
      updateConfigControls(lastSnapshot);
    }

    function updateConfigControls(snapshot) {
      if (currentPage !== "config") return;
      const connected = Boolean(snapshot?.connected);
      const idle = snapshot?.logger?.state === "idle";
      const parsed = parseConfigText();
      const errorEl = $("config-error");
      const statusEl = $("config-status");
      if (errorEl) errorEl.textContent = parsed.error && configText.trim() ? parsed.error : "";
      if (statusEl) {
        const status = snapshot?.config?.status || "No config loaded yet.";
        statusEl.textContent = configDirty ? `${status} (unsaved edits)` : status;
      }
      const loadBtn = $("load-config");
      const reloadBtn = $("reload-config");
      const addBtn = $("add-node");
      const formatBtn = $("format-config");
      const saveBtn = $("save-config");
      if (loadBtn) loadBtn.disabled = !connected;
      if (reloadBtn) reloadBtn.disabled = !connected || !idle;
      if (addBtn) addBtn.disabled = !idle;
      if (formatBtn) formatBtn.disabled = !parsed.config;
      if (saveBtn) saveBtn.disabled = !connected || !idle || !parsed.config;
    }

    function renderConfig(snapshot) {
      if (currentPage !== "config") return;
      if (snapshot.connected && !configRequestedOnce) {
        configRequestedOnce = true;
        sendCommand("config_get");
      }

      const loadedAt = snapshot.config?.loaded_at ?? null;
      const incomingText = snapshot.config?.text || "";
      if (incomingText && loadedAt !== configLoadedAt && !configDirty) {
        configLoadedAt = loadedAt;
        setConfigText(incomingText, false, true);
      }
      updateConfigControls(snapshot);
    }

    function formatConfigJson() {
      const parsed = parseConfigText();
      if (!parsed.config) return;
      setConfigText(configToText(parsed.config), true, true);
    }

    function saveConfig() {
      const parsed = parseConfigText();
      if (!parsed.config) return;
      sendCommand("config_save", {config: parsed.config, apply: true});
      configDirty = false;
      updateConfigControls(lastSnapshot);
    }

    function saveChartYRanges() {
      localStorage.setItem("chartYRanges", JSON.stringify(chartYRanges));
    }

    function numberOrNull(value) {
      const text = String(value ?? "").trim();
      if (!text) return null;
      const parsed = Number(text);
      return Number.isFinite(parsed) ? parsed : null;
    }

    function brakePressureSide(signal) {
      const functionName = String(signal.function || "").toLowerCase();
      const name = String(signal.name || "").toLowerCase();
      if (functionName === "front_brake" || name.includes("front_brake") ||
          (name.includes("front") && name.includes("brake"))) {
        return "front";
      }
      if (functionName === "rear_brake" || functionName === "rear_br" ||
          name.includes("rear_brake") || (name.includes("rear") && name.includes("brake"))) {
        return "rear";
      }
      return "";
    }

    function saveBrakePressureOffsets() {
      localStorage.setItem("brakePressureOffsets", JSON.stringify(brakePressureOffsets));
    }

    function adjustedBrakePressureValue(signal, value) {
      const side = brakePressureSide(signal);
      if (!side) return value;
      const offset = Number(brakePressureOffsets[side] || 0);
      return Number(value || 0) - offset;
    }

    function adjustedBrakePressureHistory(signal) {
      const side = brakePressureSide(signal);
      if (!side || !Array.isArray(signal.history)) return signal.history;
      const offset = Number(brakePressureOffsets[side] || 0);
      return signal.history.map((point) => ({...point, value: Number(point.value || 0) - offset}));
    }

    function zeroBrakePressure(side, rawValue) {
      brakePressureOffsets[side] = Number(rawValue || 0);
      saveBrakePressureOffsets();
      renderLatest(lastSnapshot);
      renderCharts(lastSnapshot);
    }

    function clearBrakePressureZero(side) {
      delete brakePressureOffsets[side];
      saveBrakePressureOffsets();
      renderLatest(lastSnapshot);
      renderCharts(lastSnapshot);
    }

    function resetPlayback() {
      playbackById.clear();
      playbackSnapshot = null;
    }

    function pointSampleMs(point, fallbackTimestamp = 0) {
      const sampleMs = Number(point?.sample_ms ?? point?.timestamp ?? fallbackTimestamp ?? 0);
      return Number.isFinite(sampleMs) ? sampleMs : 0;
    }

    function playbackPointKey(point, fallbackTimestamp = 0) {
      return `${pointSampleMs(point, fallbackTimestamp)}:${point?.value ?? 0}`;
    }

    function playbackStateFor(signal) {
      const id = String(signal.id);
      let state = playbackById.get(id);
      if (!state) {
        state = {
          seen: new Set(),
          queue: [],
          history: [],
          latest: null,
        };
        playbackById.set(id, state);
      }
      return state;
    }

    function enqueuePlaybackSamples(snapshot) {
      if (!smoothPlayback || currentPage !== "live" || !snapshot?.can) return;
      const now = performance.now();

      for (const signal of snapshot.can) {
        if (!selectedSignals.has(String(signal.id)) || !Array.isArray(signal.history)) continue;
        const state = playbackStateFor(signal);
        const newPoints = [];
        for (const point of signal.history) {
          const key = playbackPointKey(point, signal.timestamp);
          if (state.seen.has(key)) continue;
          state.seen.add(key);
          newPoints.push(point);
        }
        if (!newPoints.length) continue;

        newPoints.sort((a, b) => pointSampleMs(a, signal.timestamp) - pointSampleMs(b, signal.timestamp));
        const baseSampleMs = pointSampleMs(newPoints[0], signal.timestamp);
        const baseDueMs = now + playbackDelayMs;
        for (const point of newPoints) {
          const sampleMs = pointSampleMs(point, signal.timestamp);
          state.queue.push({
            dueMs: baseDueMs + Math.max(0, sampleMs - baseSampleMs),
            sample: {
              time: Number(point.time ?? Date.now() / 1000),
              sample_ms: sampleMs,
              value: Number(point.value ?? 0),
            },
          });
        }
        state.queue.sort((a, b) => a.dueMs - b.dueMs);
      }
    }

    function promotePlaybackSamples() {
      if (!smoothPlayback || currentPage !== "live") return false;
      const now = performance.now();
      let changed = false;

      for (const state of playbackById.values()) {
        while (state.queue.length && state.queue[0].dueMs <= now) {
          const queued = state.queue.shift();
          state.latest = queued.sample;
          state.history.push(queued.sample);
          if (state.history.length > 600) state.history.shift();
          changed = true;
        }
      }
      if (changed) playbackSnapshot = buildPlaybackSnapshot(lastSnapshot);
      return changed;
    }

    function buildPlaybackSnapshot(snapshot) {
      if (!snapshot?.can) return snapshot;
      return {
        ...snapshot,
        can: snapshot.can.map((signal) => {
          if (!selectedSignals.has(String(signal.id))) return signal;
          const state = playbackById.get(String(signal.id));
          if (!state?.latest) return signal;
          return {
            ...signal,
            value: state.latest.value,
            timestamp: state.latest.sample_ms,
            history: state.history.slice(),
          };
        }),
      };
    }

    function ensurePlaybackAnimation() {
      if (playbackAnimation) return;
      const tick = () => {
        playbackAnimation = null;
        if (!smoothPlayback || currentPage !== "live") return;
        if (promotePlaybackSamples()) {
          renderLatest(playbackSnapshot || lastSnapshot);
          renderCharts(playbackSnapshot || lastSnapshot);
        }
        const hasQueuedSamples = [...playbackById.values()].some((state) => state.queue.length > 0);
        if (hasQueuedSamples) {
          playbackAnimation = requestAnimationFrame(tick);
        }
      };
      playbackAnimation = requestAnimationFrame(tick);
    }

    function downsamplePoints(points, maxPoints) {
      if (!Array.isArray(points) || points.length <= maxPoints) return points || [];
      const sampled = [];
      const step = points.length / maxPoints;
      for (let index = 0; index < maxPoints; index += 1) {
        sampled.push(points[Math.min(Math.floor(index * step), points.length - 1)]);
      }
      return sampled;
    }

    function drawChart(canvas, points, key, color, options = {}) {
      const dpr = window.devicePixelRatio || 1;
      const rect = canvas.getBoundingClientRect();
      const width = Math.max(320, Math.floor(rect.width * dpr));
      const height = Math.max(120, Math.floor(rect.height * dpr));
      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
      }

      const ctx = canvas.getContext("2d");
      ctx.clearRect(0, 0, width, height);
      ctx.lineWidth = 1 * dpr;
      ctx.strokeStyle = "#d6dae2";
      ctx.beginPath();
      ctx.moveTo(36 * dpr, 12 * dpr);
      ctx.lineTo(36 * dpr, (height - 24 * dpr));
      ctx.lineTo((width - 8 * dpr), (height - 24 * dpr));
      ctx.stroke();

      if (!points || points.length < 2) return;

      const latestTime = Number(points.at(-1)?.time ?? 0);
      const windowSeconds = Number(options.windowSeconds || 0);
      if (windowSeconds > 0 && Number.isFinite(latestTime)) {
        points = points.filter((point) => latestTime - Number(point.time ?? latestTime) <= windowSeconds);
      }
      points = downsamplePoints(points, LIVE_CHART_DISPLAY_POINTS);
      if (points.length < 2) return;

      const values = points.map((p) => Number(p[key] ?? p.value ?? 0));
      let min = options.yMin ?? Math.min(...values);
      let max = options.yMax ?? Math.max(...values);
      if (min > max) [min, max] = [max, min];
      if (min === max) {
        min -= 1;
        max += 1;
      }

      const left = 40 * dpr;
      const right = width - 10 * dpr;
      const top = 12 * dpr;
      const bottom = height - 28 * dpr;
      const firstTime = Number(points[0].time ?? latestTime);
      const span = windowSeconds > 0 ? windowSeconds : Math.max(0.001, latestTime - firstTime);

      ctx.strokeStyle = color;
      ctx.lineWidth = 2 * dpr;
      ctx.beginPath();
      points.forEach((point, index) => {
        const value = Number(point[key] ?? point.value ?? 0);
        const time = Number(point.time ?? latestTime);
        const age = latestTime - time;
        const x = right - (age / span) * (right - left);
        const y = bottom - ((value - min) / (max - min)) * (bottom - top);
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();

      ctx.fillStyle = "#667085";
      ctx.font = `${11 * dpr}px system-ui`;
      ctx.fillText(max.toFixed(0), 4 * dpr, 18 * dpr);
      ctx.fillText(min.toFixed(0), 4 * dpr, bottom);
      ctx.fillText(`${windowSeconds > 0 ? windowSeconds : span.toFixed(0)}s`, right - 34 * dpr, bottom + 16 * dpr);
    }

    function renderLatest(snapshot) {
      const latest = $("latest");
      if (!latest) return;
      latest.innerHTML = "";

      if (!snapshot.can.length) {
        latest.innerHTML = `<div class="subtle">Waiting for CAN telemetry.</div>`;
        return;
      }

      for (const signal of snapshot.can) {
        const card = document.createElement("div");
        card.className = "signal-card";
        const units = signal.units ? ` ${signal.units}` : "";
        const brakeSide = brakePressureSide(signal);
        const displayValue = adjustedBrakePressureValue(signal, signal.value);
        const offset = Number(brakePressureOffsets[brakeSide] || 0);
        card.innerHTML = `
          <div class="signal-title">
            <div class="label">${signal.name || signal.id_hex}</div>
            <div class="subtle">${signal.id_hex}</div>
          </div>
          <div class="value">${displayValue}${units}</div>
          <div class="subtle">timestamp ${signal.timestamp} · ${ageText(signal.age_s)}</div>
          ${brakeSide ? `
            <div class="signal-actions">
              <button data-action="zero-brake" data-side="${brakeSide}" data-value="${signal.value}">Zero ${brakeSide}</button>
              <button data-action="clear-brake" data-side="${brakeSide}" ${offset ? "" : "disabled"}>Clear zero</button>
            </div>
            <div class="subtle">raw ${signal.value}${units} · offset ${offset}${units}</div>
          ` : ""}
        `;
        card.querySelector('[data-action="zero-brake"]')?.addEventListener("click", (event) => {
          zeroBrakePressure(event.target.dataset.side, Number(event.target.dataset.value || 0));
        });
        card.querySelector('[data-action="clear-brake"]')?.addEventListener("click", (event) => {
          clearBrakePressureZero(event.target.dataset.side);
        });
        latest.appendChild(card);
      }
    }

    function gpsMetric(label, value) {
      return `
        <div class="signal-card">
          <div class="signal-title">
            <div class="label">${label}</div>
          </div>
          <div class="value">${value}</div>
        </div>
      `;
    }

    function renderGps(snapshot) {
      const gps = $("gps");
      if (!gps) return;

      const data = snapshot.gps;
      if (!data) {
        gps.innerHTML = `<div class="subtle">Waiting for GPS telemetry.</div>`;
        return;
      }

      const valid = Boolean(data.valid);
      const hasLocation = Boolean(data.has_location);
      const hdop = data.hdop_x100 == null ? "" : (Number(data.hdop_x100) / 100).toFixed(2);
      const speed = data.speed_kph_x100 == null ? "" : `${(Number(data.speed_kph_x100) / 100).toFixed(2)} km/h`;
      const age = data.age_s == null ? "" : ageText(Number(data.age_s));
      const lat = data.latitude ?? "";
      const lon = data.longitude ?? "";

      gps.innerHTML = `
        <div class="grid latest-grid">
          ${gpsMetric("Fix", valid ? "valid" : "no fix")}
          ${gpsMetric("Satellites", data.satellites ?? "")}
          ${gpsMetric("HDOP", hdop)}
          ${gpsMetric("Speed", speed)}
        </div>
        <div class="subtle">${hasLocation ? `${lat}, ${lon}` : "No location yet"}${age ? ` · ${age}` : ""}</div>
      `;
    }

    function renderSignalSelector(snapshot) {
      const selector = $("signal-selector");
      if (!selector) return;
      selector.innerHTML = "";

      if (selectedSignals.size === 0 && snapshot.can.length) {
        for (const signal of snapshot.can.slice(0, 3)) {
          selectedSignals.add(String(signal.id));
        }
        saveSelectedSignals();
        syncPreviewSignals();
        connectEvents();
      }
      syncPreviewSignals();

      for (const signal of snapshot.can) {
        const label = document.createElement("label");
        label.className = "check";
        const checked = selectedSignals.has(String(signal.id));
        label.innerHTML = `
          <input type="checkbox" ${checked ? "checked" : ""} data-id="${signal.id}">
          <span>${signal.name || signal.id_hex}</span>
        `;
        label.querySelector("input").addEventListener("change", (event) => {
          const id = String(event.target.dataset.id);
          if (event.target.checked) selectedSignals.add(id);
          else selectedSignals.delete(id);
          saveSelectedSignals();
          resetPlayback();
          syncPreviewSignals();
          connectEvents();
          renderCharts(lastSnapshot);
        });
        selector.appendChild(label);
      }
    }

    function renderCharts(snapshot) {
      const charts = $("charts");
      if (!charts) return;
      if (!snapshot) return;

      const selected = snapshot.can.filter((signal) => selectedSignals.has(String(signal.id)));
      if (!selected.length) {
        charts.innerHTML = `<div class="subtle">Select one or more signals to show charts.</div>`;
        return;
      }

      const selectedIds = new Set(selected.map((signal) => String(signal.id)));
      charts.querySelectorAll(".chart-card").forEach((card) => {
        if (!selectedIds.has(String(card.dataset.id))) card.remove();
      });
      charts.querySelectorAll(":scope > .subtle").forEach((message) => message.remove());

      selected.forEach((signal, index) => {
        const id = String(signal.id);
        const range = chartYRanges[id] || {};
        let card = charts.querySelector(`.chart-card[data-id="${id}"]`);
        if (!card) {
          card = document.createElement("div");
          card.className = "chart-card";
          card.dataset.id = id;
          card.innerHTML = `
            <div class="chart-head">
              <div>
                <div class="label chart-label"></div>
                <div class="subtle chart-meta"></div>
              </div>
              <button class="remove" data-id="${signal.id}">Remove</button>
            </div>
            <div class="chart-controls">
              <label>Y min <input class="chart-y-min" data-id="${signal.id}" value="${range.min ?? ""}" placeholder="auto"></label>
              <label>Y max <input class="chart-y-max" data-id="${signal.id}" value="${range.max ?? ""}" placeholder="auto"></label>
            </div>
            <canvas></canvas>
          `;
          card.querySelector("button").addEventListener("click", (event) => {
            selectedSignals.delete(String(event.target.dataset.id));
            saveSelectedSignals();
            resetPlayback();
            syncPreviewSignals();
            connectEvents();
            renderSignalSelector(snapshot);
            renderCharts(snapshot);
          });
          card.querySelectorAll(".chart-y-min,.chart-y-max").forEach((input) => {
            input.addEventListener("change", (event) => {
              const inputId = String(event.target.dataset.id);
              const existing = chartYRanges[inputId] || {};
              if (event.target.classList.contains("chart-y-min")) existing.min = event.target.value.trim();
              if (event.target.classList.contains("chart-y-max")) existing.max = event.target.value.trim();
              if (!existing.min && !existing.max) delete chartYRanges[inputId];
              else chartYRanges[inputId] = existing;
              saveChartYRanges();
              renderCharts(lastSnapshot);
            });
          });
        }

        card.querySelector(".chart-label").textContent = signal.name || signal.id_hex;
        const displayValue = adjustedBrakePressureValue(signal, signal.value);
        card.querySelector(".chart-meta").textContent =
          `${signal.id_hex} · latest ${displayValue}${signal.units ? " " + signal.units : ""}`;
        charts.appendChild(card);
        const yMinInput = card.querySelector(".chart-y-min");
        const yMaxInput = card.querySelector(".chart-y-max");
        drawChart(card.querySelector("canvas"), adjustedBrakePressureHistory(signal), "value", chartColors[index % chartColors.length], {
          windowSeconds: chartTimeWindowSeconds,
          yMin: numberOrNull(yMinInput?.value),
          yMax: numberOrNull(yMaxInput?.value),
        });
      });
    }

    function healthLevel(board) {
      if (board.age_s > 3) return "bad";
      if ((board.missed_deadlines || 0) > 0 || (board.max_lateness_ms || 0) > 25 ||
          (board.load_percent || 0) > 85 || (board.queue_depth || 0) > 32 ||
          (board.sd_write_last_ms || 0) > 250) {
        return "warn";
      }
      return "good";
    }

    function renderHealth(snapshot) {
      const health = $("health");
      if (!health) return;
      health.innerHTML = "";

      if (!snapshot.health.length) {
        health.innerHTML = `<div class="subtle">Waiting for board health telemetry.</div>`;
        return;
      }

      for (const board of snapshot.health) {
        const level = healthLevel(board);
        const load = Math.max(0, Math.min(100, Number(board.load_percent || 0)));
        const card = document.createElement("div");
        card.className = "health-card";
        card.innerHTML = `
          <div class="health-title">
            <div class="label">${board.name}</div>
            <span class="pill ${level}">${level === "good" ? "ok" : level}</span>
          </div>
          <div class="subtle">${board.source} · ${board.active ? "active" : "idle"} · ${ageText(board.age_s)}</div>
          <div class="bar"><span class="${level}" style="width:${load}%"></span></div>
          <table>
            <tr><th>Load</th><td>${load.toFixed(0)}%</td><th>Queue</th><td>${board.queue_depth ?? ""}</td></tr>
            <tr><th>Late</th><td>${board.max_lateness_ms ?? board.sd_write_last_ms ?? ""} ms</td><th>Missed</th><td>${board.missed_deadlines ?? ""}</td></tr>
            <tr><th>TX fail</th><td>${board.tx_fail_count ?? ""}</td><th>Heap</th><td>${board.free_heap_kb ?? ""} KB</td></tr>
          </table>
          <canvas></canvas>
        `;
        health.appendChild(card);
        drawChart(card.querySelector("canvas"), board.history, "load_percent", level === "bad" ? "#b42318" : "#1769aa");
      }
    }

    function renderNodes(snapshot) {
      const nodes = $("nodes");
      if (!nodes) return;
      const list = snapshot.nodes || [];
      if (!list.length) {
        nodes.innerHTML = `<div class="subtle">No node status yet.</div>`;
        return;
      }

      nodes.innerHTML = `
        <table>
          <thead><tr><th>Node</th><th>Active</th><th>Last seen</th></tr></thead>
          <tbody>
            ${list.map((node) => `
              <tr>
                <td>${node.name || node.node_id}</td>
                <td>${node.active ? "yes" : "no"}</td>
                <td>${node.last_seen_rx_us || 0}</td>
              </tr>
            `).join("")}
          </tbody>
        </table>
      `;
    }

    function renderFiles(snapshot) {
      const filesEl = $("files");
      const refresh = $("refresh-files");
      if (!filesEl || !refresh) return;
      refresh.disabled = !snapshot.connected;
      filesEl.innerHTML = "";

      if (snapshot.connected && !filesRequestedOnce && snapshot.files_age_s == null) {
        filesRequestedOnce = true;
        sendCommand("files");
      }

      const sd = snapshot.sd || null;
      if (sd) {
        const used = Number(sd.used_gb || 0);
        const free = Number(sd.free_gb || 0);
        const total = Number(sd.total_gb || 0);
        const pct = Number(sd.used_percent || 0);
        const grid = document.createElement("div");
        grid.className = "sd-grid";
        grid.innerHTML = `
          <div class="metric"><span class="subtle">Used</span><strong>${used.toFixed(2)} GB</strong><span class="subtle">${pct.toFixed(1)}%</span></div>
          <div class="metric"><span class="subtle">Free</span><strong>${free.toFixed(2)} GB</strong></div>
          <div class="metric"><span class="subtle">Total</span><strong>${total.toFixed(2)} GB</strong></div>
        `;
        filesEl.appendChild(grid);
      }

      const status = document.createElement("div");
      status.className = "subtle";
      status.textContent = snapshot.files_age_s == null
        ? "No SD file list received yet."
        : `Last updated ${ageText(snapshot.files_age_s)}`;
      filesEl.appendChild(status);

      const files = Array.isArray(snapshot.files) ? snapshot.files : [];
      if (!files.length) {
        const empty = document.createElement("div");
        empty.className = "subtle";
        empty.style.marginTop = "8px";
        empty.textContent = "Press Refresh to list SD card files.";
        filesEl.appendChild(empty);
        renderDownload(snapshot, filesEl);
        return;
      }

      const table = document.createElement("table");
      table.innerHTML = `<thead><tr><th>File</th><th>Size</th><th></th></tr></thead>`;
      const body = document.createElement("tbody");
      for (const file of files) {
        const name = String(file.name || "");
        const sizeBytes = Number(file.size_bytes || 0);
        const row = document.createElement("tr");
        const nameCell = document.createElement("td");
        const sizeCell = document.createElement("td");
        const actionCell = document.createElement("td");
        const button = document.createElement("button");

        nameCell.textContent = name;
        sizeCell.textContent = formatBytes(sizeBytes);
        button.textContent = "Download";
        button.disabled = !snapshot.connected || snapshot.logger.state !== "idle" || snapshot.download.active;
        button.addEventListener("click", () => sendCommand("download", {file: name}));
        actionCell.appendChild(button);

        row.appendChild(nameCell);
        row.appendChild(sizeCell);
        row.appendChild(actionCell);
        body.appendChild(row);
      }
      table.appendChild(body);
      filesEl.appendChild(table);

      if (snapshot.logger.state !== "idle") {
        const note = document.createElement("div");
        note.className = "subtle";
        note.textContent = "Stop logging before downloading a file.";
        filesEl.appendChild(note);
      }

      renderDownload(snapshot, filesEl);
    }

    function renderDownload(snapshot, parent) {
      const download = snapshot.download || {};
      if (!download.name && !download.error) return;

      const box = document.createElement("div");
      box.className = "download-box";
      const expected = Number(download.expected_size || 0);
      const received = Number(download.received_bytes || 0);
      const pct = expected > 0 ? Math.min(100, (received / expected) * 100) : 0;
      const title = escapeHtml(download.name || "download");

      if (download.active) {
        box.innerHTML = `
          <div class="label">Downloading ${title}</div>
          <div class="progress"><span style="width:${pct}%"></span></div>
          <div class="subtle">${formatBytes(received)} / ${expected ? formatBytes(expected) : "unknown"}</div>
        `;
      } else if (download.error) {
        box.innerHTML = `
          <div class="label">Download failed</div>
          <div class="subtle">${escapeHtml(download.error)}</div>
        `;
      } else if (download.complete) {
        box.innerHTML = `
          <div class="label">Download ready</div>
          <div class="subtle">${title} · ${formatBytes(received)}</div>
          <a class="download-link" href="/downloaded" download="${title}">Save File</a>
        `;
      }
      parent.appendChild(box);
    }

    function render(snapshot) {
      const scrollEl = document.scrollingElement || document.documentElement;
      const beforeTop = scrollEl.scrollTop;
      const beforeHeight = scrollEl.scrollHeight;
      const wasAtBottom = beforeHeight - beforeTop - window.innerHeight < 8;

      lastSnapshot = snapshot;
      if (currentPage === "live" && smoothPlayback) {
        enqueuePlaybackSamples(snapshot);
        promotePlaybackSamples();
        playbackSnapshot = buildPlaybackSnapshot(snapshot);
        ensurePlaybackAnimation();
      } else if (currentPage === "live") {
        playbackSnapshot = null;
      }
      const displaySnapshot = playbackSnapshot || snapshot;

      setPill("mqtt", snapshot.connected ? "MQTT connected" : "MQTT offline", snapshot.connected ? "good" : "bad");
      setPill("feed", `feed ${snapshot.message_rate.toFixed(1)}/s`, snapshot.message_rate > 0 ? "good" : "warn");
      setPill("logger", `logger ${snapshot.logger.state}`, snapshot.logger.state === "running" ? "good" : "");
      $("start").disabled = !snapshot.connected || snapshot.logger.state === "running";
      $("stop").disabled = !snapshot.connected || snapshot.logger.state !== "running";

      if (currentPage === "live") {
        renderGps(displaySnapshot);
        renderLatest(displaySnapshot);
        renderSignalSelector(snapshot);
        renderCharts(displaySnapshot);
      } else if (currentPage === "health") {
        renderHealth(snapshot);
      } else if (currentPage === "files") {
        renderFiles(snapshot);
      } else if (currentPage === "nodes") {
        renderNodes(snapshot);
      } else if (currentPage === "config") {
        renderConfig(snapshot);
      }

      requestAnimationFrame(() => {
        const afterHeight = scrollEl.scrollHeight;
        scrollEl.scrollTop = wasAtBottom ? Math.max(0, afterHeight - window.innerHeight) : beforeTop;
      });
    }

    $("start").addEventListener("click", () => sendCommand("start"));
    $("stop").addEventListener("click", () => sendCommand("stop"));
    $("pause").addEventListener("click", () => {
      paused = !paused;
      $("pause").textContent = paused ? "Resume" : "Pause";
    });

    setupPage();

    connectEvents();
  </script>
</body>
</html>
"""


@dataclass
class DownloadTransfer:
    name: str = ""
    request_id: int | None = None
    expected_size: int = 0
    received_bytes: int = 0
    expected_chunks: int = 0
    chunks: dict[int, bytes] = field(default_factory=dict)
    active: bool = False
    complete: bool = False
    error: str = ""
    started_at: float = 0.0
    finished_at: float = 0.0
    data: bytes = b""


@dataclass
class DashboardState:
    lock: threading.Lock = field(default_factory=threading.Lock)
    connected: bool = False
    error: str = ""
    status: dict[str, Any] = field(default_factory=dict)
    files: list[dict[str, Any]] = field(default_factory=list)
    sd: dict[str, Any] | None = None
    last_files_at: float | None = None
    download: DownloadTransfer = field(default_factory=DownloadTransfer)
    config_text: str = ""
    config_status: str = ""
    last_config_at: float | None = None
    latest_can: dict[int, dict[str, Any]] = field(default_factory=dict)
    can_history: dict[int, deque[dict[str, Any]]] = field(
        default_factory=lambda: defaultdict(lambda: deque(maxlen=MAX_HISTORY))
    )
    latest_gps: dict[str, Any] = field(default_factory=dict)
    last_gps_at: float | None = None
    latest_health: dict[str, dict[str, Any]] = field(default_factory=dict)
    health_history: dict[str, deque[dict[str, Any]]] = field(
        default_factory=lambda: defaultdict(lambda: deque(maxlen=MAX_HISTORY))
    )
    received_times: deque[float] = field(default_factory=lambda: deque(maxlen=1000))
    command_status: str = ""
    mqtt_client: mqtt.Client | None = None

    def sensor_metadata_by_can_id(self) -> dict[int, dict[str, str]]:
        metadata: dict[int, dict[str, str]] = {}
        master_sensors = self.status.get("master_sensors", [])
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
                    "function": str(sensor.get("function", "")),
                }

        nodes = self.status.get("nodes", [])
        if not isinstance(nodes, list):
            return metadata

        for node in nodes:
            if not isinstance(node, dict):
                continue
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
                    "function": str(sensor.get("function", "")),
                }
        return metadata

    def message_rate(self) -> float:
        now = time.time()
        return sum(1 for item in self.received_times if now - item <= 5.0) / 5.0

    @staticmethod
    def is_gps_speed_sensor(metadata: dict[str, str]) -> bool:
        return (
            str(metadata.get("function", "")).lower() == "gps_speed"
            or str(metadata.get("units", "")).lower() == "kph_x100"
        )

    def display_units(self, metadata: dict[str, str]) -> str:
        return "km/h" if self.is_gps_speed_sensor(metadata) else str(metadata.get("units", ""))

    def display_value(self, value: Any, metadata: dict[str, str]) -> float | int:
        try:
            numeric = float(value)
        except (TypeError, ValueError):
            numeric = 0.0
        if self.is_gps_speed_sensor(metadata):
            return numeric / 100.0
        return int(numeric) if numeric.is_integer() else numeric

    def display_history(self, history: list[dict[str, Any]], metadata: dict[str, str]) -> list[dict[str, Any]]:
        if not self.is_gps_speed_sensor(metadata):
            return history
        return [
            {
                **point,
                "value": self.display_value(point.get("value", 0), metadata),
            }
            for point in history
        ]

    def snapshot(self, view: str = "live", selected_can_ids: set[int] | None = None) -> dict[str, Any]:
        with self.lock:
            now = time.time()
            logger = self.status.get("logger", {}) if isinstance(self.status, dict) else {}
            nodes = self.status.get("nodes", []) if isinstance(self.status, dict) else []
            include_can = view == "live"
            include_health = view == "health"
            include_files = view == "files"
            include_nodes = view == "nodes"
            include_config = view == "config"

            can_signals = []
            if include_can:
                sensor_metadata = self.sensor_metadata_by_can_id()
                for can_id, latest in sorted(self.latest_can.items()):
                    metadata = sensor_metadata.get(can_id, {})
                    history = (
                        list(self.can_history[can_id])
                        if selected_can_ids is None or can_id in selected_can_ids
                        else []
                    )
                    can_signals.append(
                        {
                            "id": can_id,
                            "id_hex": latest.get("id_hex", f"0x{can_id:03X}"),
                            "name": metadata.get("name", latest.get("id_hex", f"0x{can_id:03X}")),
                            "units": self.display_units(metadata),
                            "function": metadata.get("function", ""),
                            "value": self.display_value(latest.get("value", 0), metadata),
                            "timestamp": latest.get("timestamp", 0),
                            "age_s": now - float(latest.get("received_at", now)),
                            "history": self.display_history(history, metadata),
                        }
                    )

            health = []
            if include_health:
                for key, latest in sorted(self.latest_health.items()):
                    source = str(latest.get("source", "board"))
                    node_id = latest.get("node_id", "")
                    name = str(latest.get("name") or (f"node_{node_id}" if source == "node" else source))
                    queue_depth = latest.get("sample_queue_depth", latest.get("rx_queue_depth", 0))
                    health.append(
                        {
                            "key": key,
                            "source": source,
                            "node_id": node_id,
                            "name": name,
                            "active": bool(latest.get("active", False)),
                            "load_percent": latest.get("load_percent", 0),
                            "max_lateness_ms": latest.get("max_lateness_ms"),
                            "missed_deadlines": latest.get("missed_deadlines"),
                            "queue_depth": queue_depth,
                            "sample_queue_drops": latest.get("sample_queue_drops"),
                            "tx_fail_count": latest.get("tx_fail_count"),
                            "sd_write_last_ms": latest.get("sd_write_last_ms"),
                            "free_heap_kb": latest.get("free_heap_kb"),
                            "age_s": now - float(latest.get("received_at", now)),
                            "history": [
                                {
                                    "time": item.get("time", 0),
                                    "load_percent": item.get("load_percent", 0),
                                    "timing_ms": item.get("timing_ms", 0),
                                    "queue_depth": item.get("queue_depth", 0),
                                }
                                for item in self.health_history[key]
                            ],
                        }
                    )

            return {
                "connected": self.connected,
                "error": self.error,
                "message_rate": self.message_rate(),
                "logger": {
                    "state": str(logger.get("state", "unknown")),
                    "file": str(logger.get("file", "")),
                    "blocks": int(logger.get("blocks", 0) or 0),
                },
                "nodes": nodes if include_nodes and isinstance(nodes, list) else [],
                "can": can_signals,
                "gps": {
                    **self.latest_gps,
                    "age_s": None if self.last_gps_at is None else now - self.last_gps_at,
                } if include_can and self.latest_gps else None,
                "health": health,
                "files": self.files if include_files else [],
                "sd": self.sd if include_files else None,
                "files_age_s": (
                    None
                    if not include_files or self.last_files_at is None
                    else now - self.last_files_at
                ),
                "download": {
                    "name": self.download.name,
                    "request_id": self.download.request_id,
                    "expected_size": self.download.expected_size,
                    "received_bytes": self.download.received_bytes,
                    "active": self.download.active,
                    "complete": self.download.complete,
                    "error": self.download.error,
                },
                "config": {
                    "text": self.config_text if include_config else "",
                    "status": self.config_status if include_config else "",
                    "age_s": (
                        None
                        if not include_config or self.last_config_at is None
                        else now - self.last_config_at
                    ),
                    "loaded_at": self.last_config_at if include_config else None,
                },
                "command_status": self.command_status,
            }


class BajaDashboard:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.state = DashboardState()

    def start_mqtt(self) -> None:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        client.on_connect = self.on_connect
        client.on_disconnect = self.on_disconnect
        client.on_message = self.on_message
        self.state.mqtt_client = client
        client.connect_async(self.args.broker, self.args.mqtt_port, keepalive=10)
        client.loop_start()

    def on_connect(
        self,
        client: mqtt.Client,
        userdata: Any,
        flags: mqtt.ConnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del userdata, flags, properties
        with self.state.lock:
            self.state.connected = reason_code == 0
            self.state.error = "" if reason_code == 0 else f"MQTT connect failed: {reason_code}"
        if reason_code == 0:
            client.subscribe(self.args.topic)

    def on_disconnect(
        self,
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: mqtt.DisconnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del client, userdata, disconnect_flags, properties
        with self.state.lock:
            self.state.connected = False
            if reason_code != 0:
                self.state.error = f"MQTT disconnected: {reason_code}"

    def on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        del client, userdata
        try:
            payload = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if not isinstance(payload, dict):
            return

        received_at = time.time()
        topic = message.topic
        with self.state.lock:
            self.state.received_times.append(received_at)
            if topic.endswith("/status"):
                self.state.status = payload
            elif topic.endswith("/files"):
                self.handle_files(payload, received_at)
            elif topic.endswith("/download"):
                self.handle_download(payload, received_at)
            elif topic.endswith("/config"):
                self.handle_config(payload, received_at)
            elif topic.endswith("/can"):
                self.handle_can(payload, received_at)
            elif topic.endswith("/gps"):
                self.state.latest_gps = payload
                self.state.last_gps_at = received_at
            elif topic.endswith("/health"):
                self.handle_health(payload, received_at)

    def reset_download_locked(self, name: str, request_id: int | None) -> None:
        self.state.download = DownloadTransfer(
            name=name,
            request_id=request_id,
            active=True,
            started_at=time.time(),
        )

    def handle_files(self, payload: dict[str, Any], received_at: float) -> None:
        if payload.get("ok") is False:
            self.state.error = f"SD file list failed: {payload.get('error', 'unknown error')}"
            self.state.last_files_at = received_at
            return
        files = payload.get("files", [])
        self.state.files = files if isinstance(files, list) else []
        sd = payload.get("sd", None)
        self.state.sd = sd if isinstance(sd, dict) else None
        self.state.error = ""
        self.state.last_files_at = received_at

    def handle_download(self, payload: dict[str, Any], received_at: float) -> None:
        event = str(payload.get("event", ""))
        name = str(payload.get("name", ""))
        request_id_value = payload.get("request_id")
        try:
            request_id = int(request_id_value) if request_id_value is not None else None
        except (TypeError, ValueError):
            request_id = None

        if event == "begin":
            self.reset_download_locked(name, request_id)
            self.state.download.expected_size = int(payload.get("size", 0) or 0)
            self.state.download.error = ""
            return

        download = self.state.download
        if request_id is not None and download.request_id is not None and request_id != download.request_id:
            return

        if event == "error":
            download.active = False
            download.complete = False
            download.finished_at = received_at
            reason = str(payload.get("reason", "unknown_error"))
            code_name = str(payload.get("code_name", ""))
            download.error = f"{reason} {code_name}".strip()
            return

        if event == "chunk":
            try:
                seq = int(payload["seq"])
                decoded = base64.b64decode(str(payload["data"]), validate=True)
            except (KeyError, TypeError, ValueError, binascii.Error):
                download.error = "bad download chunk"
                return

            if not download.active:
                self.reset_download_locked(name, request_id)
                download = self.state.download

            if seq not in download.chunks:
                download.chunks[seq] = decoded
                download.received_bytes += len(decoded)
            return

        if event == "end":
            download.active = False
            download.finished_at = received_at
            download.expected_chunks = int(payload.get("chunks", 0) or 0)
            end_bytes = int(payload.get("bytes", 0) or 0)
            expected_bytes = download.expected_size or end_bytes
            if expected_bytes and end_bytes != expected_bytes:
                download.complete = False
                download.error = f"incomplete transfer: got {end_bytes} bytes, expected {expected_bytes}"
                return

            missing = [seq for seq in range(download.expected_chunks) if seq not in download.chunks]
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
            download.received_bytes = len(data)
            download.complete = True
            download.error = ""

    def handle_config(self, payload: dict[str, Any], received_at: float) -> None:
        event = str(payload.get("event", "config"))
        ok = bool(payload.get("ok", False))
        if event == "config" and ok:
            self.state.config_text = str(payload.get("config_text", ""))
            self.state.last_config_at = received_at
            source = str(payload.get("source", "master"))
            self.state.config_status = f"Loaded config from {source}"
            return

        code_name = str(payload.get("code_name", "")).strip()
        self.state.config_status = f"{event}: {'ok' if ok else code_name or 'failed'}"

    def handle_can(self, payload: dict[str, Any], received_at: float) -> None:
        frames = payload.get("frames")
        if isinstance(frames, list):
            for frame in frames:
                if isinstance(frame, dict):
                    self.handle_can(frame, received_at)
            return

        try:
            can_id = int(payload["id"])
        except (KeyError, TypeError, ValueError):
            return

        item = {
            "id": can_id,
            "id_hex": str(payload.get("id_hex", f"0x{can_id:03X}")),
            "timestamp": int(payload.get("timestamp", 0) or 0),
            "value": int(payload.get("value", 0) or 0),
            "received_at": received_at,
        }
        self.state.latest_can[can_id] = item
        self.state.can_history[can_id].append(
            {
                "time": received_at,
                "sample_ms": item["timestamp"],
                "value": item["value"],
            }
        )

    def handle_health(self, payload: dict[str, Any], received_at: float) -> None:
        source = str(payload.get("source", "board"))
        node_id = payload.get("node_id", "unknown")
        key = f"{source}:{node_id}"
        item = dict(payload)
        item["received_at"] = received_at
        self.state.latest_health[key] = item
        self.state.health_history[key].append(
            {
                "time": received_at,
                "load_percent": float(payload.get("load_percent", 0) or 0),
                "timing_ms": float(payload.get("max_lateness_ms", payload.get("sd_write_last_ms", 0)) or 0),
                "queue_depth": float(payload.get("sample_queue_depth", payload.get("rx_queue_depth", 0)) or 0),
            }
        )

    def publish_command(self, command: str, **fields: Any) -> tuple[bool, str]:
        client = self.state.mqtt_client
        if client is None:
            return False, "MQTT client is not ready"

        with self.state.lock:
            connected = self.state.connected
        if not connected:
            return False, "MQTT is not connected"

        message = {"cmd": command, "token": self.args.command_token}
        message.update(fields)
        payload = json.dumps(message, separators=(",", ":"))
        info = client.publish(self.args.command_topic, payload=payload, qos=0, retain=False)
        ok = info.rc == mqtt.MQTT_ERR_SUCCESS
        message = f"sent {command}" if ok else f"publish failed rc={info.rc}"
        with self.state.lock:
            self.state.command_status = message
        return ok, message

    def request_download(self, filename: str) -> tuple[bool, str]:
        if not filename:
            return False, "missing file name"

        request_id = time.time_ns() & 0xFFFFFFFF
        with self.state.lock:
            logger = self.state.status.get("logger", {}) if isinstance(self.state.status, dict) else {}
            logger_state = str(logger.get("state", "unknown"))
            if logger_state != "idle":
                return False, "stop logging before downloading"
            self.reset_download_locked(filename, request_id)

        ok, message = self.publish_command("download", file=filename, request_id=request_id)
        if not ok:
            with self.state.lock:
                self.state.download.active = False
                self.state.download.error = message
        return ok, message


def make_handler(app: BajaDashboard) -> type[BaseHTTPRequestHandler]:
    def selected_can_ids(query: str) -> set[int] | None:
        values = parse_qs(query).get("selected", [])
        if not values:
            return None

        selected: set[int] = set()
        for value in values[0].split(","):
            try:
                selected.add(int(value))
            except ValueError:
                continue
        return selected

    def normalized_view(path: str, query: str = "") -> str:
        if query:
            values = parse_qs(query).get("view", [])
            if values and values[0] in VALID_VIEWS:
                return values[0]

        page = path.strip("/") or "live"
        return page if page in VALID_VIEWS else "live"

    class Handler(BaseHTTPRequestHandler):
        def handle(self) -> None:
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError):
                return

        def log_message(self, fmt: str, *args: Any) -> None:
            return

        def send_json(self, payload: dict[str, Any], status: int = HTTPStatus.OK) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_HEAD(self) -> None:
            parsed = urlparse(self.path)
            if parsed.path in {"/", "/live", "/health", "/files", "/nodes", "/config"}:
                body_len = len(INDEX_HTML.encode("utf-8"))
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(body_len))
                self.end_headers()
                return

            if parsed.path == "/snapshot":
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                return

            if parsed.path == "/favicon.ico":
                self.send_response(HTTPStatus.NO_CONTENT)
                self.end_headers()
                return

            self.send_error(HTTPStatus.NOT_FOUND)

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            path = parsed.path
            if path in {"/", "/live", "/health", "/files", "/nodes", "/config"}:
                body = INDEX_HTML.encode("utf-8")
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if path == "/snapshot":
                self.send_json(
                    app.state.snapshot(
                        normalized_view(path, parsed.query),
                        selected_can_ids(parsed.query),
                    )
                )
                return

            if path == "/events":
                view = normalized_view(path, parsed.query)
                selected = selected_can_ids(parsed.query)
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                while True:
                    try:
                        payload = json.dumps(
                            app.state.snapshot(view, selected),
                            separators=(",", ":"),
                        )
                        self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                        self.wfile.flush()
                        if view == "live":
                            period = SSE_PERIOD_SECONDS
                        elif view == "health":
                            period = HEALTH_SSE_PERIOD_SECONDS
                        else:
                            period = SLOW_SSE_PERIOD_SECONDS
                        if view == "files":
                            with app.state.lock:
                                if app.state.download.active:
                                    period = SSE_PERIOD_SECONDS
                        time.sleep(period)
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        break
                return

            if path == "/downloaded":
                with app.state.lock:
                    download = app.state.download
                    complete = download.complete
                    data = download.data
                    expected_size = download.expected_size
                    filename = download.name or "download.bin"

                if not complete or (expected_size > 0 and len(data) == 0):
                    self.send_error(HTTPStatus.NOT_FOUND, "download is not ready")
                    return

                safe_filename = filename.rsplit("/", 1)[-1].replace("\\", "_").replace('"', "")
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Content-Disposition", f'attachment; filename="{safe_filename}"')
                self.end_headers()
                self.wfile.write(data)
                return

            if path == "/favicon.ico":
                self.send_response(HTTPStatus.NO_CONTENT)
                self.end_headers()
                return

            self.send_error(HTTPStatus.NOT_FOUND)

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            if path != "/command":
                self.send_error(HTTPStatus.NOT_FOUND)
                return

            length = int(self.headers.get("Content-Length", "0") or 0)
            try:
                body = self.rfile.read(length)
                payload = json.loads(body.decode("utf-8")) if body else {}
            except (UnicodeDecodeError, json.JSONDecodeError):
                self.send_json({"ok": False, "error": "invalid JSON"}, HTTPStatus.BAD_REQUEST)
                return

            command = str(payload.get("cmd", "")).strip().lower()
            if command not in {
                "start",
                "stop",
                "files",
                "status",
                "config_get",
                "config_reload",
                "config_save",
                "download",
                "preview_config",
            }:
                self.send_json({"ok": False, "error": "unsupported command"}, HTTPStatus.BAD_REQUEST)
                return

            if command == "download":
                filename = str(payload.get("file") or payload.get("filename") or "").strip()
                ok, message = app.request_download(filename)
            elif command == "config_save":
                fields: dict[str, Any] = {"apply": bool(payload.get("apply", True))}
                if "config" in payload:
                    fields["config"] = payload["config"]
                if "config_text" in payload:
                    fields["config_text"] = payload["config_text"]
                if "config" not in fields and "config_text" not in fields:
                    self.send_json({"ok": False, "error": "missing config"}, HTTPStatus.BAD_REQUEST)
                    return
                ok, message = app.publish_command(command, **fields)
            elif command == "preview_config":
                can_ids = payload.get("can_ids", [])
                if not isinstance(can_ids, list):
                    self.send_json({"ok": False, "error": "can_ids must be a list"}, HTTPStatus.BAD_REQUEST)
                    return
                ok, message = app.publish_command(command, can_ids=can_ids[:8])
            else:
                ok, message = app.publish_command(command)
            self.send_json({"ok": ok, "message": message}, HTTPStatus.OK if ok else HTTPStatus.SERVICE_UNAVAILABLE)

    return Handler


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fast live dashboard for Baja logger MQTT telemetry")
    parser.add_argument("--broker", default=DEFAULT_BROKER, help="MQTT broker hostname")
    parser.add_argument("--mqtt-port", type=int, default=DEFAULT_MQTT_PORT, help="MQTT broker port")
    parser.add_argument("--topic", default=DEFAULT_TOPIC, help="MQTT subscription topic")
    parser.add_argument("--command-topic", default=DEFAULT_COMMAND_TOPIC, help="MQTT command topic")
    parser.add_argument("--command-token", default=DEFAULT_COMMAND_TOKEN, help="MQTT command token")
    parser.add_argument("--web-host", default=DEFAULT_WEB_HOST, help="HTTP bind host")
    parser.add_argument("--web-port", type=int, default=DEFAULT_WEB_PORT, help="HTTP bind port")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    app = BajaDashboard(args)
    app.start_mqtt()

    server = ThreadingHTTPServer((args.web_host, args.web_port), make_handler(app))
    print(f"Fast dashboard: http://{args.web_host}:{args.web_port}")
    print(f"MQTT broker: {args.broker}:{args.mqtt_port} topic={args.topic}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping dashboard")
    finally:
        if app.state.mqtt_client is not None:
            app.state.mqtt_client.loop_stop()
            app.state.mqtt_client.disconnect()
        server.server_close()


if __name__ == "__main__":
    main()
