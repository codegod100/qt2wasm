// The qt2wasm Counter component on wasmtime (perry/component), wrapped in a
// small class over the perry-wit bindings in gen/ (see build.sh).
import { readFileSync } from "node:fs";
import { load } from "./gen/bindings";

export type Mode = "linear" | "doubling";
const IFACE = "demo:counter/counter@0.1.0";
const valueChanged: Array<(source: number, value: number) => void> = [];
const overflowed: Array<(source: number, why: string, at: bigint) => void> = [];
let api: any = null;
let component: any = null;

export function onValueChanged(f: (source: number, value: number) => void): void { valueChanged.push(f); }
export function onOverflowed(f: (source: number, why: string, at: bigint) => void): void { overflowed.push(f); }

/** Instantiate the component. Qt's start-up needs more than the default fuel. */
export function loadCounter(path: string): void {
  component = load(readFileSync(path), {
    "demo:counter/counter-events@0.1.0": {
      "value-changed": (source: number, value: number): void => { for (const f of valueChanged) f(source, value); },
      "overflowed": (source: number, why: string, at: bigint): void => { for (const f of overflowed) f(source, why, at); },
    },
  }, {
    memoryBytes: 512 * 1024 * 1024,
    fuel: 1000000000000,
    wasi: { inheritStdout: true, inheritStderr: true },
  });
  api = component.exports[IFACE];
}

export function disposeCounter(): void { if (component) component.dispose(); component = null; }

export class Counter {
  r: any;
  constructor() { this.r = api["[constructor]counter"](); }
  id(): number { return api["[method]counter.id"](this.r.borrow()); }
  value(): number { return api["[method]counter.value"](this.r.borrow()); }
  setValue(v: number): void { api["[method]counter.set-value"](this.r.borrow(), v); }
  setMode(m: Mode): void { api["[method]counter.set-mode"](this.r.borrow(), m); }
  step(): void { api["[method]counter.step"](this.r.borrow()); }
  describe(prefix: string): string { return api["[method]counter.describe"](this.r.borrow(), prefix); }
  history(n: number): number[] {
    const h = api["[method]counter.history"](this.r.borrow(), n);
    const out: number[] = [];
    for (let i = 0; i < h.length; i++) out.push(h[i]);
    return out;
  }
  dispose(): void { this.r.dispose(); }
}
