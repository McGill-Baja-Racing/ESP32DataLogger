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
int main(int argc,char **argv) {
    (void)argv;
    const uint32_t ids[]={187,185,188,190,189,187,185,187};
    const int32_t values[]={3000,-1000,1,4,1000,0,20000,3100};
    FILE *file=tmpfile();if(!file)return 1;
    for(unsigned i=0;i<8;i++){
        uint64_t record[]={ids[i],((uint64_t)(1000+i*20)<<32)|(uint32_t)values[i]};
        fwrite(record,sizeof(record),1,file);
    }
    rewind(file);httpd_req_t request=0;
    int result=stream_csv(&request,file,argc>1);fclose(file);return result;
}
'''
with tempfile.TemporaryDirectory() as directory:
    temp=Path(directory);(temp/'test.c').write_text(harness)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(temp/'test.c'),'-o',str(temp/'test')],check=True)
    full=subprocess.check_output([str(temp/'test')],text=True)
    exported=subprocess.check_output([str(temp/'test'),'cvt'],text=True)
    rows=list(csv.DictReader(io.StringIO(exported)))
    assert exported.splitlines()[0]=='sample_index,can_id,can_id_hex,signal,node,timestamp_ms,value,units,raw_data'
    assert len(list(csv.DictReader(io.StringIO(full))))==8
    assert [int(r['can_id']) for r in rows]==[187,185,187,185,187]
    assert [int(r['value']) for r in rows]==[3000,-1000,0,20000,3100]
    assert [int(r['timestamp_ms']) for r in rows]==[1000,1020,1100,1120,1140]
    assert [int(r['sample_index']) for r in rows]==list(range(5))
    for row in rows:
        assert int(row['raw_data'])==(int(row['timestamp_ms'])<<32)|(int(row['value'])&0xffffffff)
        assert row['units']=='rpm'
print('CVT input CSV export passed: exact schema, filtering, signed/raw values, timestamps')
