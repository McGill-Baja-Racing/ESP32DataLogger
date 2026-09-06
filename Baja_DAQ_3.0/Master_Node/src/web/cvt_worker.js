'use strict';
importScripts('/cvt_analysis.js');
let raw;
self.onmessage=({data})=>{
  try{
    if(data.action==='load'){
      raw=CVT.parse(data.buffer,data.format);
      const ch=CVT.clean(raw,data.options);
      self.postMessage({type:'loaded',runs:CVT.segments(ch.engine.t)});
    }else if(!raw)throw Error('Choose a log first.');
    else if(data.action==='calibrate')self.postMessage({type:'calibrated',ratio:CVT.calibrate(raw,data.options)});
    else self.postMessage({type:'result',result:CVT.analyse(raw,data.options)});
  }catch(e){self.postMessage({type:'error',message:e.message||String(e)});}
};
