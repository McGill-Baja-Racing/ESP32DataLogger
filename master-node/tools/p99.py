#!/usr/bin/env python3
"""Print the 99th-percentile error of one signal in a master SD log, ignoring zero values.

Usage:
    python3 master-node/tools/p99.py runs/A/t4_5_2000/t4_5_2000.bin 0x0BB 2000
"""

import struct
import sys


def main() -> int:
    path, can_id, expected = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
    data = open(path, "rb").read()
    errors = sorted(
        abs(value - expected)
        for ident, value, _ in struct.iter_unpack("<qiI", data[: len(data) // 16 * 16])
        if ident & 0x7FF == can_id and value
    )
    print(errors[int(0.99 * len(errors))] if errors else "no samples")
    return 0


if __name__ == "__main__":
    sys.exit(main())
