/* Browser port of the team's cvt_plot.py. No network or DOM dependencies. */
'use strict';
const CVT = (() => {
  const defaults = Object.freeze({idlerRatio:1.69565, maxRpm:4000, segment:0});
  const C = Object.freeze({hz:50, off:10, low:3.01, high:.44, efficiency:.85,
    drivenMin:50, maxGap:.2, segmentGap:2, minPlot:1500, maxPoints:500000});
  const dynoRpm=[2400,2600,2800,3000,3200,3400,3600];
  const dynoTorque=[18.5,18.1,17.4,16.6,15.4,14.5,13.5];
  const finite=Number.isFinite;
  function options(input={}) {
    const o={...defaults,...input};
    if(!finite(o.idlerRatio)||o.idlerRatio<=0||o.idlerRatio>100)throw Error('Enter a shaft ratio greater than 0 and at most 100.');
    if(!finite(o.maxRpm)||o.maxRpm<100||o.maxRpm>50000)throw Error('Enter an engine RPM ceiling between 100 and 50000.');
    if(!Number.isInteger(o.segment)||o.segment<0)throw Error('Choose a valid run.');
    return o;
  }
  function quantile(values,q) {
    const a=values.filter(finite).sort((a,b)=>a-b);if(!a.length)return NaN;
    const p=(a.length-1)*q,i=Math.floor(p),f=p-i;return a[i]+f*((a[i+1]??a[i])-a[i]);
  }
  function interp(t,v,x) {
    if(!finite(x))return NaN;
    if(x<=t[0])return v[0];if(x>=t[t.length-1])return v[v.length-1];
    let lo=0,hi=t.length-1;
    while(hi-lo>1){const m=(lo+hi)>>1;if(t[m]<=x)lo=m;else hi=m;}
    return v[lo]+(v[hi]-v[lo])*(x-t[lo])/(t[hi]-t[lo]);
  }
  const torque=r=>interp(dynoRpm,dynoTorque,r);
  const power=r=>torque(r)*r/5252;
  const mapGrid=Array.from({length:4001},(_,i)=>2400+i*1200/4000);
  const peak=mapGrid.reduce((best,x)=>power(x)>power(best)?x:best,2400);
  const bandValues=mapGrid.filter(x=>power(x)>=.98*power(peak));
  const band=[bandValues[0],bandValues[bandValues.length-1]];
  function csvFields(line) {
    const out=[];let s='',quote=false;
    for(let i=0;i<line.length;i++){const ch=line[i];if(ch==='"'){if(quote&&line[i+1]==='"'){s+='"';i++;}else quote=!quote;}else if(ch===','&&!quote){out.push(s);s='';}else s+=ch;}
    if(quote)throw Error('Unclosed quote in CSV. Use the DAQ CSV export.');out.push(s);return out;
  }
  function parse(buffer,format) {
    const raw={engine:[],idler:[],warnings:[]};
    function add(id,ms,value) {
      const key=id===187?'engine':id===185?'idler':null;if(!key)return;
      if(!finite(ms)||ms<0||ms>0xffffffff)return;
      raw[key].push([ms/1000,value]);
    }
    if(format==='bin'){
      const d=new DataView(buffer);if(d.byteLength<16)throw Error('The binary log is empty or incomplete.');
      for(let i=0;i+16<=d.byteLength;i+=16)add(d.getUint32(i,true)&0x7ff,d.getUint32(i+12,true),d.getInt32(i+8,true));
      if(d.byteLength%16)raw.warnings.push(`${d.byteLength%16} trailing bytes ignored (incomplete log record).`);
    }else{
      const lines=new TextDecoder().decode(buffer).replace(/^\uFEFF/,'').split(/\r?\n/);
      const header=csvFields(lines.shift()||'').map(x=>x.trim()),ci=header.indexOf('can_id'),ti=header.indexOf('timestamp_ms'),vi=header.indexOf('value');
      if(Math.min(ci,ti,vi)<0)throw Error('CSV needs can_id, timestamp_ms and value columns.');
      for(const line of lines){if(!line.trim())continue;const row=csvFields(line);if(!row[ci]?.trim()||!row[ti]?.trim())continue;add(Number(row[ci]),Number(row[ti]),row[vi]?.trim()?Number(row[vi]):NaN);}
    }
    return raw;
  }
  function clean(raw,input) {
    const o=options(input),caps={engine:o.maxRpm,idler:1.15*o.maxRpm/C.high/o.idlerRatio};
    const chans={stats:{},warnings:[...(raw.warnings||[])]};
    for(const key of ['engine','idler']){
      // Unwrap the logger's 32-bit millisecond clock before sorting.
      let wrap=0,last=null;const rows=raw[key].map(([t,v])=>{if(last!==null&&last>4026531.84&&t<268435.456){wrap+=4294967.296;}last=t;return[t+wrap,v];}).sort((a,b)=>a[0]-b[0]);
      const t=[],v=[];let previous=null,bad=0,negative=0;
      for(const [time,value] of rows){if(time===previous)continue;previous=time;
        if(!finite(value)||value>caps[key]){bad++;continue;}
        if(value<0)negative++;t.push(time);v.push(value);
      }
      chans[key]={t,v};chans.stats[key]={rows:rows.length,dropped:bad,negative,cap:caps[key]};
      if(negative)chans.warnings.push(`${key}: ${negative} negative RPM samples; check encoder direction. The supplied analysis expects forward-positive RPM.`);
    }
    if(!chans.engine.t.length)throw Error('No usable engine RPM (0x0BB) samples. Record node 5 and check the RPM ceiling.');
    return chans;
  }
  function segments(t) {
    const out=[];if(!t.length)return out;let start=0;
    for(let i=1;i<=t.length;i++)if(i===t.length||t[i]-t[i-1]>C.segmentGap){out.push({start:t[start],end:t[i-1],samples:i-start});start=i;}
    return out;
  }
  function resample(t,v,grid) {
    if(t.length<2)return grid.map(()=>NaN);let j=0;
    return grid.map(x=>{while(j+1<t.length&&t[j+1]<=x)j++;
      if(x<t[0]||x>t[t.length-1])return NaN;
      if(x===t[j]||j===t.length-1)return v[j];
      if(t[j+1]-t[j]>C.maxGap)return NaN;
      return v[j]+(v[j+1]-v[j])*(x-t[j])/(t[j+1]-t[j]);});
  }
  function smooth(x) {
    const valid=[];for(let i=0;i<x.length;i++)if(finite(x[i]))valid.push(i);
    if(!valid.length)return x.slice();
    const y=x.slice();let j=0;
    for(let i=0;i<y.length;i++)if(!finite(y[i])){
      while(j+1<valid.length&&valid[j+1]<i)j++;
      if(i<valid[0])y[i]=x[valid[0]];
      else if(i>valid[valid.length-1])y[i]=x[valid[valid.length-1]];
      else{const a=valid[j],b=valid[j+1];y[i]=x[a]+(x[b]-x[a])*(i-a)/(b-a);}
    }
    // pandas centered rolling median, five samples, min_periods=1.
    const med=y.map((_,i)=>quantile(y.slice(Math.max(0,i-2),i+3),.5));
    const w=Math.min(21,med.length%2?med.length:med.length-1),m=(w-1)/2;
    if(w<3)return med.map((v,i)=>finite(x[i])?v:NaN);
    let s2=0,s4=0;for(let j=-m;j<=m;j++){s2+=j*j;s4+=j**4;}
    const denominator=w*s4-s2*s2;
    // Quadratic least-squares Savitzky-Golay, including scipy mode='interp' edges.
    return med.map((_,i)=>{
      if(!finite(x[i]))return NaN;
      const start=Math.min(Math.max(i-m,0),med.length-w),at=i-start-m;
      let y0=0,y1=0,y2=0;
      for(let k=0;k<w;k++){const z=k-m,value=med[start+k];y0+=value;y1+=z*value;y2+=z*z*value;}
      const a=(y0*s4-y2*s2)/denominator,b=y1/s2,c=(w*y2-s2*y0)/denominator;
      return a+b*at+c*at*at;
    });
  }
  function analyse(raw,input={}) {
    const o=options(input),ch=clean(raw,o),runs=segments(ch.engine.t),have=ch.idler.t.length>1;
    let lo=have?Math.max(ch.engine.t[0],ch.idler.t[0]):ch.engine.t[0];
    let hi=have?Math.min(ch.engine.t.at(-1),ch.idler.t.at(-1)):ch.engine.t.at(-1);
    if(o.segment){const run=runs[o.segment-1];if(!run)throw Error('That run is not available.');lo=Math.max(lo,run.start);hi=Math.min(hi,run.end);}
    if(hi<=lo)throw Error('Need at least two engine samples and overlapping engine/idler times for a paired analysis.');
    const n=Math.ceil((hi-lo)*C.hz);if(n>C.maxPoints)throw Error('This time span is too long for quick analysis. Select an individual run.');
    const step=(lo+1/C.hz)-lo,grid=Array.from({length:n},(_,i)=>lo+i*step);
    const eng=smooth(resample(ch.engine.t,ch.engine.v,grid));
    const idl=have?smooth(resample(ch.idler.t,ch.idler.v,grid)):grid.map(()=>NaN);
    let outOfRange=0;
    const rows=grid.map((t,i)=>{
      const e=eng[i],driven=idl[i]*o.idlerRatio;let ratio=e/driven;
      if(!finite(ratio)||driven<C.drivenMin)ratio=NaN;
      if(finite(ratio)&&(ratio>C.low*1.05||ratio<C.high*.95)){ratio=NaN;outOfRange++;}
      const tq=torque(e),dt=tq*ratio*C.efficiency;
      return {t_s:t,t_rel_s:t-grid[0],engine_rpm:e,idler_rpm:idl[i],driven_rpm:driven,cvt_ratio:ratio,
        engine_torque_ftlb:tq,engine_power_hp:power(e),engine_rpm_outside_dyno_map:e<2400||e>3600,
        driven_torque_ftlb:dt,driven_power_hp:dt*driven/5252};
    });
    const running=rows.filter(r=>r.engine_rpm>C.off),engaged=rows.filter(r=>finite(r.cvt_ratio));
    const range=(key)=>engaged.reduce((a,r)=>[Math.min(a[0],r[key]),Math.max(a[1],r[key])],[Infinity,-Infinity]);
    const ratioRange=range('cvt_ratio'),torqueRange=range('driven_torque_ftlb');
    const summary={duration:rows.length/C.hz,runningSeconds:running.length/C.hz,engineMedian:quantile(running.map(r=>r.engine_rpm),.5),engineP95:quantile(running.map(r=>r.engine_rpm),.95),
      ratioMin:engaged.length?ratioRange[0]:NaN,ratioMax:engaged.length?ratioRange[1]:NaN,
      bandPercent:engaged.length?100*engaged.filter(r=>r.engine_rpm>=band[0]&&r.engine_rpm<=band[1]).length/engaged.length:NaN,
      drivenTorqueMax:engaged.length?torqueRange[1]:NaN,drivenTorqueMedian:quantile(engaged.map(r=>r.driven_torque_ftlb),.5),outOfRange,peak,band};
    const warnings=ch.warnings;
    if(!have)warnings.push('No usable idler RPM (0x0B9): engine-only analysis. CVT ratio and driven torque are unavailable.');
    else if(!engaged.length)warnings.push('No valid CVT ratios. Check shaft motion, the shaft ratio and sensor readings.');
    if(outOfRange)warnings.push(`${outOfRange} samples outside the mechanical CVT range were excluded; check shaft ratio, belt slip or sensor faults.`);
    return {rows,summary,runs,stats:ch.stats,haveIdler:have,options:o,warnings};
  }
  function calibrate(raw,input={}) {
    const result=analyse(raw,{...input,idlerRatio:1}),a=result.rows.filter(r=>finite(r.engine_rpm)&&finite(r.idler_rpm)&&r.idler_rpm>C.drivenMin&&r.engine_rpm>C.minPlot).map(r=>r.engine_rpm/r.idler_rpm);
    const k=quantile(a,.01)/C.high;if(!finite(k))throw Error('Not enough moving engine/idler data to estimate the shaft ratio.');return k;
  }
  const columns=['t_s','t_rel_s','engine_rpm','idler_rpm','driven_rpm','cvt_ratio','engine_torque_ftlb','engine_power_hp','engine_rpm_outside_dyno_map','driven_torque_ftlb','driven_power_hp'];
  function csv(rows){return columns.join(',')+'\n'+rows.map(r=>columns.map(k=>typeof r[k]==='boolean'?(r[k]?'True':'False'):finite(r[k])?r[k]:'').join(',')).join('\n')+'\n';}
  return {defaults,C,parse,clean,segments,analyse,calibrate,smooth,resample,quantile,torque,power,peak,band,csv,options};
})();
if(typeof module!=='undefined')module.exports=CVT;
