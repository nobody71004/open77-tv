// open77-paste.js in headless Chromium against a stand-in for the shared
// browser's client page: the transparent text field over the picture with a key
// handler that, like the client's, takes every key and cancels it, and the
// client's socket to /ws (answered here by Playwright). What it pins: Ctrl+V on
// the picture sends this PC's clipboard as control/clipboard and then plays
// Ctrl+V through the client's handler (pressing Ctrl only when the player has
// let go of it); the trusted V never reaches the client; nothing lands in the
// hidden field; Shift+Insert works the same; a paste anywhere else, with no
// socket, or not made by the player, is left alone; the player is shown what
// happened on the picture (a right-click is told to use Ctrl+V), and the
// television framing the page is told too, without the text.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
let chromium;
try {
  ({ chromium } = await import("playwright"));
} catch (error) {
  console.error("playwright is not installed: npm i -D playwright && npx playwright install chromium");
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
// usage: node <this file> [path to the script]   (default: ../build/open77-paste.js)
const script = fs.readFileSync(process.argv[2] || path.join(here, "..", "build", "open77-paste.js"), "utf8");

const NOTE = () => page.evaluate(() => { const n = [...document.querySelectorAll("div")].find((d) => d.style.position === "fixed"); return n && n.isConnected ? n.textContent : null; });
const page_html = `<!doctype html><head><script>window.__open77PasteSettleMs = 60;</script><script src="open77-paste.js"></script></head>
<body><textarea class="overlay" id="overlay"></textarea><textarea id="other"></textarea><script>
  window.__keys = [];
  const overlay = document.getElementById("overlay");
  const record = (type) => (e) => { __keys.push([type, e.key, e.keyCode, e.ctrlKey, e.isTrusted]); e.preventDefault(); };
  overlay.addEventListener("keydown", record("down"));
  overlay.addEventListener("keyup", record("up"));
  window.__open = (path) => { window.__ws = new WebSocket("ws://" + location.host + path); return new Promise((r) => { __ws.onopen = () => r(true); __ws.onerror = () => r(false); }); };
</script></body>`;
const server = http.createServer((req, res) => {
  if (req.url.startsWith("/open77-paste.js")) { res.writeHead(200, { "content-type": "text/javascript" }); return res.end(script); }
  res.writeHead(200, { "content-type": "text/html" }); res.end(page_html);
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));
const base = "http://127.0.0.1:" + server.address().port;

const browser = await chromium.launch();
const context = await browser.newContext();
await context.grantPermissions(["clipboard-read", "clipboard-write"], { origin: base });
let failures = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) failures++; };

async function fresh(path) {
  const page = await context.newPage();
  const sent = [];
  await page.routeWebSocket(/\/ws(\?|$)|\/elsewhere/, (ws) => { ws.onMessage((m) => sent.push(String(m))); });
  await page.goto(base + "/");
  if (path) check(await page.evaluate((p) => __open(p), path), "the client's socket opens (" + path + ")");
  return { page, sent };
}
const keys = (page) => page.evaluate(() => __keys.map((k) => k.join(",")));
const LINK = "https://example.com/pasted?x=1&y=two";

// 1. Ctrl+V, Ctrl let go at once: the clipboard goes first, then Ctrl+V is played.
let { page, sent } = await fresh("/tv/ws?password=p&username=u");
await page.evaluate((t) => navigator.clipboard.writeText(t), LINK);
await page.focus("#overlay");
await page.keyboard.down("Control"); await page.keyboard.press("v"); await page.keyboard.up("Control");
await page.waitForTimeout(300);
let k = await keys(page);
console.log("   keys: " + JSON.stringify(k));
check(sent.length === 1 && JSON.parse(sent[0]).event === "control/clipboard" && JSON.parse(sent[0]).text === LINK,
  "this PC's clipboard goes to the server as control/clipboard: " + JSON.stringify(sent));
check(!k.some((x) => x === "down,v,86,true,true"), "the player's own V never reaches the client's key handler");
check(k.join("|").includes("down,Control,17,true,false|down,v,86,true,false|up,v,86,true,false|up,Control,17,false,false"),
  "Ctrl+V is then played through it, Ctrl pressed and released because the player let go");
check(k.indexOf("down,Control,17,true,true") === 0, "the player's Ctrl itself went to the client as usual");
check(await page.inputValue("#overlay") === "", "nothing is pasted into the hidden field");
check(await NOTE() === "Pasted from this PC", "the player is shown that it was pasted: " + await NOTE());
await page.close();

// 2. Ctrl still held when the paste is played: only V is played.
({ page, sent } = await fresh("/tv/ws"));
await page.evaluate((t) => navigator.clipboard.writeText(t), "second paste");
await page.focus("#overlay");
await page.keyboard.down("Control"); await page.keyboard.press("v");
await page.waitForTimeout(250);
await page.keyboard.up("Control");
k = await keys(page);
console.log("   keys: " + JSON.stringify(k));
check(sent.length === 1 && JSON.parse(sent[0]).text === "second paste", "sent while Ctrl is held");
check(k.join("|") === "down,Control,17,true,true|down,v,86,true,false|up,v,86,true,false|up,Control,17,false,true",
  "with Ctrl held only V is played, and the player's own release lets go of Ctrl");
await page.close();

// 3. Shift+Insert pastes the same way.
({ page, sent } = await fresh("/tv/ws"));
await page.evaluate((t) => navigator.clipboard.writeText(t), "via insert");
await page.focus("#overlay");
await page.keyboard.down("Shift"); await page.keyboard.press("Insert"); await page.keyboard.up("Shift");
await page.waitForTimeout(250);
k = await keys(page);
check(sent.length === 1 && JSON.parse(sent[0]).text === "via insert", "Shift+Insert sends the clipboard too: " + JSON.stringify(sent));
check(k.join("|").includes("down,v,86,true,false|up,v,86,true,false"), "and plays Ctrl+V");
check(!k.some((x) => x.startsWith("down,Insert") && x.endsWith(",true")), "the player's Insert never reaches the client");
await page.close();

// 4. A paste anywhere but the picture is the page's own business.
({ page, sent } = await fresh("/tv/ws"));
await page.evaluate((t) => navigator.clipboard.writeText(t), "into the other field");
await page.focus("#other");
await page.keyboard.press("Control+V");
await page.waitForTimeout(250);
check(sent.length === 0, "a paste in another field sends nothing");
check(await page.inputValue("#other") === "into the other field", "and lands where it was made");
await page.close();

// 5. No client socket: Ctrl+V is left to the client as before.
({ page, sent } = await fresh(null));
await page.evaluate((t) => navigator.clipboard.writeText(t), "no socket");
await page.focus("#overlay");
await page.keyboard.press("Control+V");
await page.waitForTimeout(250);
k = await keys(page);
check(sent.length === 0 && k.some((x) => /^down,[vV],86,true,true$/.test(x)), "with no socket the client's handler gets Ctrl+V as before: " + JSON.stringify(k));
await page.close();

// 6. A socket to anywhere else is not the client's.
({ page, sent } = await fresh("/elsewhere"));
await page.evaluate((t) => navigator.clipboard.writeText(t), "wrong socket");
await page.focus("#overlay");
await page.keyboard.press("Control+V");
await page.waitForTimeout(250);
check(sent.length === 0, "nothing goes down a socket that is not the client's /ws");
await page.close();

// 6b. An empty clipboard: nothing is sent, and the player is told.
({ page, sent } = await fresh("/tv/ws"));
await page.evaluate(() => navigator.clipboard.writeText(""));
await page.focus("#overlay");
await page.keyboard.press("Control+V");
await page.waitForTimeout(250);
k = await keys(page);
check(sent.length === 0 && !k.some((x) => /^down,[vV],86,true,false$/.test(x)), "an empty clipboard sends nothing and plays nothing");
check(/Nothing to paste/.test(String(await NOTE())), "and says there is nothing to paste: " + await NOTE());
await page.close();

// 6c. A right-click on the picture opens the server browser's menu: the player is told to use Ctrl+V.
({ page, sent } = await fresh("/tv/ws"));
const box = await page.locator("#overlay").boundingBox();
await page.mouse.click(box.x + 5, box.y + 5, { button: "right" });
await page.waitForTimeout(100);
check(/Ctrl\+V/.test(String(await NOTE())), "a right-click on the picture says to use Ctrl+V: " + await NOTE());
await page.close();

// 7. A paste the player did not make (a script's own event) sends nothing.
({ page, sent } = await fresh("/tv/ws"));
await page.evaluate(() => {
  const data = new DataTransfer(); data.setData("text/plain", "forged");
  document.getElementById("overlay").dispatchEvent(new ClipboardEvent("paste", { clipboardData: data, bubbles: true }));
});
await page.waitForTimeout(250);
check(sent.length === 0, "a scripted paste event sends nothing");
await page.close();

// 8. Framed, as a television frames it: the television is told what happened, without the text.
const tvServer = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/html" });
  res.end('<!doctype html><body><iframe id="tv" style="width:600px;height:300px"></iframe><script>window.__msgs=[];' +
    'addEventListener("message",function(e){var d=e.data;if(d&&d.open77SharedBrowser===1)__msgs.push(d.kind+" "+d.text)});</script></body>');
}).listen(0, "localhost");
await new Promise((r) => tvServer.once("listening", r));
const tvOrigin = "http://localhost:" + tvServer.address().port;
await context.grantPermissions(["clipboard-read", "clipboard-write"], { origin: tvOrigin });
page = await context.newPage();
sent = [];
await page.routeWebSocket(/\/ws(\?|$)/, (ws) => { ws.onMessage((m) => sent.push(String(m))); });
await page.goto(tvOrigin + "/");
await page.evaluate((src) => { document.getElementById("tv").src = src; }, base + "/");
await page.waitForTimeout(500);
const frame = page.frames().find((f) => f !== page.mainFrame());
await frame.evaluate(() => __open("/tv/ws"));
await page.evaluate((t) => navigator.clipboard.writeText(t), "https://framed.example/link");
const fb = await page.locator("#tv").boundingBox();
await page.mouse.click(fb.x + 20, fb.y + 20);                 // into the picture (the overlay is the first field)
await page.keyboard.down("Control"); await page.keyboard.press("v"); await page.keyboard.up("Control");
await page.waitForTimeout(300);
await page.mouse.click(fb.x + 20, fb.y + 20, { button: "right" });
await page.waitForTimeout(200);
const msgs = await page.evaluate(() => __msgs);
console.log("   television heard: " + JSON.stringify(msgs));
check(sent.length === 1 && JSON.parse(sent[0]).text === "https://framed.example/link", "framed: the paste is sent");
check(msgs.some((m) => /^paste Ctrl\+V: 27 characters sent/.test(m)), "the television hears how many characters were pasted");
check(msgs.some((m) => /^paste right-click on the picture/.test(m)), "and that a right-click was told to use Ctrl+V");
check(!msgs.some((m) => m.includes("framed.example")), "never the text itself");
await page.close();
tvServer.close();

await browser.close();
server.close();
console.log(failures === 0 ? "ALL PASSED" : failures + " FAILED");
process.exit(failures === 0 ? 0 : 1);
