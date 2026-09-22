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
    E,W,UNUSED,SPARK,GPS=187,185,189,188,0x700
    assert export([(W,0,0),(E,0,0)])==[[0,0,0]]
    assert export([(W,1000,900),(SPARK,1001,1),(E,1001,3000),(E,1100,3100),(E,1101,3200)])==[[1001,3000,900],[1100,3100,900]]
    assert export([(E,1000,3000),(W,1000,900)])==[]  # no future record lookup
    assert export([(W,1001,900),(E,1000,3000)])==[]  # future timestamp
    assert export([(W,1000,900),(W,1020,-500),(E,1021,0)])==[[1021,0,-500]]
    assert export([(W,1000,900),(UNUSED,1001,9999),(E,1002,3000)])==[[1002,3000,900]]
    assert export([(UNUSED,1000,900),(E,1000,3000)])==[]
    assert export([(W,0xfffffffe,900),(E,4,3000),(E,99,3000)])==[[4,3000,900]]
    assert export([(W,1000,900),(E,10,3000)])==[]  # clock reset cannot reuse wheel
    assert export([])==[]
    assert export([(W,1000,1000)])==[]
    assert export([(GPS,100,1234),(W,100,900),(E,100,3000),(GPS,120,0),(E,120,3100)])==[[100,3000,900],[120,3100,900]]
    assert export.speeds==['12.34','0.00']
    export([(GPS,101,1234),(W,100,900),(E,100,3000)])
    assert export.speeds==['']
    export([(GPS,0xfffffffe,1),(W,0xfffffffe,900),(E,4,3000)])
    assert export.speeds==['0.01']
print('Paired CSV tests passed: independent sensors, 100 ms cutoff, missing/future data, zero, sign, rollover')
