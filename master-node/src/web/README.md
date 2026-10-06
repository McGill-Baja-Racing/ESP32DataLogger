# Browser interface and HTTP implementation

Read the [master source map](../README.md) first. This guide routes UI/server work; [Log format](../../../docs/LOG_FORMAT.md) defines downloads.

## Where the page comes from

`index.html` contains the browser interface, styling, and JavaScript. `index_html.S` embeds that asset into firmware, and the PlatformIO pre-build script `tools/web_asset_dependencies.py` makes edits participate in rebuilds. `web_server.c` starts ESP-Hosted networking and the HTTP server, serves the page, and implements handlers. There is no separate frontend package/build system.

A firmware build confirms asset integration. The page's JavaScript expects the device API; opening the HTML locally does not validate a full recording session.

## Control and live data

Handlers delegate recording actions through `app_control` rather than changing logger state directly. `live_data` holds latest values independently of native-rate logging. A single viewer lease controls live mode; tokens and expiry prevent abandoned viewers from keeping it active indefinitely. Preserve that ownership when changing graphs or polling.

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | Embedded page |
| `/api/status` | GET | Logger, nodes, counters, live state |
| `/api/logging/start`, `/api/logging/stop` | POST | Recording controls |
| `/api/logs` | GET | Completed log listing |
| `/api/logs/rename` | POST | Completed session rename |
| `/api/logs/download` | GET | Binary, full CSV, or Powertrain CSV |
| `/api/live/signals` | GET | Available signal metadata |
| `/api/live/start`, `/api/live/stop` | POST | Viewer lifecycle |
| `/api/live/samples` | GET | Latest-value polling |

Read the current handler and corresponding `index.html` caller before changing request/response fields; this table is a route map, not a substitute for the actual interface.

## Downloads and validation

CSV is streamed from completed binary records; UTC companion entries are consumed for every record, including those omitted from Powertrain output. Preserve this alignment. Signal names/units must remain consistent with protocol and offline decoding.

From the repository root, with Python 3.9+, `cc`, and PlatformIO installed:

```bash
python3 master-node/tests/test_csv_export.py
python3 master-node/tests/test_paired_csv.py
pio run -d master-node -e MasterStable -t buildprog
```

Expect zero test exits and build `SUCCESS`. Host checks compile selected actual exporter functions; they do not validate Wi-Fi, request parsing, every metadata entry, or browser interactions. On a board check controls, reconnect/lease behavior, graphs, rename conflicts, and downloads after stopping. Use [Testing](../../../docs/TESTING.md).

Next: [Log format](../../../docs/LOG_FORMAT.md), [Architecture](../../docs/MASTER_ARCHITECTURE.md), or [Troubleshooting](../../../docs/TROUBLESHOOTING.md).
