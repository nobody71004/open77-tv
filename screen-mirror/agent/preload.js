// Minimal bridge: the config comes over IPC from the main process (which owns
// config.json) and is exposed synchronously via the context bridge so the
// renderer can connect immediately; status flows up to the tray and the tray's
// "Reconnect now" flows back down. Only ipcRenderer/contextBridge are used here
// - the sandboxed preload has no fs, and with contextIsolation on, plain
// window assignments would NOT be visible to the page (hence the bridge).
const { ipcRenderer, contextBridge } = require("electron");

let config = {};
try { config = ipcRenderer.sendSync("agent-config") || {}; } catch {}

contextBridge.exposeInMainWorld("__mirrorConfig", config);
contextBridge.exposeInMainWorld("__agent", {
  status(text) { ipcRenderer.send("agent-status", String(text)); },
  onReconnect(fn) { ipcRenderer.on("agent-reconnect", () => fn()); },
});
