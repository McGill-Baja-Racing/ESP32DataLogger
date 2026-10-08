# Recording format and exports

This is the storage/export contract reference. Read the [system overview](SYSTEM_OVERVIEW.md) and [protocol](PROTOCOL.md) first. Return to the [documentation index](README.md).

## Session files

The master creates a `.bin` recording and its `.bin.utc` companion under `/sdcard`. Default names use GPS UTC (`log_YYYY-MM-DD_HH-MM-SS.bin`) when available within the naming wait; otherwise the first free numbered name (`log_0001.bin`) is selected. Repeated timestamp names receive a numeric suffix. Custom names must be 5–44 characters including `.bin`, with only letters, digits, underscores, and hyphens before the suffix. Existing binary or companion names are rejected.

Renaming through the browser applies to completed sessions and their companions. Active recordings cannot be downloaded or renamed. Keep both files together when copying a card; a binary web download contains only the `.bin` file.

## Binary records

Records are 16 bytes, with no header:

| Bytes | Contents |
|---|---|
| 0–7 | Little-endian 64-bit CAN/log identifier |
| 8–11 | Signed 32-bit measurement |
| 12–15 | Unsigned 32-bit elapsed master timestamp in ms |

The writer stores two `int64_t` values: ID and packed payload. Interpret the payload as bits before splitting it. IDs, units, and scaling are in [Protocol](PROTOCOL.md). Records remain in queue/arrival order, not globally sorted sample-time order.

Each companion entry is one little-endian signed 64-bit Unix UTC timestamp in milliseconds, aligned by record index. Zero means UTC was unavailable when the sample was enqueued. Missing/truncated companions yield blank absolute-time cells for unavailable entries. Earlier samples are not retroactively given UTC after synchronization. See [absolute time](../master-node/docs/ABSOLUTE_TIME.md) for the clock model.

UTC adds eight bytes per sample. If companion writing fails, later UTC entries are unavailable; partial binary writes also disable further companion writes to avoid unsafe alignment.

## Full CSV through the browser

Download a completed session with `format=csv`, for example `/api/logs/download?name=log_0001.bin&format=csv`.

```text
sample_index,can_id,can_id_hex,signal,node,timestamp_ms,absolute_time_utc,value,units,raw_data
```

UTC uses an ISO calendar timestamp ending in `Z`. GPS speed is converted to km/h; coordinates remain integer degrees × 10⁷ in the full CSV. Other values preserve their integer signal scale. `raw_data` is the unsigned packed payload.

## Powertrain CSV

Use the browser's Powertrain CSV option or `format=paired` at the same endpoint. Its columns are:

```text
Relative time,Absolute time,Brake pressure,Bearing RPM,Engine RPM,GPS latitude,GPS longitude,GPS Speed
```

Each row is driven by an engine RPM sample. Relative time is its master timestamp in milliseconds; absolute time is its aligned UTC entry. Bearing RPM must be at or before that timestamp and at most 100 ms old, otherwise the row is omitted. Zero and signed bearing RPM are retained without gear scaling. This is the raw measurement from a bearing inside the gearbox; wheel and secondary RPM must be calculated later using the relevant drivetrain ratios.

Front brake pressure is included in psi if at or before the engine sample and at most 100 ms old; otherwise the cell is blank. Rear brake remains in full CSV. GPS fields use the most recently encountered corresponding reading at or before the engine timestamp, without an age cutoff. Coordinates become decimal degrees and speed becomes km/h; unavailable fields are blank.

The exporter scans file order and holds one latest value per supporting signal. It does not sort records or search older history if that held value has a future timestamp. GPS-only recordings have no engine/bearing pairs and therefore no Powertrain data rows.

## Offline decoding

With Python 3.9+, from the repository root, using a `.bin` file copied from the card:

```bash
python3 master-node/tools/decode_log.py /path/to/log_0001.bin --output /path/to/log_0001.csv
```

Replace the example paths with your files. Expect a saved CSV and sample/ID counts. Without `--output`, output uses the input name with a `.csv` suffix; choose a distinct path to preserve an existing CSV. The decoder ignores incomplete trailing records and reports them.

The offline tool produces the full-CSV fields **without** `absolute_time_utc`: it does not read companions or implement Powertrain pairing. Use the browser exporter for those capabilities.

Next: [Logger implementation](../master-node/src/logger/README.md), [Web implementation](../master-node/src/web/README.md), and [Testing](TESTING.md).
