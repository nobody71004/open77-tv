// open77 headless screen-mirror agent (Electron main process).
//
// Runs on the work PC with NO visible browser: a hidden window captures the
// screen and streams it over WebRTC through the relay's signaling, exactly like
// sender.html but always-on. A tray icon is the only UI (status + quit). It
// starts at logon via Electron's auto-launch (per-user, no admin needed).
//
// Config: agent/config.json (same folder), created from config.example.json:
//   { "channel": "WORK", "token": "...", "relay": "wss://xbuniverse.duckdns.org/screen-mirror/ws",
//     "fps": 60, "autoLaunch": true, "displayName": "" }
//
// Windows note: screen capture requires a desktop session, so "start on boot"
// means start at *logon* (the tray runs in the user's session). The
// install-agent.ps1 script wires the scheduled task up accordingly.

const { app, BrowserWindow, Tray, Menu, nativeImage, ipcMain, desktopCapturer, screen, dialog } = require("electron");
const fs = require("node:fs");
const path = require("node:path");

const CONFIG_PATH = path.join(__dirname, "config.json");
const CONFIG_EXAMPLE = path.join(__dirname, "config.example.json");

function loadConfig() {
  try {
    if (!fs.existsSync(CONFIG_PATH) && fs.existsSync(CONFIG_EXAMPLE)) {
      fs.copyFileSync(CONFIG_EXAMPLE, CONFIG_PATH);
    }
    const raw = JSON.parse(fs.readFileSync(CONFIG_PATH, "utf8"));
    return Object.assign({
      channel: "WORK",
      token: "",
      relay: "wss://xbuniverse.duckdns.org/screen-mirror/ws",
      fps: 60,
      autoLaunch: true,
      displayName: "",
      iceUrl: "https://xbuniverse.duckdns.org/screen-mirror/ice",
    }, raw);
  } catch (err) {
    dialog.showErrorBox("mirror agent", "Could not read config.json: " + err.message);
    return null;
  }
}

let win = null;
let tray = null;
let config = null;
let statusText = "starting";

// Single instance: a second launch just leaves the existing tray process alone.
const gotLock = app.requestSingleInstanceLock();
if (!gotLock) {
  app.quit();
} else {
  app.whenReady().then(() => {
    config = loadConfig();
    if (!config) { app.quit(); return; }

    // Hidden window: never shown, never focused. It hosts the WebRTC sender.
    win = new BrowserWindow({
      show: false,
      skipTaskbar: true,
      width: 8,
      height: 8,
      webPreferences: {
        preload: path.join(__dirname, "preload.js"),
        contextIsolation: true,
        nodeIntegration: false,
        backgroundThrottling: false, // keep the capture + encode loop at full rate
      },
    });

    // Auto-approve the capture request in the hidden window: pick the primary
    // display (or the one named in config) and hand it to getDisplayMedia, with
    // Windows loopback audio so "Share audio" works without a prompt.
    win.webContents.session.setDisplayMediaRequestHandler(async (req, callback) => {
      try {
        const sources = await desktopCapturer.getSources({ types: ["screen"] });
        let src = null;
        if (config.displayName) {
          src = sources.find((s) => s.name === config.displayName) || null;
        }
        if (!src) {
          const primary = screen.getPrimaryDisplay();
          src = sources.find((s) => String(s.display_id) === String(primary.id)) || sources[0];
        }
        if (!src) { callback({}); return; }
        try {
          callback({ video: src, audio: "loopback" });
        } catch {
          callback({ video: src });
        }
      } catch {
        callback({});
      }
    });

    // Surface hidden-window errors on the console (the window itself is never
    // visible, so without this a renderer failure is invisible).
    win.webContents.on("preload-error", (_e, pth, err) => console.log("[agent] preload-error", pth, err && err.message));
    win.webContents.on("console-message", (_e, _level, message, line, sourceId) =>
      console.log("[agent] renderer:", message, (sourceId || "") + ":" + line));

    win.loadFile(path.join(__dirname, "renderer.html"));

    // The sandboxed preload has no fs access: it pulls the config from here.
    ipcMain.on("agent-config", (e) => { e.returnValue = config; });

    ipcMain.on("agent-status", (_e, text) => {
      statusText = String(text || "");
      if (tray) tray.setToolTip("open77 mirror - " + statusText);
    });

    buildTray();

    // Auto-launch at logon (per-user). Toggleable via the tray menu.
    if (config.autoLaunch) {
      app.setLoginItemSettings({ openAtLogin: true, path: process.execPath, args: [__dirname] });
    }
  });

  app.on("window-all-closed", () => { /* keep running in the tray */ });
}

function statusMenu() {
  return Menu.buildFromTemplate([
    { label: "Status: " + statusText, enabled: false },
    { type: "separator" },
    {
      label: "Launch at logon",
      type: "checkbox",
      checked: !!app.getLoginItemSettings().openAtLogin,
      click: (item) => {
        app.setLoginItemSettings({ openAtLogin: item.checked, path: process.execPath, args: [__dirname] });
      },
    },
    { label: "Reconnect now", click: () => { if (win) win.webContents.send("agent-reconnect"); } },
    { type: "separator" },
    { label: "Quit mirror agent", click: () => app.quit() },
  ]);
}

function buildTray() {
  // 16x16 dot icon generated at runtime so no binary asset ships with the agent.
  tray = new Tray(nativeImage.createFromBuffer(dotPng()));
  tray.setToolTip("open77 mirror - " + statusText);
  tray.setContextMenu(statusMenu());
  // Rebuild the menu on open so the status line stays current.
  tray.on("click", () => tray.setContextMenu(statusMenu()));
}

// Minimal solid-dot PNG (16x16) so the tray has an icon without shipping files.
function dotPng() {
  return Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAm0lEQVQ4y2NgGAWjYBSMglEwCkY" +
    "BIwMIYPj/H4YxMDAwMDP//IzEGBgYGhv//kRgDAwMDw///SIyBgYGB4f9/JMbAwMDA8P8/EmNgYGB" +
    "g+P8fiTEwMDAw/P+PxBgYGBgY/v9HYgwMDAwM//8jMQYGBgaG//+RGAMDAwPD//9IjIGBgYHh/38k" +
    "xgAAAABJRU5ErkJggg==",
    "base64"
  );
}
