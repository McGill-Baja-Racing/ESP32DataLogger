"""Compare browser calculations against the supplied team's Python reference.
Run with: python3 tests/test_cvt_analysis.py (requires reference requirements).
"""
from pathlib import Path
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import tempfile
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
os.environ.setdefault('MPLBACKEND','Agg')
os.environ.setdefault('MPLCONFIGDIR',str(Path(tempfile.gettempdir())/'cvt-matplotlib'))
def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools/cvt_reference'/f'{name}.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod
ref=module('cvt_plot');mock=module('make_mock_log')
assert ref.IDLER_TO_DRIVEN == 3.389286
runner=r'''
const fs=require('fs'),CVT=require(process.argv[1]);
const request=JSON.parse(fs.readFileSync(0,'utf8'));
const bytes=Buffer.from(request.csv);const raw=CVT.parse(bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength),'csv');
const answer=request.calibrate?CVT.calibrate(raw,request.options):CVT.analyse(raw,request.options);
console.log(JSON.stringify(answer));
'''
def browser(df,options=None,calibrate=False):
    payload={'csv':df.to_csv(index=False),'options':options or {'idlerRatio':2.75},'calibrate':calibrate}
    p=subprocess.run(['node','-e',runner,str(ROOT/'src/web/cvt_analysis.js')],input=json.dumps(payload),text=True,capture_output=True)
    if p.returncode:raise AssertionError(p.stderr)
    return json.loads(p.stdout)
def compare(frame,ratio=2.75,segment=0):
    with tempfile.TemporaryDirectory() as d,contextlib.redirect_stdout(io.StringIO()):
        p=Path(d)/'mock.csv';frame.to_csv(p,index=False)
        chans=ref.load(p,ratio);window=None
        if segment:
            a,b=ref.segments(chans['engine'][0])[segment-1];window=(chans['engine'][0][a],chans['engine'][0][b-1])
        expected,n_out,have=ref.analyse(chans,ratio,window)
    actual=browser(frame,{'idlerRatio':ratio,'segment':segment})
    assert len(actual['rows'])==len(expected),(len(actual['rows']),len(expected))
    assert actual['summary']['outOfRange']==n_out
    assert actual['haveIdler']==have
    for key in expected.columns:
        want=expected[key].to_numpy(dtype=float)
        got=np.array([r[key] if r[key] is not None else np.nan for r in actual['rows']],float)
        np.testing.assert_allclose(got,want,rtol=1e-8,atol=1e-6,equal_nan=True,err_msg=key)
    return actual
frame=mock.make_log(3,7)
compare(frame)
compare(frame,segment=2)
compare(frame,ratio=3.389286)
compare(frame[frame.can_id==187])
# Calibration is an assumption, not a recovery guarantee: the mock does not
# necessarily reach the 0.44 full-overdrive ratio that calibration assumes.
with tempfile.TemporaryDirectory() as d,contextlib.redirect_stdout(io.StringIO()):
    p=Path(d)/'mock.csv';frame.to_csv(p,index=False);expected=ref.calibrate(ref.load(p,1))
np.testing.assert_allclose(browser(frame,{'idlerRatio':1},True),expected,rtol=1e-8)
# Very short logs are handled without scipy's original short-window crash.
short=frame[frame.can_id==187].iloc[:2]
assert len(browser(short)['rows'])>=1
print('CVT Python parity passed: mock, run selection, ratios, engine-only, calibration, short logs')
