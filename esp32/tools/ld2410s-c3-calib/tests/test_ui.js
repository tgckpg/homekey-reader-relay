const fs=require('fs'),vm=require('vm'),assert=require('assert');
const path=require('path');
const script=fs.readFileSync(path.join(__dirname,'..','main','web.html'),'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
const elements=new Map();
function element(id){if(!elements.has(id))elements.set(id,{value:'',textContent:'',width:1000,height:180,insertAdjacentHTML(){},getContext(){return new Proxy({},{get:(o,k)=>k in o?o[k]:(()=>{}),set:(o,k,v)=>(o[k]=v,true)})}});return elements.get(id)}
const settings={common:{far_gate:8,near_gate:0,absence_s:10,status_hz_x10:5,distance_hz_x10:5,response:5},trigger:Array(16).fill(40),hold:Array(16).fill(30)};
let current={connected:true,age:0,latest:{present:true,state:2,distance_cm:80,mode:'standard',energy:Array(16).fill(20)},settings,info:{uart_rx:3,uart_tx:4,firmware:'1.1.1',serial:'12345678',firmware_raw:'test'},network:{ap_ssid:'LD2410S-Bench',ap_ip:'192.168.4.1'},job:{id:0,busy:false,status:'Done'},calibration:null,reports:3,samples:3,bad_bytes:0,heap_free:100000,error:'',logs:[],history:[{present:true,distance_cm:80}]};
let submitted=[];
const context=vm.createContext({document:{getElementById:element,querySelectorAll:()=>[]},console,Date,Math,Number,Array,Object,JSON,Promise,Error,setInterval(){},setTimeout,confirm:()=>true,fetch:async(url,options)=>{
 if(url==='/api/state')return {ok:true,json:async()=>current};
 const args=JSON.parse(options.body||'{}');submitted.push([url,args]);
 if(url==='/api/write')current.settings=args.settings;
 current.job={id:current.job.id+1,busy:false,status:'Done'};
 return {ok:true,json:async()=>({job_id:current.job.id})};
}});
(async()=>{
 vm.runInContext(script,context);await new Promise(r=>setImmediate(r));
 assert.equal(element('presence').textContent,'PRESENT');assert.equal(element('far_gate').value,8);
 current.latest.present=false;current.latest.state=0;await vm.runInContext('poll()',context);assert.equal(element('distance').textContent,'No detected person · raw state 0');
 element('far_gate').value=2;
 await vm.runInContext("act('write',{settings:edited()})",context);assert.equal(current.settings.common.far_gate,2);
 element('delay').value=15;element('factorT').value=2;element('factorH').value=1;element('seconds').value=120;
 await vm.runInContext('calibrate()',context);
 assert.deepEqual(submitted.at(-1),['/api/calibrate',{trigger:2,hold:1,seconds:120,delay:15}]);
 current.age=9;await vm.runInContext('poll()',context);assert.equal(element('presence').textContent,'No fresh data');
 console.log('Dashboard JavaScript checks passed: initial settings, absence distance suppression, async write job, delayed calibration payload, stale readings');
})().catch(e=>{console.error(e);process.exitCode=1});
