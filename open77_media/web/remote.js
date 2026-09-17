// =============================================================================
// open77_media -- web/remote.js
// =============================================================================
// The television remote: the panel a player gets when they open the UI at a set.
//
// It owns no state, on the same rule as `tv.js`: everything it shows came from
// the server through the client (`remote:state`), and every button is a REQUEST
// (`remote:action`) that changes nothing by itself. So a panel that has asked for
// a URL change, a mute or a nudge keeps showing the old value until the server's
// answer arrives and comes back down -- which is what makes two players holding a
// remote at one set agree about it, and what makes "my click did nothing"
// distinguishable from "the click worked and nothing changed".
//
// Which set it drives is not this page's decision either. The client picks the
// nearest set that is inside ITS OWN reach -- `reach` is the server's number, one
// per record, from `Open77MediaPlacement.Reach` -- and names it in `remote:state`;
// the page never sends an id and cannot address a set it was not pointed at (see
// the `remote:action` handler in `client/main.lua`).
//
// -----------------------------------------------------------------------------
// THREE THINGS THIS PAGE HAS TO GET RIGHT, BECAUSE THEY ARE ALL TYPING
// -----------------------------------------------------------------------------
//   * The SOURCE URL field is never hidden. Spawning starts with nothing in
//     reach by definition, so a field that lived inside the controls block was
//     off screen exactly when a player wanted to give the new set a link.
//   * While the field has the caret, the panel's single-key shortcuts must not
//     fire. `m` and SPACE are in every URL, and they were bound to mute and
//     pause: typing "https://www.youtube.com" muted the set and paused it.
//   * Ctrl+V pastes because the HOST handles it (`SurfaceClient::Send`, which
//     translates the accelerator into CEF's own paste command). A page cannot
//     read the clipboard, and no amount of work here would change that -- which
//     is why the field takes the caret when the panel opens, so a paste has
//     somewhere to land.

(function () {
  "use strict";

  // ---------------------------------------------------------------------------
  // Bridge
  // ---------------------------------------------------------------------------
  // The surface has no bridge when the page is opened in a normal browser while
  // developing it, so the shim stands one up and opens the panel with a sample
  // set: the layout, the catalogue and the key handling can then be iterated on
  // without a game launch. It never reports success, so a shimmed page cannot be
  // mistaken for a working one.
  const shim = {
    on: function () {},
    emit: function () {},
    ready: function () {},
  };
  const bridge = (typeof Open77 !== "undefined" && Open77) ? Open77 : shim;
  const shimmed = bridge === shim;

  const elements = {
    root: document.getElementById("remote"),
    label: document.getElementById("target-label"),
    meta: document.getElementById("target-meta"),
    close: document.getElementById("close"),
    url: document.getElementById("url"),
    urlHint: document.getElementById("url-hint"),
    load: document.getElementById("load"),
    controls: document.getElementById("controls"),
    near: document.getElementById("near"),
    nearText: document.getElementById("near-text"),
    mute: document.getElementById("mute"),
    play: document.getElementById("play"),
    curtainOpen: document.getElementById("curtain-open"),
    curtainClose: document.getElementById("curtain-close"),
    curtainReveal: document.getElementById("curtain-reveal"),
    sets: document.getElementById("sets"),
    setList: document.getElementById("setlist"),
    volume: document.getElementById("volume"),
    volumeText: document.getElementById("volume-text"),
    step: document.getElementById("step"),
    stepTrigger: document.getElementById("step-trigger"),
    stepMenu: document.getElementById("step-menu"),
    stepLabel: document.getElementById("step-label"),
    remove: document.getElementById("remove"),
    search: document.getElementById("search"),
    refresh: document.getElementById("refresh"),
    catalogue: document.getElementById("catalogue"),
    keyOpen: document.getElementById("key-open"),
    note: document.getElementById("note"),
  };

  // The server's last word, and nothing else. `reach` is per set, so there is no
  // single range to carry here: `defaultReach` is only what a set with no
  // server-described reach falls back to, and the page never has to apply it.
  let state = { open: false, target: null, count: 0, defaultReach: 15, key: "F5" };
  // How far one press moves a set. The panel's own choice, not the server's: it
  // is a multiplier on a nudge, and the server clamps what it is given.
  const STEPS = [0.05, 0.25, 1, 5];
  let moveStep = 0.25;
  let catalogue = [];
  let filter = "";
  let noteTimer = null;
  let urlEditing = false;
  let wasOpen = false;

  const emit = (action, extra) =>
    bridge.emit("remote:action", Object.assign({ action: action }, extra || {}));

  function note(text, kind) {
    elements.note.textContent = text || "";
    elements.note.className = kind ? "note " + kind : "note";
    clearTimeout(noteTimer);
    if (text) {
      noteTimer = setTimeout(() => {
        elements.note.textContent = "";
        elements.note.className = "note";
      }, 4000);
    }
  }

  function asArray(value) {
    return Array.isArray(value) ? value : [];
  }

  function text(value) {
    return String(value === undefined || value === null ? "" : value).toLocaleLowerCase();
  }

  function metres(value) {
    const number = Number(value);
    return isFinite(number) ? number.toFixed(1) + " m" : null;
  }

  // The caret is in a field: the panel's own keys are the field's characters.
  function typing() {
    const active = document.activeElement;
    if (!active) return false;
    const tag = active.tagName;
    return tag === "INPUT" || tag === "TEXTAREA";
  }

  // The placement listbox is open: its own keys are the listbox's (the arrows
  // walk it), so the panel's shortcuts stay out of the way the same way they do
  // for the caret.
  function choosing() {
    return elements.step.classList.contains("open");
  }

  // ---------------------------------------------------------------------------
  // Render
  // ---------------------------------------------------------------------------

  function renderTarget() {
    const target = state.target;
    const nearest = state.nearest;
    elements.controls.hidden = !target;
    // The link is the set's when there is one, and the next spawn's when there is
    // not -- one field, and the line under it says which of the two it is now.
    elements.load.disabled = !target;
    elements.load.title = target
      ? "Show this link on the set"
      : "Spawn a set, or step into one's reach, to change what it is playing";

    if (!target) {
      elements.label.textContent = state.count > 0
        ? "NO SET IN REACH"
        : "NO SETS ON THIS SERVER";
      elements.meta.textContent = state.count > 0
        ? state.count + " set" + (state.count === 1 ? "" : "s") + " in the world"
        : "spawn one below and it lands in front of you, facing you";
      elements.urlHint.textContent = "the next set you spawn is born with this link";
    } else {
      const distance = Number(target.distance);
      const reach = Number(target.reach);
      elements.label.textContent = "#" + target.id + "  " + (target.label || "television");
      elements.meta.textContent =
        (target.record ? target.record + " \u00b7 " : "") +
        (isFinite(distance) ? distance.toFixed(1) + " m away" : "distance unknown") +
        (isFinite(reach) ? " \u00b7 reach " + reach.toFixed(0) + " m" : "") +
        (target.materialised === true ? " \u00b7 on screen" : " \u00b7 not rendered here yet");
      elements.urlHint.textContent = "applies to set #" + target.id +
        " \u00b7 paste with Ctrl+V";

      // The URL, volume and transport controls cannot be re-synced mid-thought:
      // overwriting the link somebody is typing or dragging loses their work.
      if (!urlEditing) elements.url.value = target.url || "";
      elements.volume.value = String(Number(target.volume) || 0);
      elements.volumeText.textContent = String(Math.round(Number(target.volume) || 0));
      elements.mute.textContent = target.muted ? "UNMUTE" : "MUTE";
      elements.mute.classList.toggle("active", target.muted === true);
      elements.play.textContent = target.paused ? "PLAY" : "PAUSE";
      elements.play.classList.toggle("active", target.paused === true);
      // The lit button is the SET's mode, read back from the server, and not this
      // page's memory of which button was pressed: a control that only remembered
      // its own click would disagree with the set the moment a second player -- or
      // the console -- changed it.
      const curtain = String(target.curtain || "open");
      elements.curtainOpen.classList.toggle("active", curtain === "open");
      elements.curtainClose.classList.toggle("active", curtain === "closed");
      elements.curtainReveal.classList.toggle("active", curtain === "reveal");
    }

    // Why nothing is drivable, when something is nearby: the set, its distance,
    // and the distance it can be driven from.
    if (!target && nearest) {
      const distance = metres(nearest.distance);
      const reach = metres(nearest.reach);
      elements.nearText.textContent = distance === null
        ? "the nearest set is #" + nearest.id + " (" + (nearest.label || "television") + ")"
        : "nearest set: #" + nearest.id + " " + (nearest.label || "television") +
          " \u2014 " + distance + " away" +
          (reach === null ? "" : ", drivable from " + reach);
      elements.near.hidden = false;
    } else {
      elements.near.hidden = true;
      elements.nearText.textContent = "";
    }

    elements.keyOpen.textContent = state.key || "F5";
  }

  function renderCatalogue() {
    elements.catalogue.replaceChildren();
    const shown = catalogue.filter(record => filter === "" ||
      text(record.label).includes(filter) || text(record.id).includes(filter) ||
      text(record.model).includes(filter));

    if (shown.length === 0) {
      const empty = document.createElement("p");
      empty.className = "empty";
      empty.textContent = catalogue.length === 0
        ? "No records received. Is the open77_media resource running on this server?"
        : "No record matches that filter.";
      elements.catalogue.append(empty);
      return;
    }

    for (const record of shown) {
      const card = document.createElement("button");
      card.className = "card";
      card.type = "button";
      const title = document.createElement("strong");
      title.textContent = record.label || record.id;
      const blurb = document.createElement("small");
      blurb.textContent = record.blurb || record.model || "";
      const size = document.createElement("span");
      size.className = "tag";
      const width = Number(record.width), height = Number(record.height);
      size.textContent = isFinite(width) && isFinite(height)
        ? width.toFixed(2) + " x " + height.toFixed(2) + " m"
        : String(record.id || "");
      card.append(title, blurb, size);
      card.addEventListener("click", () => {
        // The SOURCE URL above is what the new set is born with: it is the same
        // question whichever shape the panel is in, and answering it twice with
        // two fields is how they end up disagreeing.
        emit("spawn", { record: record.id, url: elements.url.value.trim() });
        note("spawning " + (record.label || record.id) + " ...");
      });
      elements.catalogue.append(card);
    }
  }

  // Every set in the world, with the one button that can address a set the panel
  // is not pointed at. The reach rule is stated per row rather than enforced
  // silently: a row that cannot be driven says so and still offers REMOVE, which
  // is the whole reason this list exists.
  function renderSets() {
    const sets = asArray(state.sets);
    elements.setList.replaceChildren();
    elements.sets.hidden = sets.length === 0;
    if (sets.length === 0) return;

    for (const set of sets) {
      const row = document.createElement("div");
      row.className = "setrow";

      const name = document.createElement("span");
      name.className = "setname";
      name.textContent = "#" + set.id + " " + (set.label || set.record || "television");

      const where = document.createElement("span");
      where.className = "setwhere";
      const distance = metres(set.distance);
      const reach = Number(set.reach);
      const inReach = isFinite(distance === null ? NaN : Number(set.distance)) &&
        isFinite(reach) && Number(set.distance) <= reach;
      where.textContent = (distance === null ? "distance unknown" : distance + " away") +
        (isFinite(reach) ? (inReach ? " \u00b7 in reach" : " \u00b7 reach " + reach.toFixed(0) + " m") : "");
      where.classList.toggle("ok", inReach);

      const remove = document.createElement("button");
      remove.className = "btn danger small";
      remove.type = "button";
      remove.textContent = "REMOVE";
      remove.title = "Delete set #" + set.id + " and its prop";
      remove.addEventListener("click", () => {
        emit("remove", { id: set.id });
        note("removing set #" + set.id + " ...");
      });

      row.append(name, where, remove);
      elements.setList.append(row);
    }
  }

  function render() {
    const open = state.open === true;
    elements.root.classList.toggle("open", open);
    renderTarget();
    renderSets();
    renderCatalogue();

    // The caret, once per open. The host owns Ctrl+V (a page cannot read the
    // clipboard), so the paste needs a focused field to land in -- and taking it
    // on the opening EDGE rather than on every state push is what keeps it from
    // stealing the caret back from somebody mid-URL.
    if (open && !wasOpen) elements.url.focus();
    wasOpen = open;
  }

  // ---------------------------------------------------------------------------
  // Controls
  // ---------------------------------------------------------------------------

  elements.close.addEventListener("click", () => emit("close"));

  function showUrl() {
    if (!state.target) return;
    const url = elements.url.value.trim();
    emit("url", { url: url });
    note("asked for " + (url || "the idle test pattern") + " ...");
  }
  elements.load.addEventListener("click", showUrl);
  elements.url.addEventListener("keydown", event => {
    if (event.key === "Enter") showUrl();
  });
  elements.url.addEventListener("focus", () => { urlEditing = true; });
  elements.url.addEventListener("blur", () => { urlEditing = false; });

  elements.mute.addEventListener("click", () =>
    emit("muted", { value: !(state.target && state.target.muted === true) }));
  elements.play.addEventListener("click", () =>
    emit("paused", { value: !(state.target && state.target.paused === true) }));
  // Three modes, each naming itself. The page still only REQUESTS: the lit button
  // changes when the server's answer arrives, on the same rule as every other
  // control here.
  for (const button of [[elements.curtainOpen, "open"], [elements.curtainClose, "closed"],
    [elements.curtainReveal, "reveal"]]) {
    button[0].addEventListener("click", () => {
      if (!state.target) return;
      emit("curtain", { value: button[1] });
      note("curtain " + button[1] + " ...");
    });
  }
  elements.remove.addEventListener("click", () => {
    if (!state.target) return;
    emit("remove");
    note("removing set #" + state.target.id + " ...");
  });

  // Sent on release rather than on every pixel of the drag: the server is the
  // only writer of a set's volume, and a slider that streamed while being dragged
  // would be a request per frame.
  elements.volume.addEventListener("change", () =>
    emit("volume", { volume: Number(elements.volume.value) }));

  // ---------------------------------------------------------------------------
  // Placement step (a listbox, because a native `<select>` popup does not render
  // in this compositor -- the repo pins that rule in `WebUiAssetTests`)
  // ---------------------------------------------------------------------------
  function closeStep() {
    elements.step.classList.remove("open");
    elements.stepTrigger.setAttribute("aria-expanded", "false");
  }

  function focusStep() {
    const selected = elements.stepMenu.querySelector(".dropdown-option.on")
      || elements.stepMenu.querySelector(".dropdown-option");
    if (selected) {
      selected.focus();
      selected.scrollIntoView({ block: "nearest" });
    }
  }

  function chooseStep(value) {
    moveStep = value;
    elements.stepLabel.textContent = value + " m";
    for (const option of elements.stepMenu.querySelectorAll(".dropdown-option")) {
      const on = Number(option.dataset.value) === value;
      option.classList.toggle("on", on);
      option.setAttribute("aria-selected", on ? "true" : "false");
    }
    closeStep();
  }

  function buildStepMenu() {
    for (const value of STEPS) {
      const option = document.createElement("button");
      option.type = "button";
      option.className = "dropdown-option";
      option.setAttribute("role", "option");
      option.dataset.value = String(value);
      option.textContent = value + " m";
      option.addEventListener("click", event => {
        event.stopPropagation();
        chooseStep(value);
        elements.stepTrigger.focus();
      });
      elements.stepMenu.appendChild(option);
    }
    chooseStep(moveStep);
  }

  buildStepMenu();

  elements.stepTrigger.addEventListener("click", event => {
    event.stopPropagation();
    const opening = !elements.step.classList.contains("open");
    elements.step.classList.toggle("open", opening);
    elements.stepTrigger.setAttribute("aria-expanded", opening ? "true" : "false");
    if (opening) focusStep();
  });
  elements.step.addEventListener("keydown", event => {
    const options = Array.prototype.slice.call(
      elements.stepMenu.querySelectorAll(".dropdown-option"));
    if (!options.length) return;
    const current = options.indexOf(document.activeElement);
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      elements.step.classList.add("open");
      elements.stepTrigger.setAttribute("aria-expanded", "true");
      const direction = event.key === "ArrowDown" ? 1 : -1;
      options[((current < 0 ? (direction > 0 ? -1 : 0) : current) + direction
        + options.length) % options.length].focus();
    } else if (event.key === "Home" || event.key === "End") {
      event.preventDefault();
      elements.step.classList.add("open");
      elements.stepTrigger.setAttribute("aria-expanded", "true");
      options[event.key === "Home" ? 0 : options.length - 1].focus();
    } else if (event.key === "Escape") {
      event.stopPropagation();
      closeStep();
      elements.stepTrigger.focus();
    }
  });
  document.addEventListener("click", () => closeStep());

  for (const button of document.querySelectorAll("[data-move]"))
    button.addEventListener("click", () => {
      if (!state.target) return;
      emit("move", { direction: button.dataset.move, metres: moveStep });
      note("nudging set #" + state.target.id + " " + button.dataset.move +
        " " + moveStep + " m ...");
    });
  for (const button of document.querySelectorAll("[data-turn]"))
    button.addEventListener("click", () => {
      if (!state.target) return;
      emit("rotate", { direction: button.dataset.turn, degrees: 15 });
      note("turning set #" + state.target.id + " " + button.dataset.turn + " ...");
    });

  elements.search.addEventListener("input", event => {
    filter = text(event.target.value);
    renderCatalogue();
  });
  elements.refresh.addEventListener("click", () => emit("catalogue"));

  // ---------------------------------------------------------------------------
  // Keys
  // ---------------------------------------------------------------------------
  // The panel is the focused surface while it is open, so these are the keys the
  // player has: one hand on WASD is not the posture this panel assumes. Every one
  // of them yields to the caret -- see `typing()` above, which is the bug this
  // section was. Escape never yields: it is how a player leaves a field, and a
  // panel that could be typed into but not escaped would be a trap.
  document.addEventListener("keydown", event => {
    if (!state.open) return;
    if (event.key === "Escape") { event.preventDefault(); emit("close"); return; }
    if (typing() || choosing()) return;
    if (!state.target) return;

    if (event.key === "m" || event.key === "M") {
      event.preventDefault();
      emit("muted", { value: !(state.target.muted === true) });
    } else if (event.key === " ") {
      event.preventDefault();
      emit("paused", { value: !(state.target.paused === true) });
    } else if (event.key === "ArrowUp" || event.key === "ArrowDown") {
      event.preventDefault();
      const step = event.key === "ArrowUp" ? 5 : -5;
      const volume = Math.min(100, Math.max(0, Math.round(Number(state.target.volume) || 0) + step));
      emit("volume", { volume: volume });
    } else if (event.key === "[") {
      event.preventDefault();
      emit("rotate", { direction: "left", degrees: 15 });
    } else if (event.key === "]") {
      event.preventDefault();
      emit("rotate", { direction: "right", degrees: 15 });
    }
  });

  // ---------------------------------------------------------------------------
  // Bridge
  // ---------------------------------------------------------------------------

  bridge.on("remote:state", payload => {
    if (payload && typeof payload === "object") state = payload;
    render();
  });

  bridge.on("remote:catalogue", payload => {
    if (!payload || typeof payload !== "object") return;
    catalogue = asArray(payload.catalogue);
    renderCatalogue();
  });

  bridge.on("remote:result", payload => {
    if (!payload || typeof payload !== "object") return;
    note(String(payload.text || ""), payload.ok === true ? "ok" : "error");
    // A refused spawn or a refused URL is the one answer the panel cannot see in
    // its state, so the catalogue is re-read with it: the record list is what an
    // operator changes when a spawn stops working.
    if (payload.ok !== true) emit("catalogue");
  });

  bridge.ready();
  bridge.emit("remote:ready", {});

  if (shimmed) {
    // Browser-only: draw the panel so the layout can be worked on offline. The
    // sample data is deliberately not a set that exists anywhere.
    state = {
      open: true,
      count: 2,
      defaultReach: 15,
      key: "F5",
      nearest: {
        id: 7, label: "Cinema screen, 150 ft", record: "cinema.150ft",
        distance: 30.6, reach: 38, materialised: true,
      },
    };
    catalogue = [
      { id: "tv.16x9", label: "Television, 16:9", blurb: "Freestanding cabinet", width: 1.16, height: 0.66, model: "open77_prop_tv_16x9" },
      { id: "cinema.150ft", label: "Cinema screen, 150 ft", blurb: "A 150 ft 16:9 picture", width: 45.72, height: 26.0131, model: "open77_prop_cinema_150ft" },
    ];
    note("no bridge: this is the offline preview, not a live set", "error");
    render();
  }
})();
