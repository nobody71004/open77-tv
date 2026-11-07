import http from "node:http";
import fs from "node:fs";
import WebSocket from "ws";
const [ip, out] = process.argv.slice(2);
const list = await new Promise((res, rej) => http.get(`http://${ip}:9222/json/list`, r => { let b=""; r.on("data",d=>b+=d); r.on("end",()=>res(JSON.parse(b))); }).on("error", rej));
const page = list.filter(t => t.type === "page" && /^http/.test(t.url)).pop();
if (!page) { console.log("no page"); process.exit(1); }
console.log("page:", page.url);
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise(r => ws.on("open", r));
ws.send(JSON.stringify({ id: 1, method: "Page.captureScreenshot", params: { format: "png" } }));
const shot = await new Promise((res, rej) => {
  ws.on("message", raw => { const m = JSON.parse(String(raw)); if (m.id === 1) m.error ? rej(new Error(m.error.message)) : res(m.result.data); });
  ws.on("error", rej);
});
fs.writeFileSync(out, Buffer.from(shot, "base64"));
console.log("wrote", out);
ws.close();
