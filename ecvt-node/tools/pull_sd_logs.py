#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


DEFAULT_DEST_DIR = Path("./Data")
DEFAULT_PATTERNS = ("log_*.bin", "LOG_*.BIN")


def discover_mount_candidates() -> list[Path]:
    candidates: list[Path] = []
    home = Path.home()

    for root in (Path("/Volumes"), Path("/media") / home.name, Path("/run/media") / home.name):
        if not root.exists():
            continue
        for child in root.iterdir():
            if child.is_dir():
                candidates.append(child)

    return candidates


def find_log_files(volume: Path, patterns: tuple[str, ...]) -> list[Path]:
    files: list[Path] = []
    for pattern in patterns:
        files.extend(volume.glob(pattern))

    unique_files = sorted({path.resolve() for path in files})
    return [path for path in unique_files if path.is_file()]


def choose_volume(explicit_volume: str | None, patterns: tuple[str, ...]) -> Path:
    if explicit_volume:
        volume = Path(explicit_volume).expanduser().resolve()
        if not volume.exists():
            raise FileNotFoundError(f"Volume path does not exist: {volume}")
        return volume

    candidates = []
    for mount in discover_mount_candidates():
        if find_log_files(mount, patterns):
            candidates.append(mount)

    if not candidates:
        raise FileNotFoundError(
            "Could not find a mounted SD card volume containing log files. "
            "Pass --volume /path/to/mount explicitly."
        )

    if len(candidates) > 1:
        joined = "\n".join(f"  - {path}" for path in candidates)
        raise RuntimeError(
            "Multiple mounted volumes contain matching log files. "
            "Choose one with --volume:\n"
            f"{joined}"
        )

    return candidates[0]


def copy_files(files: list[Path], dest_dir: Path, overwrite: bool) -> int:
    dest_dir.mkdir(parents=True, exist_ok=True)
    copied = 0

    for src in files:
        dest = dest_dir / src.name
        if dest.exists() and not overwrite:
            src_stat = src.stat()
            dest_stat = dest.stat()
            if dest_stat.st_size == src_stat.st_size and dest_stat.st_mtime >= src_stat.st_mtime:
                print(f"Skipping up-to-date file: {dest}")
                continue

        shutil.copy2(src, dest)
        copied += 1
        print(f"Copied: {src} -> {dest}")

    return copied


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Copy ESP32-P4 SD card log files from a mounted SD card to this computer."
    )
    parser.add_argument(
        "--volume",
        help="Mounted SD card path, for example /Volumes/NO_NAME. If omitted, the script auto-detects it.",
    )
    parser.add_argument(
        "--dest",
        default=str(DEFAULT_DEST_DIR),
        help="Destination directory on this computer. Default: ./Data",
    )
    parser.add_argument(
        "--latest",
        action="store_true",
        help="Copy only the newest matching log file.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Always overwrite local files, even if they appear up to date.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    patterns = DEFAULT_PATTERNS

    try:
        volume = choose_volume(args.volume, patterns)
        files = find_log_files(volume, patterns)
    except (FileNotFoundError, RuntimeError) as exc:
        print(exc, file=sys.stderr)
        return 1

    if not files:
        print(f"No matching log files found on {volume}", file=sys.stderr)
        return 1

    if args.latest:
        files = [max(files, key=lambda path: path.stat().st_mtime)]

    print(f"Using SD card volume: {volume}")
    copied = copy_files(files, Path(args.dest).expanduser(), args.overwrite)

    if copied == 0:
        print("No files were copied.")
    else:
        print(f"Copied {copied} file(s).")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
