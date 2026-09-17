"""Exercise the actual firmware's paired exporter, including incomplete data."""
from pathlib import Path
import csv
import io
import struct
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/web/web_server.c').read_text()
start=source.index('static esp_err_t stream_paired_csv(')
end=source.index('static esp_err_t stream_csv(',start)
harness=r'''
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include "protocol/app_protocol.h"
typedef int esp_err_t;
typedef int httpd_req_t;
#define ESP_OK 0
#define ESP_FAIL 1
static int httpd_resp_send_chunk(httpd_req_t *request,const char *data,size_t size) {
    (void)request;return size&&fwrite(data,1,size,stdout)!=size?ESP_FAIL:ESP_OK;
}
'''+source[start:end]+r'''
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *file=fopen(argv[1],"rb");if(!file)return 1;
    httpd_req_t request=0;int result=stream_paired_csv(&request,file);
    fclose(file);return result;
}
'''
with tempfile.TemporaryDirectory() as directory:
    temp=Path(directory);(temp/'test.c').write_text(harness)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(temp/'test.c'),'-o',str(temp/'test')],check=True)
    def export(records):
        p=temp/'input.bin';p.write_bytes(b''.join(struct.pack('<qQ',cid,(ts<<32)|(value&0xffffffff)) for cid,ts,value in records))
        text=subprocess.check_output([str(temp/'test'),str(p)],text=True)
        assert text.splitlines()[0]=='Timestamp,Engine RPM,Wheel RPM,Car Speed (km/h)'
        rows=list(csv.DictReader(io.StringIO(text)))
        assert all(len(row)==4 for row in rows)
        export.speeds=[row['Car Speed (km/h)'] for row in rows]
        return [[int(row[k]) for k in ('Timestamp','Engine RPM','Wheel RPM')] for row in rows]
    E,W,RAW,SPARK,GPS=187,189,185,188,0x700
    records=[
        (E,0,0),(W,0,0),                     # timestamp/value zero is real data
        (E,1000,3000),(SPARK,1000,1),(GPS,999,4),(W,1000,1000),
        (E,1020,3100),(RAW,1020,900),         # raw wheel is not a recorded pair
        (E,1040,3200),(W,1060,1100),         # mismatched timestamps omitted
        (W,1080,1200),(E,1080,3300),         # same timestamp, reverse record order
        (E,1100,0),(W,1100,500),             # stopped engine, moving wheel
        (E,1120,3000),(W,1120,-500),         # reverse wheel stays signed
        (W,1120,-500),                      # unmatched duplicate does not emit
        (E,1140,3500),                      # orphan overwritten by new engine
        (E,1160,3600),(W,1160,1400),
        (E,1160,3650),(W,1160,1500),         # distinct complete pair, same timestamp
        (E,0xffffffff,3000),(W,0xffffffff,1000),
        (E,4,3010),(W,4,1010),              # millisecond rollover
        (E,24,3020),                         # incomplete final record pair
    ]
    assert export(records)==[[0,0,0],[1000,3000,1000],[1080,3300,1200],[1100,0,500],[1120,3000,-500],[1160,3600,1400],[1160,3650,1500],[0xffffffff,3000,1000],[4,3010,1010]]
    assert export.speeds[:7]==['','0.04','0.04','0.04','0.04','0.04','0.04']
    assert export([(GPS,100,1234),(E,100,3000),(W,100,1000),(GPS,120,0),(E,120,3000),(W,120,1000)])==[[100,3000,1000],[120,3000,1000]]
    assert export.speeds==['12.34','0.00']
    export([(GPS,101,1234),(E,100,3000),(W,100,1000)])
    assert export.speeds==['']
    export([(GPS,0xfffffffe,1),(E,4,3000),(W,4,1000)])
    assert export.speeds==['0.01']
    assert export([])==[]
    assert export([(RAW,1000,1000),(E,1000,3000)])==[]
    assert export([(W,1000,1000)])==[]
print('Paired CSV tests passed: complete pairs only, zeros, signs, interleaving, rollover, missing data')
