"""Compile and exercise the firmware's actual CSV exporter on the host."""
from pathlib import Path
import csv
import io
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/web/web_server.c').read_text()
live_source=(ROOT/'src/live/live_data.c').read_text()
metadata=live_source[live_source.index('static const live_signal_metadata_t signals['):live_source.index('static live_sample_t latest[')]
helper_start=source.index('static int64_t read_utc_ms(')
helper_end=source.index('static bool sample_at_or_before(',helper_start)
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
#include "live/live_data.h"
#include "aim/aim_adapter.h"
'''+metadata+r'''
static const live_signal_metadata_t *metadata_for(uint32_t id) {
    for (unsigned i=0; i<LIVE_DATA_SIGNAL_COUNT; i++) {
        if (signals[i].can_id==id) return &signals[i];
    }
    return NULL;
}
static int httpd_resp_send_chunk(httpd_req_t *request,const char *data,size_t size) {
    (void)request;return size&&fwrite(data,1,size,stdout)!=size?ESP_FAIL:ESP_OK;
}
'''+source[helper_start:helper_end]+source[start:end]+r'''
int main(void) {
    const uint32_t ids[]={187,185,188,0x703,189,187,185,187,0x700,0x700,0x700,CAN_ID_FRONT_LIN_1,CAN_ID_FRONT_LIN_2,CAN_ID_REAR_LIN_1,CAN_ID_REAR_LIN_2};
    const int32_t values[]={3000,-1000,1,4,1000,0,20000,3100,1234,0,1,25,50,75,100};
    FILE *file=tmpfile();if(!file)return 1;
    for(unsigned i=0;i<15;i++){
        uint64_t record[]={ids[i],((uint64_t)(1000+i*20)<<32)|(uint32_t)values[i]};
        fwrite(record,sizeof(record),1,file);
    }
    rewind(file);httpd_req_t request=0;
    int result=stream_csv(&request,file,NULL);fclose(file);return result;
}
'''
with tempfile.TemporaryDirectory() as directory:
    temp=Path(directory);(temp/'test.c').write_text(harness)
    (temp/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(temp),'-I'+str(ROOT/'src'),'-I'+str(ROOT.parent),str(temp/'test.c'),'-o',str(temp/'test')],check=True)
    full=subprocess.check_output([str(temp/'test')],text=True)
    assert full.splitlines()[0]=='sample_index,can_id,can_id_hex,signal,node,timestamp_ms,absolute_time_utc,value,units,raw_data'
    full_rows=list(csv.DictReader(io.StringIO(full)))
    assert len(full_rows)==15
    assert full_rows[3]["signal"]=="engine_rpm_aim"
    assert full_rows[3]["node"]=="aim_evo4s"
    assert [r['value'] for r in full_rows[8:11]]==['12.34','0.00','0.01']
    assert all(r['units']=='km/h' for r in full_rows[8:11])
    import runpy
    decoder=runpy.run_path(str(ROOT/'tools/decode_log.py'))
    import struct
    binary=temp/'speed.bin'
    binary.write_bytes(b''.join(struct.pack('<QQ',int(r['can_id']),int(r['raw_data'])) for r in full_rows))
    decoder['decode_log'](binary,temp/'speed.csv')
    decoded=list(csv.DictReader((temp/'speed.csv').open()))
    assert all(r['units']=='km/h' for r in decoded[8:11])
    assert decoded[3]['signal']==full_rows[3]['signal']=='engine_rpm_aim'
    assert decoded[3]['node']==full_rows[3]['node']=='aim_evo4s'
    assert [r['signal'] for r in full_rows[-4:]]==['front_lin_pot_1','front_lin_pot_2','rear_lin_pot_1','rear_lin_pot_2']
    assert all(r['units']=='mm' for r in full_rows[-4:])
    for firmware,python in zip(full_rows,decoded):
        for key in ('value','raw_data','timestamp_ms','signal','node','units'):
            assert firmware[key]==python[key], (key,firmware,python)
print('Full CSV export passed: GPS units, precision, and decoder parity')
