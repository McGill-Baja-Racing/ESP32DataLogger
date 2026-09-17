"""Offline end-to-end analysis test. Requires Playwright and a Chrome install."""
from pathlib import Path
import importlib.util
import json
import os
import struct
import tempfile
import threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
from playwright.sync_api import sync_playwright
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('mock',ROOT/'tools/cvt_reference/make_mock_log.py')
mock=importlib.util.module_from_spec(spec);spec.loader.exec_module(mock)
frame=mock.make_log(3,7)
records=b''.join(struct.pack('<qQ',int(r.can_id),(int(r.timestamp_ms)<<32)|(int(r.value)&0xffffffff)) for r in frame.itertuples())
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        path=urlparse(self.path).path
        if path=='/api/logs/download':
            if parse_qs(urlparse(self.path).query).get('format')==['cvt']:
                data=frame[frame.can_id.isin([185,187])].to_csv(index=False).encode();mime='text/csv'
            elif parse_qs(urlparse(self.path).query).get('format')==['paired']:
                data=b'Timestamp,Engine RPM,Wheel RPM\r\n1000,3000,1000\r\n1020,0,500\r\n';mime='text/csv'
            else:data=records;mime='application/octet-stream'
        elif path=='/api/logs':data=json.dumps([{'name':'log_0001.bin','size_bytes':len(records)}]).encode();mime='application/json'
        elif path=='/api/status':data=json.dumps({'logger_state':'idle','current_file':'none','can_drops':0,'log_drops':0,'nodes':{str(i):'off' for i in range(1,7)},'live_enabled':False}).encode();mime='application/json'
        elif path=='/api/live/signals':data=b'[]';mime='application/json'
        else:
            file=ROOT/'src/web'/({'/':'index.html','/analysis':'analysis.html'}.get(path,path.lstrip('/')))
            if not file.is_file():self.send_error(404);return
            data=file.read_bytes();mime='text/html' if file.suffix=='.html' else 'application/javascript'
        self.send_response(200)
        if path=='/api/logs/download' and mime=='text/csv':
            filename='log_0001_rpm_paired.csv' if parse_qs(urlparse(self.path).query).get('format')==['paired'] else 'log_0001_cvt_input.csv'
            self.send_header('Content-Disposition',f'attachment; filename="{filename}"')
        self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
base=f'http://127.0.0.1:{server.server_port}'
with sync_playwright() as p:
    browser=p.chromium.launch(executable_path=os.environ.get('CHROME_PATH','/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'),headless=True)
    context=browser.new_context(accept_downloads=True,viewport={'width':1280,'height':1000})
    # All application resources must be served locally, including chart library.
    external=[]
    def route(request):
        if not request.request.url.startswith(base):external.append(request.request.url);request.abort()
        else:request.continue_()
    context.route('**/*',route)
    page=context.new_page();errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto(base)
    assert page.locator('#logs a').all_text_contents()==['BIN','CSV','Powertrain CSV']
    with page.expect_download() as download:page.get_by_role('link',name='Powertrain CSV',exact=True).click()
    assert download.value.suggested_filename=='log_0001_rpm_paired.csv'
    assert Path(download.value.path()).read_text().splitlines()==['Timestamp,Engine RPM,Wheel RPM','1000,3000,1000','1020,0,500']
    page.goto(base+'/analysis?name=log_0001.bin')
    page.wait_for_function("document.getElementById('status').textContent==='Analysis complete'")
    assert page.locator('#ratio').input_value()=='3.389286'
    assert page.locator('#segment option').count()>=4
    assert page.locator('#results').is_visible()
    # Compare at the mock's known ratio and download all outputs.
    page.locator('#ratio').fill('2.75');page.locator('#ratio').blur()
    page.get_by_role('button',name='Run analysis',exact=True).click()
    page.wait_for_function("document.getElementById('status').textContent==='Analysis complete'")
    with tempfile.TemporaryDirectory() as tmp:
        for button,suffix in [('Download figure PNG','.png'),('Download processed CSV','.csv'),('Download summary','.txt')]:
            with page.expect_download() as download:page.get_by_role('button',name=button,exact=True).click()
            item=download.value;target=Path(tmp)/('result'+suffix);item.save_as(target);assert target.stat().st_size>100
        page.screenshot(path='/tmp/cvt-analysis-desktop.png',full_page=True)
    page.locator('#segment').select_option('2');page.get_by_role('button',name='Run analysis',exact=True).click()
    page.wait_for_function("document.getElementById('status').textContent==='Analysis complete'")
    assert 'Run 2' in page.locator('#settings').inner_text()
    page.get_by_role('button',name='Estimate shaft ratio',exact=True).click()
    page.wait_for_function("document.getElementById('status').textContent==='Analysis complete'")
    assert float(page.locator('#ratio').input_value())>0
    # Local CSV upload, missing idler, and malformed input.
    engine=frame[frame.can_id==187].to_csv(index=False)
    page.locator('#localFile').set_input_files({'name':'engine.csv','mimeType':'text/csv','buffer':engine.encode()})
    page.wait_for_function("document.getElementById('status').textContent==='Analysis complete'")
    assert 'engine-only' in page.locator('#warnings').inner_text()
    page.locator('#localFile').set_input_files({'name':'bad.csv','mimeType':'text/csv','buffer':b'wrong,columns\n1,2'})
    page.wait_for_function("document.getElementById('status').textContent==='Analysis unavailable'")
    assert not page.locator('#results').is_visible()
    mobile=context.new_page()
    mobile.set_viewport_size({'width':390,'height':844});mobile.goto(base+'/analysis?name=log_0001.bin')
    mobile.wait_for_function("document.getElementById('status').textContent==='Analysis complete'")
    assert mobile.evaluate('document.documentElement.scrollWidth <= innerWidth')
    mobile.screenshot(path='/tmp/cvt-analysis-mobile.png',full_page=True)
    assert not errors,errors
    assert not external,external
    browser.close()
server.shutdown()
print('Offline browser checks passed: button, charts, PNG/CSV/report, runs, calibration, uploads, mobile')
