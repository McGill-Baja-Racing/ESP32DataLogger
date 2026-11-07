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
health=(ROOT/'src/health/node_health.c').read_text()
health_start=health.index('static const char *const code_names')
health_end=health.index('#define SUMMARY_FRESH_MS',health_start)
# Statuses (one with every bit set), an event for every byte value, then GPS.
records=[(0x0BB,3000),(0x0B9,-1000),(0x0BC,1),(0x703,4),(0x0BD,1000),(0x0BB,0),(0x0B9,20000),(0x0BB,3100),
         (0x0D3,0x1100|2<<17|1<<19|2<<22|3<<30),(0x0D0,0),(0x0D6,0xFFFFFFFF)]
# Event payloads pack code:5 error:4 subject:8 sequence:6 count:9, so every code and error value is covered.
records+=[(0x0E5,code%32|(code%16)<<5|((0xB0+code)&0xFF)<<9|(code%64)<<17|(code*3%512)<<23) for code in range(256)]
records+=[(0x0E5,18|0x70<<9),(0x0E5,4|0x1FF<<23)]
records+=[(0x0F4,3|200<<8|2<<16|4<<24),(0x0F0,255|0<<8|0<<16|99<<24)]
records+=[(0x0D3,0xFFFFFFFF)]  # the longest status detail
records+=[(0x0BD,25),(0x0BE,50),(0x0BF,75),(0x0C9,100)]
records+=[(0x0D7,1<<20),(0x0D8,3<<22),(0x0E8,16|0xC9<<9),(0x0F8,4),(0x0E0,18|1<<9)]
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
#define NODE_HEALTH_TEXT_MAX 640
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
'''+health[health_start:health_end]+source[helper_start:helper_end]+source[start:end]+r'''
int main(void) {
    const uint32_t ids[]={'''+','.join(str(i) for i,_ in records)+r'''};
    const int32_t values[]={'''+','.join(str(v-(1<<32) if v>=1<<31 else v) for _,v in records)+r'''};
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
    (temp/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(temp),'-I'+str(ROOT/'src'),'-I'+str(ROOT.parent),str(temp/'test.c'),'-o',str(temp/'test')],check=True)
    full=subprocess.check_output([str(temp/'test')],text=True)
    assert full.splitlines()[0]=='sample_index,can_id,can_id_hex,signal,node,timestamp_ms,absolute_time_utc,value,units,raw_data,detail'
    full_rows=list(csv.DictReader(io.StringIO(full)))
    assert len(full_rows)==len(records)
    status,master=full_rows[8],full_rows[9]
    assert (status['signal'],status['node'],status['detail'])==('node_status','mpu_node_3','error recording can_passive gps_no_fix 0xB4 no_readings 0xB8 failed_to_start')
    assert (master['node'],master['detail'])==('master','ok idle')
    assert (full_rows[11+18]['node'],full_rows[11+18]['detail'])==('engine_node_5','node_offline brake_node_1 adc_node_6 front_shock_node_7')
    assert full_rows[11+256]['detail']=='node_offline encoder_node_4 engine_node_5 adc_node_6'
    assert full_rows[11+257]['detail']=='can_tx subject 0x00 count 511+ ESP_OK'
    diagnostics,own=full_rows[11+258],full_rows[11+259]
    assert (diagnostics['signal'],diagnostics['node'],diagnostics['detail'])==('node_diagnostics','encoder_node_4','tx_errors 3 rx_errors 200 resets 2 last_reset panic')
    assert (own['node'],own['detail'])==('master','tx_errors 255 rx_errors 0 resets 0 last_reset unknown')
    assert len(full_rows[11+260]['detail'])>320 and full_rows[11+260]['detail'].endswith('0xB8 failed_to_start')
    assert [r['value'] for r in full_rows[-3:]]==['12.34','0.00','0.01']
    assert all(r['units']=='km/h' for r in full_rows[-3:])
    assert full_rows[276]['detail']=='ok idle 0xBD some_readings_failed'
    assert full_rows[277]['detail']=='ok idle 0xC9 failed_to_start'
    assert full_rows[280]['detail']=='node_offline rear_shock_node_8'
    import runpy
    decoder=runpy.run_path(str(ROOT/'tools/decode_log.py'))
    import struct
    binary=temp/'speed.bin'
    binary.write_bytes(b''.join(struct.pack('<QQ',int(r['can_id']),int(r['raw_data'])) for r in full_rows))
    decoder['decode_log'](binary,temp/'speed.csv')
    decoded=list(csv.DictReader((temp/'speed.csv').open()))
    assert all(r['units']=='km/h' for r in decoded[-3:])
    assert decoded[3]['signal']==full_rows[3]['signal']=='engine_rpm_aim'
    assert decoded[3]['node']==full_rows[3]['node']=='aim_evo4s'
    assert [r['signal'] for r in full_rows[272:276]]==['front_lin_pot_1','front_lin_pot_2','rear_lin_pot_1','rear_lin_pot_2']
    assert all(r['units']=='mm' for r in full_rows[272:276])
    for firmware,python in zip(full_rows,decoded):
        for key in ('value','raw_data','timestamp_ms','detail','signal','node','units'):
            assert firmware[key]==python[key], (key,firmware,python)
print('Full CSV export passed: GPS units, precision, and decoder parity')
