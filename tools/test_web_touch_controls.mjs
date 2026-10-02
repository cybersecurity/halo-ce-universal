import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
const input=readFileSync(new URL('../port/web/site/input.js',import.meta.url),'utf8');
function fixture(){
  class Element {
    constructor(){this.children=[];this.listeners={};this.style={};this.dataset={};this.classList={add(){},remove(){},toggle(){}};}
    appendChild(e){this.children.push(e);}
    setAttribute(){}
    addEventListener(k,f){(this.listeners[k]??=[]).push(f);}
    emit(k,touches=[]){for(const f of this.listeners[k]||[])f({type:k,changedTouches:touches,preventDefault(){}});}
    closest(){return this.dataset.control?this:null;}
  }
  const root=new Element(),win=new Element(),doc=new Element(),pad={axes:[0,0,0,0,0,0],buttons:0},events=[];
  win.innerWidth=800;win.HaloResumeAudio=()=>{};
  doc.createElement=()=>new Element();let target=root;doc.elementFromPoint=()=>target;
  let now=1000;const timers=new Map();let tid=0;
  const body=input.slice(input.indexOf('  function buildCompactTouchControls'), input.indexOf('  function buildTouchControls'));
  const buttons="{\n    SOUTH: 0, EAST: 1, WEST: 2, NORTH: 3, BACK: 4, GUIDE: 5, START: 6, LEFT_STICK: 7, RIGHT_STICK: 8,\n    LEFT_SHOULDER: 9, RIGHT_SHOULDER: 10, DPAD_UP: 11, DPAD_DOWN: 12, DPAD_LEFT: 13, DPAD_RIGHT: 14,\n  }";
  vm.runInNewContext(`const BUTTON=${buttons};\n${body}\nbuildCompactTouchControls(root);`,{Date:{now:()=>now},setTimeout:f=>{timers.set(++tid,f);return tid;},clearTimeout:id=>timers.delete(id),root,window:win,document:doc,touchPad:pad,touchUsed:false,lookSensitivity:1,axis:v=>Math.round(v*32767),pushEvent:(...e)=>events.push(e),EVENT:{FOCUS:5,MOUSE_MOTION:2}});
  return {root,win,doc,pad,events,tick(){now+=100;for(const f of timers.values())f();timers.clear();},target(id){target=id?root.children.find(e=>e.dataset.control===id):root;}};
}
test('movement and firing/aiming work together, cancellation and backgrounding release input',()=>{
  const f=fixture(),touch=(identifier,clientX,clientY)=>({identifier,clientX,clientY});
  f.root.emit('touchstart',[touch(1,120,180)]);
  f.root.emit('touchmove',[touch(1,145,160)]);
  assert.ok(f.pad.axes[0]>0&&f.pad.axes[1]<0);
  f.target('fire');f.root.emit('touchstart',[touch(2,680,160)]);
  assert.equal(f.pad.axes[5],32767);
  f.root.emit('touchmove',[touch(2,695,170)]);
  assert.deepEqual(f.events.at(-1),[2,0,0,0,0,30,20]);
  f.root.emit('touchcancel',[touch(2,695,170)]);assert.equal(f.pad.axes[5],0);
  f.target('a');f.root.emit('touchstart',[touch(3,650,250)]);assert.equal(f.pad.buttons,1);
  f.doc.hidden=true;f.doc.emit('visibilitychange');
  assert.equal(f.pad.buttons,0);assert.ok(f.pad.axes.every(v=>v===0));
  f.target('fire');f.root.emit('touchstart',[touch(4,680,160)]);f.win.emit('blur');
  assert.equal(f.pad.axes[5],0);
});

test('compact HUD omits controller navigation and supports tap or hold fire',()=>{
  const f=fixture(),t={identifier:7,clientX:680,clientY:160};
  const ids=f.root.children.map(e=>e.dataset.control).filter(Boolean);
  for(const id of ['start','back','up','down','left','right','white']) assert.ok(!ids.includes(id));
  assert.equal(ids.length,9);
  f.target('fire');f.root.emit('touchstart',[t]);f.root.emit('touchend',[t]);
  assert.equal(f.pad.axes[5],0);f.tick();assert.equal(f.pad.axes[5],0);
  f.root.emit('touchstart',[t]);f.tick();assert.equal(f.pad.axes[5],32767);
  f.root.emit('touchend',[t]);assert.equal(f.pad.axes[5],0);
});

test('invisible weapon HUD target swaps without activating movement or aiming',()=>{
  const f=fixture(),t={identifier:8,clientX:95,clientY:35};
  const target=f.root.children.find(e=>e.dataset.control==='y');
  assert.match(target.className,/fps-weapon-hud/);
  assert.equal(target.children.length,0);
  f.target('y');f.root.emit('touchstart',[t]);
  assert.equal(f.pad.buttons,1<<3);
  f.root.emit('touchmove',[{...t,clientX:100}]);
  assert.ok(f.pad.axes.every(v=>v===0));
  assert.ok(!f.events.some(e=>e[0]===2));
  f.root.emit('touchend',[t]);assert.equal(f.pad.buttons,0);
});

test('compact layout is opt-in and the controller layout remains available',()=>{
  const app=readFileSync(new URL('../port/web/site/app.js',import.meta.url),'utf8');
  const settings=app.slice(app.indexOf('  const coarsePointer'), app.indexOf('  function saveSettings'));
  for(const saved of [{}, {touch:true}, {touchFps:true}]) {
    const actual=vm.runInNewContext(settings+'\nsettings.touchFps',{
      ios:false,pixelFrames:false,matchMedia:()=>({matches:true}),localStorage:{getItem:()=>JSON.stringify(saved)}
    });
    assert.equal(actual,saved.touchFps===true);
  }
  assert.match(input,/touchFps = false/);
  assert.match(input,/if \(touchFps\) buildCompactTouchControls\(touchRoot\);\s*else buildTouchControls\(touchRoot\)/);
});
