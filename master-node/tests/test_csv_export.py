"""Compile and exercise the firmware's actual CSV exporter on the host."""
from pathlib import Path
import csv
import io
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/web/web_server.c').read_text()
helper_start=source.index('static int64_t read_utc_ms(')
helper_end=source.index('static bool sample_at_or_before(',helper_start)
start=source.index('static esp_err_t stream_csv(')
end=source.index('static esp_err_t download_handler',start)
health=(ROOT/'src/health/node_health.c').read_text()
health_start=health.index('static const char *const code_names')
health_end=health.index('static void print_frame(',health_start)
slot_define=next(line for line in health.splitlines() if line.startswith('#define NODE_SLOT_COUNT'))
# Statuses (one with every fault and out-of-range health and mode), an event for every byte value, then GPS.
records=[(0x0BB,3000),(0x0B9,-1000),(0x0BC,1),(0x0BE,4),(0x0BD,1000),(0x0BB,0),(0x0B9,20000),(0x0BB,3100),
         (0x0D3,0x8800|2<<16|1<<24),(0x0D0,0),(0x0D6,0xFFFF|7<<16|2<<24)]
records+=[(0x0E5,code|((0xB0+code)&0xFF)<<8|code<<16|(code%16)<<24) for code in range(256)]
records+=[(0x700,1234),(0x700,0),(0x700,1)]
harness=r'''
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "protocol/app_protocol.h"
'''+slot_define+r'''
#define NODE_HEALTH_TEXT_MAX 320
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
'''+health[health_start:health_end]+source[helper_start:helper_end]+source[start:end]+r'''
int main(void) {
    const uint32_t ids[]={'''+','.join(str(i) for i,_ in records)+r'''};
    const int32_t values[]={'''+','.join(str(v) for _,v in records)+r'''};
    FILE *file=tmpfile();if(!file)return 1;
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++){
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
    assert full.splitlines()[0]=='sample_index,can_id,can_id_hex,signal,node,timestamp_ms,absolute_time_utc,value,units,raw_data,detail'
    full_rows=list(csv.DictReader(io.StringIO(full)))
    assert len(full_rows)==len(records)
    status,master=full_rows[8],full_rows[9]
    assert (status['signal'],status['node'],status['detail'])==('node_status','node_3','error recording can_passive gps_no_fix')
    assert (master['node'],master['detail'])==('master','ok idle')
    assert full_rows[11+7]['detail']=='can_tx subject 0xB7 count 7 ESP_ERR_INVALID_RESPONSE', full_rows[18]
    assert full_rows[11+201]['detail']=='unknown subject 0x79 count 201 unknown'
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
        # The metadata stub knows only two sensors, so names are compared on health rows.
        keys=('signal','node','units') if firmware['detail'] else ()
        for key in ('value','raw_data','timestamp_ms','detail')+keys:
            assert firmware[key]==python[key], (key,firmware,python)
print('Full CSV export passed: GPS units, precision, and decoder parity')
