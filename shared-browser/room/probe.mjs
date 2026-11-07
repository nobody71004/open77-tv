import puppeteer from "puppeteer-core";
const ip = process.argv[2];
const b = await puppeteer.connect({ browserURL: `http://${ip}:9222`, defaultViewport: { width: 1280, height: 720 } });
console.log("connected; pages:", (await b.pages()).length, "targets:", (await b.targets()).length);
for (const t of await b.targets()) console.log("target:", t.type(), t.url());
const page = (await b.pages())[0] || (await Promise.all((await b.targets()).map(t => t.page()))).filter(Boolean)[0];
if (!page) { console.log("no page handle"); process.exit(1); }
await page.bringToFront().catch(()=>{});
await new Promise(r => setTimeout(r, 1500));
console.log("page url:", page.url(), "title:", await page.title());
await page.screenshot({ path: "/tmp/room-splash.png" });
console.log("shot ok");
await b.disconnect();
