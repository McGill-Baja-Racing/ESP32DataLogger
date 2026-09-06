"""Browser regression for independent derived live RPM views from one CAN source."""
from pathlib import Path
import json
import math
import os
import threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse
from playwright.sync_api import sync_playwright

ROOT=Path(__file__).resolve().parents[1]
state={'live':False,'sample':(1000,1)}
metadata=[{'can_id':185,'signal':'bearing_rpm','node':'encoder_node_4','units':'rpm','native_rate_hz':50},
          {'can_id':187,'signal':'engine_rpm','node':'engine_node_5','units':'rpm','native_rate_hz':100},
          {'can_id':189,'signal':'engine_wheel_rpm','node':'master_pair_node_4_5','units':'rpm','native_rate_hz':100},
          {'can_id':186,'signal':'generic_adc_voltage','node':'adc_node_6','units':'mV','native_rate_hz':100}]
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def respond(self,body,mime='application/json'):
        data=body if isinstance(body,bytes) else json.dumps(body).encode()
        self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_GET(self):
        path=urlparse(self.path).path
        if path=='/':return self.respond((ROOT/'src/web/index.html').read_bytes(),'text/html')
        if path=='/api/status':return self.respond({'logger_state':'running','current_file':'log_0001.bin','can_drops':0,'log_drops':0,'nodes':{str(i):'active' for i in range(1,7)},'live_enabled':state['live']})
        if path=='/api/live/signals':return self.respond(metadata)
        if path=='/api/logs':return self.respond([])
        if path=='/api/live/samples':
            value,sequence=state['sample']
            return self.respond({'samples':[{'can_id':cid,'value':v,'timestamp_ms':sequence*20,'sequence':sequence} for cid,v in [(185,value),(187,3000)]]})
        self.send_error(404)
    def do_POST(self):
        path=urlparse(self.path).path
        state['live']=path=='/api/live/start'
        self.respond({'token':'abc','live_enabled':state['live']})
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
with sync_playwright() as p:
    browser=p.chromium.launch(executable_path=os.environ.get('CHROME_PATH','/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'),headless=True)
    page=browser.new_page(viewport={'width':1280,'height':1000});errors=[]
    page.on('pageerror',lambda error:errors.append(str(error)))
    page.goto(f'http://127.0.0.1:{server.server_port}')
    def checkbox(key):return page.locator(f'#signals input[data-id="{key}"]')
    def chart(key):return page.locator(f'section.chart[data-id="{key}"]')
    def wait_value(key,value):page.wait_for_function('(args)=>document.querySelector(`section.chart[data-id="${args[0]}"] .value`)?.textContent===args[1]',arg=[str(key),value])
    for key in [185,'secondary_rpm','wheel_rpm',187]:checkbox(key).check()
    checkbox(186).click();assert not checkbox(186).is_checked()
    assert page.locator('section.chart').count()==4
    assert chart(185).locator('h3').inner_text()=='Raw Bearing RPM'
    assert chart('secondary_rpm').locator('h3').inner_text()=='Secondary RPM'
    assert chart('wheel_rpm').locator('h3').inner_text()=='Wheel RPM'
    assert '3.389286' in chart('secondary_rpm').inner_text()
    assert '3.589' in chart('wheel_rpm').inner_text()
    page.get_by_role('button',name='Enable live data',exact=True).click()
    wait_value(185,'1000 rpm');wait_value('secondary_rpm','3389.3 rpm');wait_value('wheel_rpm','278.6 rpm')
    secondary=page.evaluate("charts.get('secondary_rpm').points.at(-1)")
    wheel=page.evaluate("charts.get('wheel_rpm').points.at(-1)")
    assert math.isclose(secondary['v'],1000*3.389286)
    assert math.isclose(wheel['v'],1000/3.589)
    assert secondary['t']==wheel['t']==20
    chart('secondary_rpm').get_by_role('button',name='Pause',exact=True).click()
    state['sample']=(2000,2)
    wait_value(185,'2000 rpm');wait_value('wheel_rpm','557.3 rpm')
    assert chart('secondary_rpm').locator('.value').inner_text()=='3389.3 rpm'
    chart('secondary_rpm').get_by_role('button',name='Resume',exact=True).click()
    wait_value('secondary_rpm','6778.6 rpm')
    # Removing one view must not remove another view with the same source ID.
    checkbox(185).uncheck();assert chart('secondary_rpm').count()==1 and chart('wheel_rpm').count()==1
    checkbox(185).check();wait_value(185,'2000 rpm')
    page.screenshot(path='/tmp/live-rpm-views-desktop.png',full_page=True)
    state['sample']=(0,3)
    wait_value(185,'0 rpm');wait_value('secondary_rpm','0.0 rpm');wait_value('wheel_rpm','0.0 rpm')
    state['sample']=(-100,4)
    wait_value(185,'-100 rpm');wait_value('secondary_rpm','-338.9 rpm');wait_value('wheel_rpm','-27.9 rpm')
    checkbox(187).uncheck();checkbox(189).check()
    assert chart(189).locator('h3').inner_text()=='Engine Paired Bearing RPM'
    wait_value(189,'Unavailable')
    page.set_viewport_size({'width':390,'height':844})
    assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
    page.screenshot(path='/tmp/live-rpm-views-mobile.png',full_page=True)
    page.get_by_role('button',name='Disable',exact=True).click()
    page.wait_for_function("document.getElementById('liveState').textContent==='Disabled' && charts.get('wheel_rpm').points.length===0")
    assert not errors,errors
    browser.close()
server.shutdown()
print('Live RPM browser checks passed: formulas, labels, independent views, pause, zeros, signs, mobile')
