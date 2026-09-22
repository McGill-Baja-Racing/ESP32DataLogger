"""Compile and exercise the firmware's actual CSV exporter on the host."""
from pathlib import Path
import csv
import io
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/web/web_server.c').read_text()
start=source.index('static esp_err_t stream_csv(')
end=source.index('static esp_err_t download_handler',start)
harness=r'''
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "protocol/app_protocol.h"
typedef int esp_err_t;
typedef int httpd_req_t;
#define ESP_OK 0
#define ESP_FAIL 1
typedef struct { const char *signal, *node, *units; } live_signal_metadata_t;
static const live_signal_metadata_t *metadata_for(uint32_t id) {
    static const live_signal_metadata_t engine={"engine_rpm","engine_node_5","rpm"};
    static const live_signal_metadata_t idler={"bearing_rpm","encoder_node_4","rpm"};
    return id==187?&engine:id==185?&idler:NULL;
}
static int httpd_resp_send_chunk(httpd_req_t *request,const char *data,size_t size) {
    (void)request;return size&&fwrite(data,1,size,stdout)!=size?ESP_FAIL:ESP_OK;
}
'''+source[start:end]+r'''
int main(void) {
    const uint32_t ids[]={187,185,188,190,189,187,185,187,0x700,0x700,0x700};
    const int32_t values[]={3000,-1000,1,4,1000,0,20000,3100,1234,0,1};
    FILE *file=tmpfile();if(!file)return 1;
    for(unsigned i=0;i<11;i++){
        uint64_t record[]={ids[i],((uint64_t)(1000+i*20)<<32)|(uint32_t)values[i]};
        fwrite(record,sizeof(record),1,file);
    }
    rewind(file);httpd_req_t request=0;
    int result=stream_csv(&request,file,NULL);fclose(file);return result;
}
'''
with tempfile.TemporaryDirectory() as directory:
    temp=Path(directory);(temp/'test.c').write_text(harness)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(temp/'test.c'),'-o',str(temp/'test')],check=True)
    full=subprocess.check_output([str(temp/'test')],text=True)
    assert full.splitlines()[0]=='sample_index,can_id,can_id_hex,signal,node,timestamp_ms,absolute_time_utc,value,units,raw_data'
    full_rows=list(csv.DictReader(io.StringIO(full)))
    assert len(full_rows)==11
    assert [r['value'] for r in full_rows[-3:]]==['12.34','0.00','0.01']
    assert all(r['units']=='km/h' for r in full_rows[-3:])
    import runpy
    decoder=runpy.run_path(str(ROOT/'tools/decode_log.py'))
    import struct
    binary=temp/'speed.bin'
    binary.write_bytes(b''.join(struct.pack('<QQ',int(r['can_id']),int(r['raw_data'])) for r in full_rows))
    decoder['decode_log'](binary,temp/'speed.csv')
    decoded=list(csv.DictReader((temp/'speed.csv').open()))
    assert all(r['units']=='km/h' for r in decoded[-3:])
    for firmware,python in zip(full_rows,decoded):
        for key in ('value','raw_data','timestamp_ms'):
            assert firmware[key]==python[key], (key,firmware,python)
print('Full CSV export passed: GPS units, precision, and decoder parity')
