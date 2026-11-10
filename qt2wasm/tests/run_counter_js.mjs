// Drive the Counter component's JS bindings (qt2wasm build -> counter-js/,
// jco transpile + load()) under Node: the same checks as run_counter.py
// under wasmtime-py. Usage: node run_counter_js.mjs path/to/counter-js
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const dir = resolve(process.argv[2] ?? 'counter-js');
const { load } = await import(pathToFileURL(`${dir}/index.js`).href);
const events = [];
const { counter } = await load({
  'demo:counter/counter-events': {
    valueChanged: (source, value) => events.push(['value-changed', source, value]),
    overflowed: (source, why, at) => events.push(['overflowed', source, why, at]),
  },
});
const c = new counter.Counter();
assert.equal(c.id(), 1);
c.setValue(5);
c.step();
c.setMode('doubling');
c.step();
assert.equal(c.value(), 12);
assert.equal(c.describe('n='), 'n=12 (doubling)');
assert.deepEqual([...c.history(2)], [6, 12]);
c.setValue(0x40000000);
c.step();
assert.deepEqual(events, [
  ['value-changed', 1, 5],
  ['value-changed', 1, 6],
  ['value-changed', 1, 12],
  ['value-changed', 1, 1073741824],
  ['overflowed', 1, 'past INT_MAX', 2147483648n],
]);
console.log(`counter JS bindings OK: ${events.length} signals received`);
