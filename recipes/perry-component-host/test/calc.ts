// A real component on wasmtime (perry/component) from a native Perry binary,
// through the perry-wit bindings, with a TypeScript host import.
import { readFileSync } from "node:fs";
import { load } from "./gen/bindings";

const c = load(readFileSync(process.argv[2]), {
  "test:demo/host@1.0.0": { double: (x: number): number => x * 2 },
});
console.log("add: " + c.exports["test:demo/calc@1.0.0"].add(20, 1));
c.dispose();
