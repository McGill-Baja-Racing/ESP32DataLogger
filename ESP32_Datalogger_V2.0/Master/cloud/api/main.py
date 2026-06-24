from __future__ import annotations

import csv
import json
import os
import re
import secrets
import shutil
import struct
import time
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import FileResponse, PlainTextResponse
from pydantic import BaseModel, Field


API_VERSION = "0.2.2"
DATA_DIR = Path(os.environ.get("BAJA_DATA_DIR", "/data"))
UPLOAD_DIR = DATA_DIR / "uploads"
TOKEN = os.environ.get("BAJA_API_TOKEN", "baja_logger_test_v1")
RETENTION_DAYS = int(os.environ.get("BAJA_UPLOAD_RETENTION_DAYS", "14"))
MAX_UPLOADS = int(os.environ.get("BAJA_MAX_UPLOADS", "200"))
MAX_FILENAME_LEN = 96
LOG_RECORD_SIZE = 16
DECODE_READ_BYTES = 64 * 1024

app = FastAPI(title="Baja Logger Cloud API", version=API_VERSION)


class UploadStart(BaseModel):
    filename: str = Field(min_length=1, max_length=MAX_FILENAME_LEN)
    size: int = Field(ge=0)
    expected_chunks: int | None = Field(default=None, ge=0)
    device_id: str = Field(default="master", max_length=64)


class UploadFinish(BaseModel):
    expected_chunks: int | None = Field(default=None, ge=0)
    expected_size: int | None = Field(default=None, ge=0)


def require_token(
    x_device_token: str | None = Header(default=None),
    token: str | None = Query(default=None),
) -> None:
    supplied = x_device_token or token
    if not supplied or not secrets.compare_digest(supplied, TOKEN):
        raise HTTPException(status_code=401, detail="invalid token")


def safe_filename(name: str) -> str:
    base = Path(name).name
    cleaned = re.sub(r"[^A-Za-z0-9_.-]", "_", base)
    if not cleaned or cleaned in {".", ".."}:
        raise HTTPException(status_code=400, detail="invalid filename")
    return cleaned[:MAX_FILENAME_LEN]


def upload_path(file_id: str) -> Path:
    if not re.fullmatch(r"[A-Za-z0-9_-]{12,40}", file_id):
        raise HTTPException(status_code=404, detail="unknown file id")
    return UPLOAD_DIR / file_id


def metadata_path(file_id: str) -> Path:
    return upload_path(file_id) / "metadata.json"


def load_metadata(file_id: str) -> dict[str, Any]:
    path = metadata_path(file_id)
    if not path.exists():
        raise HTTPException(status_code=404, detail="unknown file id")
    return json.loads(path.read_text())


def save_metadata(file_id: str, metadata: dict[str, Any]) -> None:
    metadata_path(file_id).write_text(json.dumps(metadata, indent=2, sort_keys=True))


def dir_size_bytes(path: Path) -> int:
    if not path.exists():
        return 0
    total = 0
    for item in path.rglob("*"):
        if item.is_file():
            try:
                total += item.stat().st_size
            except OSError:
                continue
    return total


def upload_metadata_items() -> list[dict[str, Any]]:
    files = []
    for meta_file in sorted(UPLOAD_DIR.glob("*/metadata.json")):
        try:
            metadata = json.loads(meta_file.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        metadata["storage_bytes"] = dir_size_bytes(meta_file.parent)
        files.append(metadata)
    files.sort(key=lambda item: item.get("started_at", 0), reverse=True)
    return files


def storage_info() -> dict[str, Any]:
    usage = shutil.disk_usage(DATA_DIR)
    used_by_uploads = dir_size_bytes(UPLOAD_DIR)
    return {
        "upload_retention_days": RETENTION_DAYS,
        "max_uploads": MAX_UPLOADS,
        "uploads_bytes": used_by_uploads,
        "disk_total_bytes": usage.total,
        "disk_used_bytes": usage.used,
        "disk_free_bytes": usage.free,
        "disk_used_percent": round((usage.used / usage.total) * 100, 2) if usage.total else 0,
        "uploads_percent_of_disk": round((used_by_uploads / usage.total) * 100, 2) if usage.total else 0,
    }


def cleanup_uploads() -> dict[str, Any]:
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    now = time.time()
    cutoff = now - (RETENTION_DAYS * 24 * 60 * 60)
    uploads = upload_metadata_items()
    to_delete: set[str] = set()

    for item in uploads:
        started_at = float(item.get("started_at", 0) or 0)
        finished_at = float(item.get("finished_at", 0) or 0)
        reference_time = finished_at or started_at
        if reference_time and reference_time < cutoff:
            to_delete.add(str(item["file_id"]))

    for item in uploads[MAX_UPLOADS:]:
        to_delete.add(str(item["file_id"]))

    deleted = []
    freed_bytes = 0
    for file_id in sorted(to_delete):
        root = upload_path(file_id)
        if not root.exists():
            continue
        size = dir_size_bytes(root)
        shutil.rmtree(root, ignore_errors=True)
        deleted.append(file_id)
        freed_bytes += size

    return {
        "deleted": deleted,
        "deleted_count": len(deleted),
        "freed_bytes": freed_bytes,
        "retention_days": RETENTION_DAYS,
        "max_uploads": MAX_UPLOADS,
    }


def finish_binary_upload(file_id: str, bin_path: Path, metadata: dict[str, Any]) -> dict[str, Any]:
    actual_size = bin_path.stat().st_size
    expected_size = metadata.get("expected_size")
    if expected_size is not None and actual_size != int(expected_size):
        metadata.update(
            {
                "finished_at": time.time(),
                "status": "size_mismatch",
                "bytes_received": actual_size,
            }
        )
        save_metadata(file_id, metadata)
        raise HTTPException(
            status_code=409,
            detail=f"size mismatch: got {actual_size}, expected {expected_size}",
        )

    csv_path = bin_path.with_suffix(".csv")
    debug = decode_log_to_csv(bin_path, csv_path)

    metadata.update(
        {
            "finished_at": time.time(),
            "status": "complete",
            "bytes_received": actual_size,
            "bin_path": bin_path.name,
            "csv_path": csv_path.name,
            "debug": debug,
        }
    )
    save_metadata(file_id, metadata)
    return {"ok": True, "file_id": file_id, "size": actual_size, "debug": debug}


def decode_log_to_csv(bin_path: Path, csv_path: Path) -> dict[str, Any]:
    file_size = bin_path.stat().st_size
    full_records = file_size // LOG_RECORD_SIZE
    trailing_bytes = file_size % LOG_RECORD_SIZE
    timestamp_decreases = 0
    gaps_over_1000_ms = 0
    previous_timestamp: int | None = None

    read_bytes = DECODE_READ_BYTES - (DECODE_READ_BYTES % LOG_RECORD_SIZE)
    with bin_path.open("rb") as source, csv_path.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["sample_index", "can_id", "can_id_hex", "timestamp_ms", "value", "raw_data"])

        index = 0
        while index < full_records:
            block = source.read(read_bytes)
            if not block:
                break

            record_count = len(block) // LOG_RECORD_SIZE
            for offset in range(0, record_count * LOG_RECORD_SIZE, LOG_RECORD_SIZE):
                can_id, packed = struct.unpack_from("<qq", block, offset)
                raw_data = packed & 0xFFFFFFFFFFFFFFFF
                timestamp_ms = (raw_data >> 32) & 0xFFFFFFFF
                value = raw_data & 0xFFFFFFFF

                if previous_timestamp is not None:
                    if timestamp_ms < previous_timestamp:
                        timestamp_decreases += 1
                    if timestamp_ms - previous_timestamp > 1000:
                        gaps_over_1000_ms += 1
                previous_timestamp = timestamp_ms

                writer.writerow([index, can_id, f"0x{can_id:03X}", timestamp_ms, value, raw_data])
                index += 1

    return {
        "records": full_records,
        "trailing_bytes": trailing_bytes,
        "timestamp_decreases": timestamp_decreases,
        "gaps_over_1000_ms": gaps_over_1000_ms,
    }


@app.on_event("startup")
def startup() -> None:
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    cleanup_uploads()


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "ok": True,
        "service": "baja-cloud-api",
        "version": API_VERSION,
        "features": {
            "raw_upload": True,
            "chunk_upload_legacy": True,
            "csv_conversion": True,
            "retention_cleanup": True,
            "delete_upload": True,
        },
        "storage": storage_info(),
        "time": time.time(),
    }


@app.post("/upload/start")
def upload_start(
    body: UploadStart,
    token: str | None = Query(default=None),
    x_device_token: str | None = Header(default=None),
) -> dict[str, Any]:
    require_token(x_device_token=x_device_token, token=token)
    file_id = secrets.token_urlsafe(12)
    root = upload_path(file_id)
    chunks = root / "chunks"
    chunks.mkdir(parents=True, exist_ok=False)

    filename = safe_filename(body.filename)
    metadata = {
        "file_id": file_id,
        "filename": filename,
        "device_id": body.device_id,
        "expected_size": body.size,
        "expected_chunks": body.expected_chunks,
        "started_at": time.time(),
        "finished_at": None,
        "status": "uploading",
        "bytes_received": 0,
        "chunks_received": 0,
        "debug": {},
    }
    save_metadata(file_id, metadata)
    return {"ok": True, "file_id": file_id}


@app.post("/upload/raw")
async def upload_raw(
    request: Request,
    token: str | None = Query(default=None),
    x_device_token: str | None = Header(default=None),
    x_file_name: str | None = Header(default=None),
    x_file_size: int | None = Header(default=None),
    x_request_id: str | None = Header(default=None),
) -> dict[str, Any]:
    require_token(x_device_token=x_device_token, token=token)
    if not x_file_name:
        raise HTTPException(status_code=400, detail="missing X-File-Name header")

    file_id = secrets.token_urlsafe(12)
    root = upload_path(file_id)
    root.mkdir(parents=True, exist_ok=False)

    filename = safe_filename(x_file_name)
    tmp_path = root / f"{filename}.part"
    bin_path = root / filename
    bytes_received = 0

    metadata = {
        "file_id": file_id,
        "filename": filename,
        "device_id": "master",
        "request_id": x_request_id,
        "expected_size": x_file_size,
        "expected_chunks": None,
        "started_at": time.time(),
        "finished_at": None,
        "status": "uploading_raw",
        "bytes_received": 0,
        "chunks_received": None,
        "debug": {},
    }
    save_metadata(file_id, metadata)

    try:
        with tmp_path.open("wb") as out:
            async for chunk in request.stream():
                if chunk:
                    out.write(chunk)
                    bytes_received += len(chunk)
    except Exception:
        metadata["status"] = "upload_failed"
        metadata["bytes_received"] = bytes_received
        save_metadata(file_id, metadata)
        raise

    tmp_path.replace(bin_path)
    metadata["bytes_received"] = bytes_received
    result = finish_binary_upload(file_id, bin_path, metadata)
    cleanup_uploads()
    return {
        **result,
        "csv_url": f"/files/{file_id}/csv",
        "bin_url": f"/files/{file_id}/bin",
    }


@app.put("/upload/{file_id}/chunk/{seq}")
async def upload_chunk(
    file_id: str,
    seq: int,
    request: Request,
    token: str | None = Query(default=None),
    x_device_token: str | None = Header(default=None),
) -> dict[str, Any]:
    require_token(x_device_token=x_device_token, token=token)
    if seq < 0:
        raise HTTPException(status_code=400, detail="negative chunk sequence")

    metadata = load_metadata(file_id)
    body = await request.body()
    chunk_path = upload_path(file_id) / "chunks" / f"{seq:08d}.chunk"
    existed = chunk_path.exists()
    chunk_path.write_bytes(body)

    if not existed:
        metadata["chunks_received"] = int(metadata.get("chunks_received", 0)) + 1
        metadata["bytes_received"] = int(metadata.get("bytes_received", 0)) + len(body)
    save_metadata(file_id, metadata)
    return {"ok": True, "file_id": file_id, "seq": seq, "bytes": len(body)}


@app.post("/upload/{file_id}/finish")
def upload_finish(
    file_id: str,
    body: UploadFinish,
    token: str | None = Query(default=None),
    x_device_token: str | None = Header(default=None),
) -> dict[str, Any]:
    require_token(x_device_token=x_device_token, token=token)
    metadata = load_metadata(file_id)
    root = upload_path(file_id)
    chunk_dir = root / "chunks"
    chunk_paths = sorted(chunk_dir.glob("*.chunk"))

    expected_chunks = body.expected_chunks or metadata.get("expected_chunks")
    if expected_chunks is not None and len(chunk_paths) != int(expected_chunks):
        raise HTTPException(
            status_code=409,
            detail=f"missing chunks: got {len(chunk_paths)}, expected {expected_chunks}",
        )

    bin_path = root / metadata["filename"]
    with bin_path.open("wb") as out:
        for chunk_path in chunk_paths:
            with chunk_path.open("rb") as src:
                shutil.copyfileobj(src, out)

    metadata["expected_size"] = body.expected_size or metadata.get("expected_size")
    metadata["chunks_received"] = len(chunk_paths)
    result = finish_binary_upload(file_id, bin_path, metadata)
    cleanup_uploads()
    return result


@app.get("/files")
def list_files() -> dict[str, Any]:
    cleanup = cleanup_uploads()
    return {
        "ok": True,
        "files": upload_metadata_items(),
        "storage": storage_info(),
        "cleanup": cleanup,
    }


def delete_upload_by_id(file_id: str) -> dict[str, Any]:
    root = upload_path(file_id)
    if not root.exists():
        raise HTTPException(status_code=404, detail="unknown file id")
    freed_bytes = dir_size_bytes(root)
    shutil.rmtree(root)
    return {"ok": True, "file_id": file_id, "freed_bytes": freed_bytes, "storage": storage_info()}


@app.delete("/files/{file_id}")
def delete_upload(
    file_id: str,
    token: str | None = Query(default=None),
    x_device_token: str | None = Header(default=None),
) -> dict[str, Any]:
    require_token(x_device_token=x_device_token, token=token)
    return delete_upload_by_id(file_id)


@app.post("/files/{file_id}/delete")
def delete_upload_post(
    file_id: str,
    token: str | None = Query(default=None),
    x_device_token: str | None = Header(default=None),
) -> dict[str, Any]:
    require_token(x_device_token=x_device_token, token=token)
    return delete_upload_by_id(file_id)


@app.post("/delete-upload")
def delete_upload_query(
    file_id: str = Query(...),
    token: str | None = Query(default=None),
    x_device_token: str | None = Header(default=None),
) -> dict[str, Any]:
    require_token(x_device_token=x_device_token, token=token)
    return delete_upload_by_id(file_id)


@app.get("/files/{file_id}/csv")
def download_csv(file_id: str) -> FileResponse:
    metadata = load_metadata(file_id)
    if metadata.get("status") != "complete" or not metadata.get("csv_path"):
        raise HTTPException(status_code=404, detail="csv is not ready")
    path = upload_path(file_id) / metadata["csv_path"]
    return FileResponse(path, media_type="text/csv", filename=path.name)


@app.get("/files/{file_id}/bin")
def download_bin(file_id: str) -> FileResponse:
    metadata = load_metadata(file_id)
    if metadata.get("status") != "complete" or not metadata.get("bin_path"):
        raise HTTPException(status_code=404, detail="binary file is not ready")
    path = upload_path(file_id) / metadata["bin_path"]
    return FileResponse(path, media_type="application/octet-stream", filename=path.name)


@app.get("/", response_class=PlainTextResponse)
def root() -> str:
    return "Baja Logger Cloud API is running. Try /health or /files.\n"
