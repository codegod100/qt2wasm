// Drive the Canvas component's JS bindings (QtGui: QImage/QPainter, embedded
// font) under Node: records, enums, flags, option and list<u8> across the JS
// boundary. Usage: node run_canvas_js.mjs path/to/canvas-js
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const dir = resolve(process.argv[2] ?? 'canvas-js');
const { load } = await import(pathToFileURL(`${dir}/index.js`).href);
const painted = [];
const { canvas } = await load({
  'demo:canvas/canvas-events': { painted: (source, dirty) => painted.push([source, dirty]) },
});
const rgba = (r, g, b, a = 255) => ({ r, g, b, a });
const c = new canvas.Canvas(128, 96);
assert.deepEqual(c.size(), { width: 128, height: 96 });
assert.ok(c.fontFamilies().length >= 1);
c.clear(rgba(18, 18, 30));
c.shape('ellipse', { x: 24, y: 20, width: 80, height: 80 }, rgba(220, 60, 60), { color: rgba(255, 255, 255), width: 2 });
const box = c.text({ x: 10, y: 40 }, 'Qt', 20, rgba(255, 255, 255), { bold: true });
assert.ok(box.width > 0 && box.height > 0, JSON.stringify(box));
const centre = c.pixel({ x: 64, y: 60 });
assert.ok(centre && centre.r > 150, JSON.stringify(centre));
assert.equal(c.pixel({ x: 999, y: 0 }), undefined);
const img = c.image();
assert.equal(img.width, 128);
assert.equal(img.pixels.length, 128 * 96 * 4);
assert.ok(painted.length >= 3, `painted ${painted.length}`);
console.log(`canvas JS bindings OK: ${painted.length} painted signals, ${img.pixels.length} bytes`);
