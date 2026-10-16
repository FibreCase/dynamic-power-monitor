const fs=require('fs'),vm=require('vm'),assert=require('assert');
const elements=new Map();
function element(id){if(!elements.has(id))elements.set(id,{value:'',textContent:'',disabled:false,children:[],events:{},classList:{toggle(){}},setAttribute(){},replaceChildren(){this.children=[];},append(el){this.children.push(el);},addEventListener(ev,fn){this.events[ev]=fn;}});return elements.get(id);}
const packet=new DataView(new ArrayBuffer(28));packet.setUint8(0,1);packet.setUint8(1,4);packet.setUint8(2,0);packet.setFloat32(4,25.5,true);packet.setFloat32(8,43.2,true);packet.setFloat32(12,27.5,true);packet.setFloat32(16,40.4,true);packet.setUint16(20,650,true);packet.setUint16(22,12,true);packet.setUint8(24,2);
let writes=[],active=0,requests=0,failRead=false;
const state={async readValue(){if(failRead)throw Error("timeout");assert.equal(active++,0);await Promise.resolve();active--;return new DataView(packet.buffer.slice(0));}};
const mode={async writeValueWithResponse(data){assert.equal(active++,0);await Promise.resolve();writes.push(data[0]);packet.setUint8(2,data[0]);active--;}};
const device={name:'5V-test',events:{},addEventListener(ev,fn){this.events[ev]=fn;},gatt:{connected:false,async connect(){this.connected=true;return this;},async getPrimaryService(){return{async getCharacteristic(id){return id.startsWith('5e7a0002')?state:mode;}};},disconnect(){this.connected=false;device.events.gattserverdisconnected?.();}}};
const context=vm.createContext({console,DataView,Uint8Array,window:{isSecureContext:true,addEventListener(){}},navigator:{bluetooth:{async requestDevice(opts){requests++;assert(opts.filters[0].services[0].startsWith('5e7a0001'));return device;}}},document:{hidden:false,getElementById:element,querySelector(){return element('labels');},querySelectorAll(){return element('labels').children;},createElement(){return element(Symbol());}},setInterval(){}});
const html=fs.readFileSync('web/ble.html','utf8');vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1],context);
(async()=>{
assert.equal(element('slider').disabled,true);assert.equal(requests,0);
await element('connect').events.click();assert.equal(requests,1);assert.equal(element('power').textContent,'PWR');assert.equal(element('sensor-temperature').textContent,'25.5');assert.equal(element('sensor-co2').textContent,'650');assert.equal(element('slider').max,'3');assert.equal(element('slider').disabled,false);
element('slider').value='1';await element('slider').events.change();assert.equal(writes.at(-1),2);assert.equal(element('mode').textContent,'BL');
packet.setUint8(1,6);packet.setUint8(2,0);await vm.runInContext('poll()',context);assert.equal(element('power').textContent,'ECO');assert.equal(element('slider').max,'2');assert(!element('labels').children.some(el=>el.textContent==='BL'));assert(element('detail').textContent.includes('CTL2 关闭'));assert(element('sensor-status').textContent.includes('10 秒'));
element('slider').value='1';await Promise.all([vm.runInContext('poll()',context),element('slider').events.change()]);assert.equal(writes.at(-1),1);assert.equal(element('mode').textContent,'ON');
failRead=true;await vm.runInContext('poll()',context);assert.equal(element('slider').disabled,true);assert.equal(element('power').textContent,'待同步');failRead=false;await vm.runInContext('poll()',context);assert.equal(element('slider').disabled,false);
packet.setUint8(1,14);await vm.runInContext('poll()',context);assert.equal(element('sensor-temperature').textContent,'--');
packet.setUint8(1,7);await vm.runInContext('poll()',context);assert.equal(element('slider').disabled,true);
await element('disconnect').events.click();assert.equal(element('power').textContent,'未连接');assert.equal(element('slider').disabled,true);assert.equal(element('sensor-co2').textContent,'--');
await element('connect').events.click();assert.equal(requests,2);assert.equal(element('power').textContent,'Loading…');
console.log('BLE web: connect, environment, PWR/ECO modes, serialized GATT, loading and reconnect passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
