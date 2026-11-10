import { createServer } from "node:http";
const s = createServer((_q, r) => { r.end("ok"); });
console.log(typeof s.listen);
