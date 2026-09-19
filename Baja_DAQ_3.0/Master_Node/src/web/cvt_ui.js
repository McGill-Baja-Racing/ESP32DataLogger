'use strict';
const byId=id=>document.getElementById(id);
let worker,loaded=false,busy=false,result=null,filename='log',figures=[],reportText='',ratioEstimated=false;
const MAX_FILE_BYTES=128*1024*1024;
function opts(){return CVT.options({idlerRatio:Number(byId('ratio').value),maxRpm:Number(byId('maxRpm').value),segment:Number(byId('segment').value)});}
function setBusy(value,message){busy=value;for(const id of ['run','calibrate'])byId(id).disabled=value||!loaded;for(const id of ['ratio','maxRpm','segment','localFile'])byId(id).disabled=value;if(message)byId('status').textContent=message;}
function clearResults(){result=null;byId('results').hidden=true;for(const figure of figures)figure.destroy();figures=[];}
function fail(message){setBusy(false,'Analysis unavailable');byId('error').textContent=message;}
function startWorker(){
  if(worker)worker.terminate();worker=new Worker('/cvt_worker.js');
  worker.onerror=e=>fail(e.message||'Analysis worker could not start. Reload this page.');
  worker.onmessage=({data})=>{
    if(data.type==='error'){clearResults();fail(data.message);}
    else if(data.type==='loaded'){
      loaded=true;byId('segment').replaceChildren(new Option('All runs','0'));
      data.runs.forEach((r,i)=>byId('segment').add(new Option(`Run ${i+1} · ${(r.end-r.start).toFixed(1)} s · ${r.samples} engine samples`,String(i+1))));
      run();
    }else if(data.type==='calibrated'){
      ratioEstimated=true;byId('ratio').value=data.ratio.toFixed(6);byId('status').textContent='Using an estimated ratio; confirm against tooth counts.';run();
    }else if(data.type==='result'){
      try{result=data.result;render();setBusy(false,'Analysis complete');}catch(e){clearResults();fail(e.message);}
    }
  };
}
function run(action='analyse'){
  try{const options=opts();clearResults();byId('error').textContent='';setBusy(true,action==='calibrate'?'Estimating shaft ratio…':'Processing RPM samples…');worker.postMessage({action,options});}catch(e){fail(e.message);}
}
function loadBuffer(buffer,name){
  try{
    if(buffer.byteLength>MAX_FILE_BYTES)throw Error('Quick analysis supports files up to 128 MiB. Use the Python tool for larger logs.');
    clearResults();filename=name;byId('filename').textContent=name;loaded=false;startWorker();setBusy(true,'Reading RPM channels…');
    worker.postMessage({action:'load',buffer,format:name.toLowerCase().endsWith('.bin')?'bin':'csv',options:opts()},[buffer]);
  }catch(e){fail(e.message);}
}
async function fetchLog(name){
  try{
    setBusy(true,'Downloading log for analysis…');
    const response=await fetch(`/api/logs/download?name=${encodeURIComponent(name)}&format=bin`);
    if(!response.ok){const body=await response.json().catch(()=>({}));throw Error(body.error||`Download failed: HTTP ${response.status}`);}
    const reader=response.body.getReader(),chunks=[];let size=0;
    while(true){const {value,done}=await reader.read();if(done)break;size+=value.length;if(size>MAX_FILE_BYTES){await reader.cancel();throw Error('Log exceeds the 128 MiB quick-analysis limit. Download it and use the Python tool.');}chunks.push(value);byId('status').textContent=`Downloading log… ${(size/1048576).toFixed(1)} MiB`;}
    const bytes=new Uint8Array(size);let offset=0;for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.length;}loadBuffer(bytes.buffer,name);
  }catch(e){fail(e.message);}
}
const fmt=(x,d=1)=>Number.isFinite(x)?x.toFixed(d):'Unavailable';
function metric(label,value){const box=document.createElement('div');box.className='metric';const strong=document.createElement('strong');strong.textContent=value;box.append(strong,document.createTextNode(label));byId('metrics').append(box);}
function render(){
  const {summary:s,rows,options:o,stats,warnings}=result;
  byId('results').hidden=false;byId('metrics').replaceChildren();
  metric('Engine running',`${fmt(s.runningSeconds)} s`);metric('Engine median',`${fmt(s.engineMedian,0)} rpm`);
  metric('CVT ratio span',`${fmt(s.ratioMin,2)} – ${fmt(s.ratioMax,2)}`);metric('Estimated driven torque max',`${fmt(s.drivenTorqueMax)} ft-lb`);
  byId('settings').textContent=`${filename} · Shaft ratio ${o.idlerRatio}${ratioEstimated?' (estimated: assumes full overdrive)':''} · RPM ceiling ${o.maxRpm} · ${o.segment?`Run ${o.segment}`:'All runs'}`;
  const lines=[`Engine running: ${fmt(s.runningSeconds)} s of ${fmt(s.duration)} s`,
    `Engine RPM: median ${fmt(s.engineMedian,0)}, p95 ${fmt(s.engineP95,0)}`,
    `Peak-power reference: ${fmt(s.peak,0)} rpm; 98% band ${fmt(s.band[0],0)}–${fmt(s.band[1],0)} rpm`,
    `In 98% power band: ${fmt(s.bandPercent)}% of engaged samples`,
    `CVT ratio: ${fmt(s.ratioMin,3)} to ${fmt(s.ratioMax,3)} (mechanical range 0.44–3.01)`,
    `Estimated driven torque: max ${fmt(s.drivenTorqueMax)}, median ${fmt(s.drivenTorqueMedian)} ft-lb`,
    `Engine input: ${stats.engine.rows} rows; ${stats.engine.dropped} implausible/nonfinite samples removed`,
    `Idler input: ${stats.idler.rows} rows; ${stats.idler.dropped} implausible/nonfinite samples removed`,
    `Out-of-range ratio samples removed: ${s.outOfRange}`];
  byId('summary').textContent=lines.join('\n');byId('warnings').textContent=warnings.join('\n');byId('warnings').hidden=!warnings.length;
  reportText=`Baja CVT analysis\n${byId('settings').textContent}\n\n${lines.join('\n')}\n\n${warnings.join('\n')}\n\nTorque/power are WOT map estimates, not measured torque. Dyno map: 2400–3600 rpm, held flat outside this range. Efficiency: 0.85.\n50 Hz grid; 0.10 s median; 0.40 s quadratic smoothing; interpolation gap limit 0.20 s.\n`;
  drawFigures(rows);
}
function color(f){const colors=[[68,1,84],[59,82,139],[33,145,140],[94,201,98],[253,231,37]],p=Math.max(0,Math.min(.9999,f))*4,i=Math.floor(p),a=colors[i],b=colors[i+1];return `rgb(${a.map((x,j)=>Math.round(x+(b[j]-x)*(p-i))).join(',')})`;}
function scatter(rows,x,y){const stride=Math.max(1,Math.ceil(rows.length/6000)),selected=rows.filter((_,i)=>i%stride===0),end=result.summary.duration;return {type:'scatter',label:'Samples (purple → yellow = elapsed time)',data:selected.map(r=>({x:r[x],y:r[y]})),pointRadius:2,pointBackgroundColor:selected.map(r=>color(r.t_rel_s/end)),pointBorderWidth:0};}
function line(rows,key,label,color,yAxisID='y'){
  const stride=Math.max(1,Math.ceil(rows.length/4000)),data=[];
  for(let i=0;i<rows.length;i+=stride){let value=rows[i][key];for(let j=i;j<Math.min(rows.length,i+stride);j++)if(!Number.isFinite(rows[j][key])){value=null;break;}data.push({x:rows[i].t_rel_s,y:Number.isFinite(value)?value:null});}
  return {type:'line',label,data,borderColor:color,backgroundColor:color,pointRadius:0,borderWidth:1.3,spanGaps:false,yAxisID};
}
function chart(id,title,datasets,xTitle,yTitle,extra={}){
  const options={responsive:true,maintainAspectRatio:false,animation:false,parsing:false,plugins:{title:{display:true,text:title},legend:{labels:{boxWidth:14,font:{size:10}}}},scales:{x:{type:'linear',title:{display:true,text:xTitle}},y:{type:'linear',title:{display:true,text:yTitle}}}};
  if(extra.reverse){options.scales.x.reverse=true;options.scales.x.min=.44*.9;options.scales.x.max=3.01*1.08;}
  if(extra.y1)options.scales.y1={position:'right',reverse:true,min:.44*.85,max:3.01*1.15,grid:{drawOnChartArea:false},title:{display:true,text:'CVT ratio'}};
  const figure=new Chart(byId(id),{type:'line',data:{datasets},options});figures.push(figure);
}
function drawFigures(rows){
  const engaged=rows.filter(r=>Number.isFinite(r.cvt_ratio)),shift=engaged.filter(r=>r.engine_rpm>1500);
  const datasets=[scatter(shift,'cvt_ratio','engine_rpm')];
  if(shift.length){
    const min=shift.reduce((v,r)=>Math.min(v,r.cvt_ratio),Infinity),max=shift.reduce((v,r)=>Math.max(v,r.cvt_ratio),-Infinity);
    const bins=Array.from({length:40},()=>[]);for(const r of shift){const i=max===min?0:Math.min(39,Math.floor((r.cvt_ratio-min)/(max-min)*40));if(i>=0)bins[i].push(r);}
    const stats=bins.filter(b=>b.length>=3).map(b=>({x:CVT.quantile(b.map(r=>r.cvt_ratio),.5),median:CVT.quantile(b.map(r=>r.engine_rpm),.5),lo:CVT.quantile(b.map(r=>r.engine_rpm),.1),hi:CVT.quantile(b.map(r=>r.engine_rpm),.9)}));
    for(const [key,label] of [['lo','10th percentile'],['hi','90th percentile'],['median','Median shift curve']])datasets.push({type:'line',label,data:stats.map(r=>({x:r.x,y:r[key]})),pointRadius:0,borderColor:'#c0392b',borderWidth:key==='median'?2:0,backgroundColor:'#c0392b25',fill:key==='hi'?'-1':false});
  }
  datasets.push({type:'line',label:`Peak power ${fmt(CVT.peak,0)} rpm`,data:[{x:.396,y:CVT.peak},{x:3.251,y:CVT.peak}],borderColor:'#17202a',borderDash:[5,4],pointRadius:0});
  for(const [i,y] of CVT.band.entries())datasets.push({type:'line',label:i?'98% power band':'Band lower edge',data:[{x:.396,y},{x:3.251,y}],pointRadius:0,borderWidth:0,backgroundColor:'#17202a15',fill:i?'-1':false});
  chart('shift',shift.length?'CVT shift curve':'CVT shift curve — no valid moving samples',datasets,'CVT ratio (low → high)','Engine speed [rpm]',{reverse:true});
  const ideal=Array.from({length:200},(_,i)=>{const x=.44+(3.01-.44)*i/199;return{x,y:CVT.torque(CVT.peak)*x*.85};});
  chart('torqueRatio','Estimated driven-clutch torque',[scatter(engaged,'cvt_ratio','driven_torque_ftlb'),{type:'line',label:`Ideal at ${fmt(CVT.peak,0)} rpm`,data:ideal,borderColor:'#e67e22',borderDash:[5,4],pointRadius:0}],'CVT ratio','Torque [ft-lb]',{reverse:true});
  const torqueRows=rows.map(r=>({...r,engine_torque_ftlb:r.engine_rpm>10?r.engine_torque_ftlb:NaN,outside:r.engine_rpm>10&&r.engine_rpm_outside_dyno_map?r.engine_torque_ftlb:NaN}));
  const outside=line(torqueRows,'outside','Outside dyno map','#999999');outside.fill='origin';outside.backgroundColor='#99999944';
  chart('torqueTime','Estimated torque versus time',[outside,line(torqueRows,'engine_torque_ftlb','Engine (WOT map)','#c0392b'),line(rows,'driven_torque_ftlb','Driven clutch','#2c3e50')],'Time [s]','Torque [ft-lb]');
  const speeds=[line(rows,'engine_rpm','Engine','#c0392b')];if(result.haveIdler)speeds.push(line(rows,'driven_rpm','Driven clutch','#16a085'),line(rows,'cvt_ratio','CVT ratio','#2c3e50','y1'));
  chart('speeds','Speeds and ratio versus time',speeds,'Time [s]','Speed [rpm]',{y1:result.haveIdler});
}
function download(blob,suffix){const a=document.createElement('a'),url=URL.createObjectURL(blob);a.href=url;a.download=filename.replace(/\.(bin|csv)$/i,'')+'_cvt'+suffix;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),30000);}
byId('csv').onclick=()=>{if(result)download(new Blob([CVT.csv(result.rows)],{type:'text/csv'}),'.csv');};
byId('report').onclick=()=>download(new Blob([reportText],{type:'text/plain'}),'_summary.txt');
byId('png').onclick=()=>{
  if(!result)return;const canvas=document.createElement('canvas');canvas.width=1600;canvas.height=1150;const c=canvas.getContext('2d');c.fillStyle='white';c.fillRect(0,0,1600,1150);c.fillStyle='#17202a';c.font='24px system-ui';c.fillText('Baja CVT analysis · '+filename,25,35);c.font='17px system-ui';c.fillText(`Shaft ratio ${result.options.idlerRatio} · engine ceiling ${result.options.maxRpm} rpm · ${result.options.segment?'Run '+result.options.segment:'All runs'}`,25,65);
  figures.forEach((f,i)=>c.drawImage(f.canvas,15+(i%2)*800,90+Math.floor(i/2)*490,770,470));c.fillText('Torque/power are WOT map estimates, not measured torque. Assumed CVT efficiency: 0.85.',25,1115);
  canvas.toBlob(blob=>{if(blob)download(blob,'.png');},'image/png');
};
byId('run').onclick=()=>run();byId('calibrate').onclick=()=>run('calibrate');
for(const id of ['ratio','maxRpm','segment'])byId(id).onchange=()=>{if(id==='ratio')ratioEstimated=false;clearResults();byId('status').textContent='Settings changed. Run analysis to update results.';};
byId('localFile').onchange=async()=>{const file=byId('localFile').files[0];if(!file)return;try{if(file.size>MAX_FILE_BYTES)throw Error('Choose a file no larger than 128 MiB.');setBusy(true,'Opening log…');loadBuffer(await file.arrayBuffer(),file.name);}catch(e){fail(e.message);}};
window.addEventListener('pagehide',()=>{if(worker)worker.terminate();});
const logName=new URLSearchParams(location.search).get('name');
if(logName){if(/^[A-Za-z0-9_-]{1,40}\.bin$/.test(logName))fetchLog(logName);else fail('Invalid log filename. Open analysis from the completed logs list.');}
