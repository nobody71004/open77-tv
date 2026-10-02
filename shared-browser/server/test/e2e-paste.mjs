// Pasting into the shared browser, end to end: the client page framed the way a
// television frames it, a link written to this browser's clipboard, a click on
// the shared browser's address bar, Ctrl+V and Enter -- then the picture is
// photographed (/out/paste-*.png) and the server's clipboard read by the caller.
// Afterwards the shared browser is sent back to its home page the same way.
// Run ON the VPS in Playwright's image; the link (viewer password) is read from
// /secret and never printed.
import fs from "node:fs";
import http from "node:http";
import { chromium } from "playwright";
const link = fs.readFileSync("/secret", "utf8").trim().replace(/#open77-shared-browser$/, "");
const TEST_URL = "https://example.com/?open77-paste-e2e";
const server = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/html" });
  res.end('<!doctype html><body style="margin:0;background:#000"><iframe id="tv" style="border:0;width:1280px;height:720px" ' +
    'allow="autoplay; fullscreen; clipboard-read; clipboard-write" referrerpolicy="no-referrer"></iframe><script>window.__msgs=[];' +
    'addEventListener("message",function(e){var d=e.data;if(d&&d.open77SharedBrowser===1)__msgs.push(d.kind+" ["+d.mode+"] "+d.text)});</script></body>');
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));
const origin = "http://127.0.0.1:" + server.address().port;
const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const context = await browser.newContext({ viewport: { width: 1280, height: 720 } });
await context.grantPermissions(["clipboard-read", "clipboard-write"], { origin });
const page = await context.newPage();
const clipboardFrames = [];
page.on("websocket", (ws) => ws.on("framesent", (f) => {
  const text = String(f.payload);
  if (text.includes("control/clipboard")) clipboardFrames.push(text);
}));
await page.goto(origin + "/");
const u = new URL(link); u.searchParams.set("open77", "2");
await page.evaluate((src) => { document.getElementById("tv").src = src; }, u.toString());
for (let i = 0; i < 30; i++) {
  await page.waitForTimeout(1000);
  if ((await page.evaluate(() => __msgs.slice())).some((m) => /^connected/.test(m))) break;
}
await page.waitForTimeout(2500);

async function pasteInto(url) {
  await page.mouse.click(640, 400);                     // take the browser (implicit hosting)
  await page.waitForTimeout(500);
  await page.mouse.click(400, 62);                      // its address bar
  await page.waitForTimeout(700);
  await page.evaluate((t) => navigator.clipboard.writeText(t), url);
  await page.keyboard.down("Control"); await page.keyboard.press("v"); await page.keyboard.up("Control");
  await page.waitForTimeout(1200);
}
await pasteInto(TEST_URL);
await page.screenshot({ path: "/out/paste-1-pasted.png" });
await page.keyboard.press("Enter");
await page.waitForTimeout(4000);
await page.screenshot({ path: "/out/paste-2-opened.png" });
console.log(JSON.stringify({ ice: (await page.evaluate(() => __msgs)).filter((m) => /^(connected|gave_up|retry)/.test(m)).map((m) => m.slice(0, 160)),
  clipboardFrames }, null, 1));
await pasteInto("https://www.youtube.com/");
await page.keyboard.press("Enter");
await page.waitForTimeout(3000);
await page.screenshot({ path: "/out/paste-3-home.png" });
await browser.close();
process.exit(0);
