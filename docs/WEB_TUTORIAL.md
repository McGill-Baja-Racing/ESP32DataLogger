# Worked tutorial: clarify a browser control

Change the existing **Display rate** label to **Graph refresh rate** so users can distinguish display updates from sensor acquisition. This is a small practice change, not a change already applied to the firmware. Start with [Getting started](GETTING_STARTED.md), work on a [task branch](../CONTRIBUTING.md#work-on-a-branch), and build the master successfully before editing.

## 1. Find the implementation and its boundaries

| File | Why you read or change it |
|---|---|
| [`master-node/src/web/index.html`](../master-node/src/web/index.html) | Embedded HTML, styles, and JavaScript; owns the visible label and polling caller |
| [`master-node/src/web/index_html.S`](../master-node/src/web/index_html.S) | Embeds that HTML into firmware; inspect, no change needed for this label |
| [`master-node/tools/web_asset_dependencies.py`](../master-node/tools/web_asset_dependencies.py) | Tracks the embedded asset for rebuilds; inspect, no change needed |
| [`master-node/platformio.ini`](../master-node/platformio.ini) | Selects the master environment and pre-build dependency script |
| [`master-node/src/web/web_server.c`](../master-node/src/web/web_server.c) | Serves the page and HTTP API; inspect when behavior changes |

Search the HTML for `Display rate` and then for `$('rate')`. The select chooses how often the browser polls latest values. It does not configure the sensor descriptor period. Follow [the web source guide](../master-node/src/web/README.md) if these boundaries are unfamiliar.

## 2. Make the one-label edit

In `index.html`, replace only the text `Display rate` with `Graph refresh rate`. Preserve `id="rate"`, the option values, and JavaScript. The element ID connects the select to the caller; changing it without the corresponding JavaScript would break the control.

Review `git diff` and run `git diff --check` from the repository root. Expect one text change and no whitespace errors. There is no frontend package installation or separate website deployment: the page is part of the board firmware.

## 3. Build and upload the firmware

From the repository root:

```bash
pio run -d master-node -e MasterStable -t buildprog
```

Expect `SUCCESS`. With the intended master board connected, use [Flashing](FLASHING.md) to identify its port, then replace `PORT`:

```bash
pio run -d master-node -e MasterStable -t upload --upload-port PORT
pio device monitor --port PORT -b 115200
```

The master needs its normal SD and Wi-Fi setup. It attempts automatic recording after boot; protect any ongoing test session before uploading. A successful build proves asset integration, not that the browser interaction works.

## 4. Check on the device

Connect to `BajaDAQ`, open `http://192.168.4.1`, and reload the page after flashing. Confirm the new label is visible. If old text remains, verify the board/profile uploaded and reload without the cached page.

On a controlled bench, start a recording if idle, enable live viewing, and select a signal with a known changing input. Switch graph refresh rates and confirm updates continue without browser console errors. Stop recording and wait for idle. Check that live viewing stops, a completed log can be downloaded, and its source timestamps remain independent of the selected graph rate. Do not infer acquisition rate from the graph animation alone.

Record the build result, board/profile, browser, observed label and controls, and checks you could not perform in your PR. Without a board, report the label edit and build evidence and explicitly leave device checks pending.

## If the change also needs an API change

A label does not require one. For an agreed behavior change, trace the existing JavaScript `fetch` caller into its registered handler in `web_server.c` before designing fields. Write down request method, URL, fields, response, and error behavior together; update both ends consistently. Recording and live lifecycle policy belongs in `app/app_control.*`, not directly in HTML or a handler manipulating logger state. Latest-signal data belongs in `live/live_data.*`.

Choose tests by behavior using [Testing](TESTING.md): export changes need CSV host checks; lifecycle or browser changes need device interaction checks. Preserve asynchronous stop, live lease ownership, error messages, and completed-file restrictions. See [master architecture](../master-node/docs/MASTER_ARCHITECTURE.md) and [log format](LOG_FORMAT.md) before changing those contracts.
