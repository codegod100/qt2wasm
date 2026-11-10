// A native GTK4 window (Perry perry/ui) driving the Qt Counter QObject: the
// qt2wasm component runs as-is on wasmtime (perry/component, JIT, WASI p2),
// embedded in the binary.
import { App, VStack, HStack, Text, Button, State } from "perry/ui";
import wasmPath from "./counter.wasm" with { type: "file" };
import { loadCounter, Counter, onValueChanged } from "./counter";

loadCounter(wasmPath);
const c = new Counter();
const label = State(c.describe("n="));
const hist = State("history: []");
const sig = State("no signals yet");
let signals = 0;
onValueChanged((src: number, v: number) => { signals++; sig.set(`value-changed #${signals}: source=${src} value=${v}`); });
function refresh(): void {
  label.set(c.describe("n="));
  hist.set("history: " + JSON.stringify(c.history(8)));
}

App({
  title: "Qt Counter on GTK4 (Perry + wasmtime)",
  width: 480,
  height: 260,
  body: VStack(14, [
    Text("Qt QObject (wasm component on wasmtime) in a Perry GTK4 app"),
    Text(`${label.value}`),
    Text(`${hist.value}`),
    Text(`${sig.value}`),
    HStack(10, [
      Button("Step", () => { c.step(); refresh(); }),
      Button("Linear", () => { c.setMode("linear"); refresh(); }),
      Button("Doubling", () => { c.setMode("doubling"); refresh(); }),
      Button("Reset", () => { c.setValue(0); refresh(); }),
    ]),
  ]),
});
