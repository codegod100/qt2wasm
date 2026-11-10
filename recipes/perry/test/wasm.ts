// (module (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))
const bytes = new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11]);
const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
console.log("add:", (inst.exports as any).add(2, 40));
