// Smoke test: drive the Qt Counter component on wasmtime from a native Perry binary.
//   ./counter-cli counter.wasm
import { loadCounter, disposeCounter, Counter, onValueChanged } from "./counter";

loadCounter(process.argv[2]);
const signals: string[] = [];
onValueChanged((src: number, v: number) => { signals.push(`${src}:${v}`); });
const c = new Counter();
c.step(); c.step();
c.setMode("doubling");
c.step(); c.step();
console.log(c.describe("n="));
console.log("history " + JSON.stringify(c.history(10)));
console.log("signals " + signals.join(","));
c.dispose();
disposeCounter();
