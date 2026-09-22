"""Compile and exercise the firmware's powertrain CSV exporter on the host."""
from pathlib import Path
import csv
import io
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/web/web_server.c").read_text()
start = source.index("static int64_t read_utc_ms(")
end = source.index("static esp_err_t stream_csv(", start)
harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include "protocol/app_protocol.h"
typedef int esp_err_t;
typedef int httpd_req_t;
#define ESP_OK 0
#define ESP_FAIL 1
static int httpd_resp_send_chunk(httpd_req_t *request,const char *data,size_t size) {
    (void)request;return size&&fwrite(data,1,size,stdout)!=size?ESP_FAIL:ESP_OK;
}
''' + source[start:end] + r'''
int main(int argc,char **argv) {
    if(argc<2||argc>3)return 2;
    FILE *file=fopen(argv[1],"rb");if(!file)return 1;
    FILE *utc_file=argc==3?fopen(argv[2],"rb"):NULL;
    if(argc==3&&!utc_file){fclose(file);return 1;}
    httpd_req_t request=0;int result=stream_paired_csv(&request,file,utc_file);
    if(utc_file)fclose(utc_file);fclose(file);return result;
}
'''

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(harness)
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "src"),
            str(temp / "test.c"),
            "-o",
            str(temp / "test"),
        ],
        check=True,
    )

    def export(records, utc_values=None):
        binary = temp / "input.bin"
        binary.write_bytes(
            b"".join(
                struct.pack("<qQ", can_id, (timestamp << 32) | (value & 0xFFFFFFFF))
                for can_id, timestamp, value in records
            )
        )
        command = [str(temp / "test"), str(binary)]
        if utc_values is not None:
            utc = temp / "input.bin.utc"
            utc.write_bytes(b"".join(struct.pack("<q", value) for value in utc_values))
            command.append(str(utc))
        text = subprocess.check_output(command, text=True)
        assert text.splitlines()[0] == (
            "Relative time,Absolute time,Brake pressure,Bearing RPM,Engine RPM,"
            "GPS latitude,GPS longitude,GPS Speed"
        )
        rows = list(csv.DictReader(io.StringIO(text)))
        assert all(len(row) == 8 for row in rows)
        return rows

    ENGINE = 0x0BB
    BEARING = 0x0B9
    FRONT_BRAKE = 0x0B1
    REAR_BRAKE = 0x0B2
    UNUSED = 0x0BD
    SPARK = 0x0BC
    GPS_SPEED = 0x700
    GPS_LATITUDE = 0x701
    GPS_LONGITUDE = 0x702

    records = [
        (FRONT_BRAKE, 995, 250),
        (REAR_BRAKE, 995, 125),
        (GPS_SPEED, 996, 1234),
        (GPS_LATITUDE, 996, 455235947),
        (GPS_LONGITUDE, 996, -734223998),
        (BEARING, 1000, 900),
        (SPARK, 1001, 1),
        (ENGINE, 1001, 3000),
    ]
    utc_values = [0, 0, 0, 0, 0, 0, 0, 1_700_000_000_123]
    rows = export(records, utc_values)
    assert rows == [
        {
            "Relative time": "1001",
            "Absolute time": "2023-11-14T22:13:20.123Z",
            "Brake pressure": "250",
            "Bearing RPM": "900",
            "Engine RPM": "3000",
            "GPS latitude": "45.5235947",
            "GPS longitude": "-73.4223998",
            "GPS Speed": "12.34",
        }
    ]

    # The singular brake column is the front pressure channel. Rear pressure
    # remains available in the full CSV and never replaces it here.
    rows = export([(REAR_BRAKE, 1000, 999), (BEARING, 1000, 900), (ENGINE, 1000, 3000)])
    assert rows[0]["Brake pressure"] == ""

    # Brake and bearing values cannot come from the future. Brake is blank once
    # stale, while a missing/stale bearing suppresses the whole powertrain row.
    rows = export([(FRONT_BRAKE, 1000, 250), (BEARING, 1000, 900), (ENGINE, 1101, 3000)])
    assert rows == []
    rows = export([(BEARING, 1100, 900), (FRONT_BRAKE, 1100, 250), (ENGINE, 1000, 3000)])
    assert rows == []
    rows = export([(FRONT_BRAKE, 999, 250), (BEARING, 1100, 900), (ENGINE, 1100, 3000)])
    assert rows[0]["Brake pressure"] == ""

    # GPS is held between fixes without an age cutoff, including valid zeros.
    rows = export(
        [
            (GPS_SPEED, 100, 0),
            (GPS_LATITUDE, 100, 0),
            (GPS_LONGITUDE, 100, 0),
            (BEARING, 1000, -500),
            (ENGINE, 1000, 0),
        ]
    )
    assert rows[0]["GPS Speed"] == "0.00"
    assert rows[0]["GPS latitude"] == "0.0000000"
    assert rows[0]["GPS longitude"] == "0.0000000"
    assert rows[0]["Bearing RPM"] == "-500"
    assert rows[0]["Engine RPM"] == "0"

    # A GPS record timestamped after the engine sample is not eligible.
    rows = export(
        [
            (GPS_SPEED, 1001, 1234),
            (GPS_LATITUDE, 1001, 455235947),
            (GPS_LONGITUDE, 1001, -734223998),
            (BEARING, 1000, 900),
            (ENGINE, 1000, 3000),
        ]
    )
    assert all(rows[0][key] == "" for key in ("GPS Speed", "GPS latitude", "GPS longitude"))

    # Unsynchronized/missing UTC entries stay blank; record alignment and
    # 32-bit millisecond rollover are preserved.
    rows = export(
        [(UNUSED, 0xFFFFFFFD, 1), (BEARING, 0xFFFFFFFE, 900), (ENGINE, 4, 3000)],
        [1_700_000_000_000, 1_700_000_000_010, 0],
    )
    assert rows[0]["Relative time"] == "4"
    assert rows[0]["Absolute time"] == ""

    assert export([]) == []
    assert export([(BEARING, 1000, 900)]) == []

print(
    "Powertrain CSV tests passed: schema, UTC alignment, brake, RPM, GPS, "
    "missing data, and rollover"
)
