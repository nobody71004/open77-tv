import puppeteer from "puppeteer-core";
const ip = process.argv[2] || "172.17.0.2";
const b = await puppeteer.connect({ browserURL: `http://${ip}:9222` });
const pages = (await Promise.all((await b.targets()).filter(t => t.type() === "page").map(t => t.page()))).filter(Boolean);
const page = pages[pages.length - 1];
console.log("page:", page.url());
const v = await page.evaluate(process.argv[3]);
console.log(JSON.stringify(v, null, 2));
await b.disconnect();
