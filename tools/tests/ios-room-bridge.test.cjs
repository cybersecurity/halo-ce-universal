const {test}=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const fs=require('node:fs');
const path=require('node:path');
function fixture() {
  let callbacks, reports=[];
  const net={address:0x0302010a, attach(){}, async join(){}, async quickPlay(c){callbacks=c;return {role:'join',hostAddress:123,epoch:7};},
    quickPlayStarted(){}, quickPlayCheckpoint(v){reports.push(v);}, quickPlayLost(){return false;}, quickPlayPhase(v){reports.push(v);}, async leave(){}};
  const ctx=vm.createContext({HaloNet:net,btoa:s=>Buffer.from(s,'binary').toString('base64'),atob:s=>Buffer.from(s,'base64').toString('binary')});
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../../port/ios/network/room.js'),'utf8')+'\nglobalThis.api=HaloNative;',ctx);
  const api=ctx.api;
  return {api, words:new Int32Array(api.memory.buffer), bytes:new Uint8Array(api.memory.buffer), callbacks:()=>callbacks, reports};
}
function frame() {
  const b=Buffer.alloc(32);b.writeUInt32LE(32,0);b.writeUInt32LE(1,4);b.writeUInt32LE(5,20);b.write('hello',24);return b;
}
const plain=v=>JSON.parse(JSON.stringify(v));
test('native send ring wraps counters and defers whole batches under backpressure',()=>{
  const {api,words,bytes}=fixture(), packet=frame();
  words[1]=words[2]=-16;
  assert.equal(api.exchange({out:packet.toString('base64')}).sent,32);
  assert.equal(words[1],16);
  const actual=Buffer.alloc(32);
  for(let i=0;i<32;i++)actual[i]=bytes[api.offsets.netOut+((0xfffffff0+i)&1048575)];
  assert.deepEqual(actual,packet);
  words[1]=words[2]+1048576;
  assert.equal(api.exchange({out:packet.toString('base64')}).sent,0);
});
test('receive batch repeats until acknowledged, then releases ring space exactly once',()=>{
  const {api,words,bytes}=fixture(), packet=frame();
  words[4]=-16;words[3]=16;
  for(let i=0;i<32;i++)bytes[api.offsets.netIn+((0xfffffff0+i)&2097151)]=packet[i];
  const first=api.exchange({}).input;
  assert.deepEqual(Buffer.from(first.data,'base64'),packet);
  assert.deepEqual(plain(api.exchange({ack:first.id+1}).input),plain(first));
  assert.equal(words[4],-16);
  assert.equal(api.exchange({ack:first.id}).input.data,'');
  assert.equal(words[4],16);
  assert.equal(api.exchange({ack:first.id}).input.data,'');
});
test('malformed batches never advance outgoing producer',()=>{
  const {api,words}=fixture();
  assert.throws(()=>api.exchange({out:Buffer.alloc(23).toString('base64')}),/Truncated/);
  const b=frame();b.writeUInt32LE(200,20);
  assert.throws(()=>api.exchange({out:b.toString('base64')}),/Invalid packet/);
  assert.equal(words[1],0);
});
test('room selection, hold, reconnect, migration, reports and cancellation preserve order',async()=>{
  const {api,callbacks,reports}=fixture();await api.join({room:'TEST'});
  assert.deepEqual(plain(api.exchange({}).commands),[[1,0x0302010a,7,0]]);
  callbacks().onStatus({hold:true});callbacks().onReconnect({hostAddress:123,epoch:8});
  callbacks().onFailover({role:'host',hostAddress:456,epoch:9});callbacks().onStatus({hold:false});
  assert.deepEqual(plain(api.exchange({reports:[{phase:'checkpoint',tick:5},{phase:'playing'}]}).commands),
    [[2,1,0,0],[4,123,8,0],[3,1,456,9],[2,0,0,0]]);
  assert.deepEqual(plain(reports),[{phase:'checkpoint',tick:5},'playing']);
  await api.leave();assert.deepEqual(plain(api.exchange({}).commands),[[5,0,0,0]]);
});
test('ping snapshot preserves signed unavailable values and network address words',()=>{
  const {api,words}=fixture();words[6]=0x0a010203;words[7]=4;words[8]=-10;words[9]=2;
  words[10]=0x0a010204;words[11]=37;words[12]=0x0a010205;words[13]=-1;
  assert.deepEqual(plain(api.exchange({}).pings),{host:0x0a010203,epoch:4,updated:4294967286,rows:[[0x0a010204,37],[0x0a010205,-1]]});
});
