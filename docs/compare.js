// Media on feature pages: before/after comparisons with upstream, evidence
// tables, and the check that decides whether a capture exists at all.
//
// A comparison is the same view in upstream Windows Terminal and in Mullion.
// Which views it offers depends on whether the two shots line up pixel for
// pixel (same window, same scene, the feature's change the only change):
//
//   aligned pairs   hover   the "before" shot; hovering (or focusing, or tapping) shows "after"
//                   split   a divider dragged across the two, also movable with the arrow keys
//                   side    the two next to each other, stacked on a narrow screen
//                   stack   before above after, each the full width of the column
//   any other pair  side or stack, stack by default: two different layouts
//                   side by side are each too small to read
//
// Every layer carries both themes (shot-light / shot-dark), so a comparison
// follows the page theme exactly as a plain screenshot does. The default view is
// a display setting (Mullion.settings().compareMode), used where a figure offers
// it; a figure's own buttons override it for that figure until the setting
// changes again.
//
// A capture that does not exist renders nothing: a missing "before" leaves the
// "after" as a single image, and a missing "after" hides the figure. Each
// figure reports the outcome with a "mediastate" event, so the page can drop a
// section left with nothing in it.
(function () {
  "use strict";

  var MODES = {
    hover: { label: "Hover", hint: "Before; hover for after" },
    split: { label: "Split", hint: "Drag the divider" },
    side: { label: "Side by side", hint: "Both at once, next to each other" },
    stack: { label: "Stacked", hint: "Before above after, each full width" }
  };
  var ALIGNED = ["hover", "split", "side", "stack"];
  var LOOSE = ["side", "stack"];

  function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  // Captions are HTML (a <code> span); alt text and labels take the text of it.
  function text(html) { return String(html).replace(/<[^>]*>/g, ""); }

  function layer(which, base, alt) {
    return '<div class="compare-layer compare-' + which + '">' +
      '<span class="compare-label">' + (which === "before" ? "Before" : "After") + "</span>" +
      '<img class="shot-light" src="media/' + esc(base) + '-light.png" alt="' + esc(alt) + '" loading="lazy" draggable="false" />' +
      '<img class="shot-dark" src="media/' + esc(base) + '-dark.png" alt="' + esc(alt) + '" loading="lazy" draggable="false" />' +
      "</div>";
  }

  // item: { kind: "compare", before, after, caption, aligned?, frame? }
  //   aligned: true   the shots line up pixel for pixel, so Hover and Split mean something
  //   frame: "square" no rounded frame, for a shot whose own corners are the point
  function html(item) {
    var caption = text(item.caption || "");
    var modes = item.aligned ? ALIGNED : LOOSE;
    var buttons = "";
    for (var i = 0; i < modes.length; i++) {
      buttons += '<button type="button" role="radio" aria-checked="false" data-compare-mode="' + modes[i] +
        '" data-tip="' + esc(MODES[modes[i]].hint) + '">' + MODES[modes[i]].label + "</button>";
    }
    return '<figure class="compare' + (item.frame === "square" ? " frame-square" : "") + '" data-compare data-modes="' + modes.join(" ") + '">' +
      '<div class="compare-bar">' +
        '<span class="compare-chip" aria-live="polite"></span>' +
        '<div class="compare-modes" role="radiogroup" aria-label="Compare view">' + buttons + "</div>" +
      "</div>" +
      '<div class="compare-stage" tabindex="0" aria-label="' + esc(caption) + ' (before and after)">' +
        layer("before", item.before, caption + " — upstream Windows Terminal (before)") +
        layer("after", item.after, caption + " — Mullion (after)") +
        '<div class="compare-handle" role="slider" tabindex="-1" aria-label="Divider between before and after" ' +
          'aria-valuemin="0" aria-valuemax="100" aria-valuenow="50"><span aria-hidden="true">&#8644;</span></div>' +
      "</div>" +
      "<figcaption>" + (item.caption || "") + "</figcaption>" +
      "</figure>";
  }

  // Evidence that has no pixels of its own (a UI Automation tree, a CI run) is
  // a table, not a picture of one: it reads in either theme, scales, and can
  // be selected and searched.
  //
  // item: { kind: "evidence", caption, columns: [...], source?, note?,
  //         before?: table, after: table }
  // table: { label (HTML), rows: [[cell, ...], ...], mark?: [row index, ...] }
  // A null cell reads "(none)". Marked rows are the ones the caption points at.
  function evidenceTable(which, t, columns) {
    var head = "";
    for (var c = 0; c < columns.length; c++) { head += '<th scope="col">' + esc(columns[c]) + "</th>"; }
    var body = "";
    for (var r = 0; r < t.rows.length; r++) {
      var marked = (t.mark || []).indexOf(r) >= 0;
      body += "<tr" + (marked ? ' class="marked"' : "") + ">";
      for (var k = 0; k < t.rows[r].length; k++) {
        var v = t.rows[r][k];
        body += v == null ? '<td><span class="none">(none)</span></td>' : "<td>" + esc(v) + "</td>";
      }
      body += "</tr>";
    }
    return '<div class="evidence-side">' +
      '<p class="evidence-label">' + (which ? '<span class="evidence-which">' + which + "</span>" : "") + t.label + "</p>" +
      '<div class="table-scroll"><table><thead><tr>' + head + "</tr></thead><tbody>" + body + "</tbody></table></div>" +
      "</div>";
  }

  function evidence(item) {
    var cols = item.columns || [];
    return '<figure class="evidence">' +
      (item.source ? '<pre class="evidence-source"><code>' + esc(item.source) + "</code></pre>" : "") +
      (item.before ? evidenceTable("Before", item.before, cols) + evidenceTable("After", item.after, cols)
        : evidenceTable("", item.after, cols)) +
      (item.note ? '<p class="evidence-note">' + item.note + "</p>" : "") +
      "<figcaption>" + (item.caption || "") + "</figcaption>" +
      "</figure>";
  }

  // Whether a set of shots (one per theme) exists: "present" as soon as one
  // loads, "absent" once every one has failed, "pending" until then. Only the
  // current theme's copy is on screen, and a lazy image that is not on screen
  // never loads or fails, so once one copy fails the others are fetched
  // eagerly to settle the question.
  function watchShots(shots, onChange) {
    var state = "";
    function evaluate() {
      var failed = 0, loaded = 0;
      for (var i = 0; i < shots.length; i++) {
        if (shots[i].classList.contains("absent")) { failed++; }
        else if (shots[i]._loaded) { loaded++; }
      }
      var next = loaded ? "present" : failed === shots.length ? "absent" : "pending";
      if (next !== state) { state = next; onChange(state); }
    }
    function failed(el) {
      el._loaded = false;
      el.classList.add("absent");
      for (var i = 0; i < shots.length; i++) {
        if (shots[i] !== el && shots[i].tagName === "IMG" && shots[i].loading === "lazy") { shots[i].loading = "eager"; }
      }
      evaluate();
    }
    function loaded(el) {
      el._loaded = true;
      el.classList.remove("absent");
      evaluate();
    }
    for (var i = 0; i < shots.length; i++) {
      var el = shots[i];
      // A video reports failure on its own element, not by bubbling, so both
      // node types are listened to the same way.
      el.addEventListener("error", function () { failed(this); });
      el.addEventListener(el.tagName === "VIDEO" ? "loadeddata" : "load", function () { loaded(this); });
      // Settled before these listeners existed (a cached image).
      if (el.tagName === "IMG" && el.complete && el.getAttribute("src")) {
        if (el.naturalWidth) { el._loaded = true; } else { el.classList.add("absent"); }
      }
    }
    evaluate();
  }

  function announce(fig, state) {
    fig.setAttribute("data-media", state);
    var e;
    try { e = new CustomEvent("mediastate", { bubbles: true }); }
    catch (err) { e = document.createEvent("Event"); e.initEvent("mediastate", true, false); }
    fig.dispatchEvent(e);
  }

  // One capture is taken at 150% scaling; show it at its on-screen size.
  var CAPTURE_SCALE = 1.5;
  function fitToCapture(el, box) {
    var w = el.tagName === "VIDEO" ? el.videoWidth : el.naturalWidth;
    if (w) { (box || el).style.maxWidth = Math.round(w / CAPTURE_SCALE) + "px"; }
  }

  function wireOne(fig) {
    var stage = fig.querySelector(".compare-stage");
    var handle = fig.querySelector(".compare-handle");
    var chip = fig.querySelector(".compare-chip");
    var buttons = fig.querySelectorAll("[data-compare-mode]");
    var offered = fig.getAttribute("data-modes").split(" ");
    var override = null;
    var peeking = false;
    var split = 50;
    var hasBefore = true;

    function mode() {
      if (!hasBefore) { return "after-only"; }
      if (override) { return override; }
      var s = window.Mullion && Mullion.settings ? Mullion.settings().compareMode : "";
      return offered.indexOf(s) >= 0 ? s : offered[0] === "hover" ? "hover" : "stack";
    }

    function render() {
      var m = mode();
      fig.setAttribute("data-mode", m);
      fig.classList.toggle("peek", peeking);
      for (var i = 0; i < buttons.length; i++) {
        var on = buttons[i].getAttribute("data-compare-mode") === m;
        buttons[i].setAttribute("aria-checked", String(on));
        buttons[i].tabIndex = on ? 0 : -1;
      }
      stage.style.setProperty("--split", split + "%");
      handle.setAttribute("aria-valuenow", String(Math.round(split)));
      handle.tabIndex = m === "split" ? 0 : -1;
      var text = m === "hover" ? (peeking ? "After" : "Before") : "";
      chip.textContent = text;
      chip.hidden = !text;
    }

    for (var i = 0; i < buttons.length; i++) {
      buttons[i].addEventListener("click", function () {
        override = this.getAttribute("data-compare-mode");
        peeking = false;
        render();
      });
    }
    // A radio group: one tab stop, arrows move the choice.
    fig.querySelector(".compare-modes").addEventListener("keydown", function (e) {
      var back = e.key === "ArrowLeft" || e.key === "ArrowUp", fwd = e.key === "ArrowRight" || e.key === "ArrowDown";
      if (!back && !fwd) { return; }
      e.preventDefault();
      var at = offered.indexOf(mode());
      var next = (at + (fwd ? 1 : offered.length - 1)) % offered.length;
      override = offered[next];
      render();
      buttons[next].focus();
    });

    // Peek: hover, keyboard focus, or a tap on touch screens (no hover there).
    function setPeek(on) { if (peeking !== on) { peeking = on; render(); } }
    stage.addEventListener("pointerenter", function (e) { if (e.pointerType === "mouse") { setPeek(true); } });
    stage.addEventListener("pointerleave", function (e) { if (e.pointerType === "mouse") { setPeek(false); } });
    stage.addEventListener("focus", function () { if (mode() === "hover") { setPeek(true); } });
    stage.addEventListener("blur", function () { setPeek(false); });
    stage.addEventListener("click", function (e) {
      if (e.pointerType && e.pointerType !== "mouse" && mode() === "hover") { setPeek(!peeking); }
    });

    // Split: follow the pointer while it is down (or hovering, with a mouse).
    var dragging = false;
    function moveTo(clientX) {
      var r = stage.getBoundingClientRect();
      split = Math.max(0, Math.min(100, ((clientX - r.left) / r.width) * 100));
      render();
    }
    stage.addEventListener("pointerdown", function (e) {
      if (mode() !== "split") { return; }
      dragging = true;
      try { stage.setPointerCapture(e.pointerId); } catch (err) { /* older browsers */ }
      moveTo(e.clientX);
    });
    stage.addEventListener("pointermove", function (e) {
      if (mode() === "split" && (dragging || e.pointerType === "mouse")) { moveTo(e.clientX); }
    });
    stage.addEventListener("pointerup", function () { dragging = false; });
    stage.addEventListener("pointercancel", function () { dragging = false; });
    handle.addEventListener("keydown", function (e) {
      var step = e.shiftKey ? 10 : 2;
      if (e.key === "ArrowLeft" || e.key === "ArrowDown") { split = Math.max(0, split - step); }
      else if (e.key === "ArrowRight" || e.key === "ArrowUp") { split = Math.min(100, split + step); }
      else if (e.key === "Home") { split = 0; }
      else if (e.key === "End") { split = 100; }
      else { return; }
      e.preventDefault();
      render();
    });

    // The figure is whatever "after" is; "before" only decides whether it is
    // a comparison or a single image.
    var afterState = "pending", beforeState = "pending";
    function settle() {
      hasBefore = beforeState !== "absent";
      fig.classList.toggle("compare-no-before", !hasBefore);
      fig.classList.toggle("no-media", afterState === "absent");
      render();
      announce(fig, afterState === "absent" ? "absent" : afterState === "present" && beforeState !== "pending" ? "present" : "pending");
    }
    watchShots(fig.querySelectorAll(".compare-before img"), function (s) { beforeState = s; settle(); });
    watchShots(fig.querySelectorAll(".compare-after img"), function (s) { afterState = s; settle(); });
    var afters = fig.querySelectorAll(".compare-after img");
    for (var a = 0; a < afters.length; a++) {
      afters[a].addEventListener("load", function () { fitToCapture(this, stage); });
      if (afters[a].complete && afters[a].naturalWidth) { fitToCapture(afters[a], stage); }
    }

    fig._compareReset = function () { override = null; peeking = false; render(); };
    render();
  }

  // A plain screenshot or recording: shown at its capture size, hidden when
  // neither theme's copy exists.
  function wirePlain(fig) {
    var shots = fig.querySelectorAll("img, video");
    for (var i = 0; i < shots.length; i++) {
      shots[i].addEventListener(shots[i].tagName === "VIDEO" ? "loadeddata" : "load", function () { fitToCapture(this); });
      if (shots[i].tagName === "IMG" && shots[i].complete && shots[i].naturalWidth) { fitToCapture(shots[i]); }
    }
    watchShots(shots, function (s) {
      fig.classList.toggle("no-media", s === "absent");
      announce(fig, s);
    });
  }

  function wire(scope) {
    var figs = (scope || document).querySelectorAll("figure[data-compare], figure[data-base]");
    for (var i = 0; i < figs.length; i++) {
      if (figs[i]._wired) { continue; }
      figs[i]._wired = true;
      if (figs[i].hasAttribute("data-compare")) { wireOne(figs[i]); } else { wirePlain(figs[i]); }
    }
  }

  // Changing the default view resets every figure's own choice, as the notes
  // app's before/after does: the setting is the newer, more deliberate choice.
  if (window.Mullion && Mullion.onChange) {
    var last = Mullion.settings().compareMode;
    Mullion.onChange(function (s) {
      if (s.compareMode === last) { return; }
      last = s.compareMode;
      var figs = document.querySelectorAll("figure[data-compare]");
      for (var i = 0; i < figs.length; i++) { if (figs[i]._compareReset) { figs[i]._compareReset(); } }
    });
  }

  window.MullionCompare = { html: html, evidence: evidence, wire: wire, text: text, modes: MODES };
})();
