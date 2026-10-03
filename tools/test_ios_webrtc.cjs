/* Opt-in internet test against public room signaling, using a unique test room.
   Install playwright under build/network-test and its Chromium browser first. */
const {chromium}=require('../build/network-test/node_modules/playwright');
const fs=require('node:fs'),http=require('node:http'),path=require('node:path');
const {spawn,execFileSync}=require('node:child_process');
const root=path.resolve(__dirname,'..');
const room='HALOTEST'+require('node:crypto').randomBytes(8).toString('hex').toUpperCase();
const source=fs.readFileSync(path.join(root,'port/web/site/net.js'),'utf8');
const peer=`
const memory={buffer:new ArrayBuffer(3*1024*1024+64)},w=new Int32Array(memory.buffer),b=new Uint8Array(memory.buffer);
const offsets={netLocalAddress:0,netOutWrite:4,netOutRead:8,netInWrite:12,netInRead:16,netOut:64,netOutBytes:1048576,netIn:1048640,netInBytes:2097152};
HaloNet.attach({memory,base:0,offsets});
HaloNet.on((type,value)=>console.log(type,JSON.stringify(value)));
HaloNet.join('${room}').then(()=>HaloNet.quickPlay()).then(s=>console.log('selected',JSON.stringify(s)));
setInterval(()=>{
 let r=w[4]>>>0, end=w[3]>>>0;
 while(r!==end){
  let h=new Uint8Array(24);for(let i=0;i<24;i++)h[i]=b[offsets.netIn+((r+i)&2097151)];
  let v=new DataView(h.buffer),n=v.getUint32(0,true);let packet=new Uint8Array(n);
  for(let i=0;i<n;i++)packet[i]=b[offsets.netIn+((r+i)&2097151)];
  r=(r+n)>>>0;
  if(![1,3,4].includes(v.getUint32(4,true)))continue;
  let p=new DataView(packet.buffer);p.setUint32(12,v.getUint32(8,true),true);p.setUint32(8,HaloNet.address,true);
  p.setUint16(16,v.getUint16(18,true),true);p.setUint16(18,v.getUint16(16,true),true);
  let out=w[1]>>>0;if(n>1048576-((out-(w[2]>>>0))>>>0))throw Error('fixture ring full');
  for(let i=0;i<n;i++)b[64+((out+i)&1048575)]=packet[i];w[1]=(out+n)|0;
 }
 w[4]=r|0;
},4);
`;
(async()=>{
 fs.mkdirSync(path.join(root,'build/ios/probe'),{recursive:true});
 execFileSync('xcrun',['clang','-fobjc-arc','-O2','-Iport/linux/src','-Iport/web/src',
 'port/ios/tests/webrtc_probe.m','port/ios/network/room_bridge.c','port/web/src/web_net.c',
 '-framework','Cocoa','-framework','WebKit','-o','build/ios/probe/webrtc-probe'],{cwd:root,stdio:'inherit'});
 const server=http.createServer((req,res)=>{res.setHeader('Content-Type','text/html');res.end('<script>'+source+'\n'+peer+'</script>');});
 await new Promise(r=>server.listen(0,'127.0.0.1',r));
 const browser=await chromium.launch();
 try{
  const page=await browser.newPage();page.on('console',m=>console.log('Chromium:',m.text()));page.on('pageerror',e=>console.error(e));
  await page.goto('http://127.0.0.1:'+server.address().port);
  console.log('Isolated test room:',room);
  const child=spawn(path.join(root,'build/ios/probe/webrtc-probe'),[root,room],{stdio:'inherit'});
  const result=await new Promise(r=>child.on('exit',r));
  if(result!==0)process.exitCode=1;
 }finally{await browser.close();server.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
