// Before/after comparison for feature pages: the same view in upstream Windows
// Terminal and in Mullion, in four modes.
//
//   hover       the "before" shot; hovering (or focusing, or tapping) shows "after"
//   split       a divider dragged across the two, also movable with the arrow keys
//   side        the two next to each other, stacked on a narrow screen
//   difference  the "after" shot; hovering blends "before" over it with
//               mix-blend-mode: difference, so every unchanged pixel goes black
//               and only what the feature changed lights up
//
// Every layer carries both themes (shot-light / shot-dark), so a comparison
// follows the page theme exactly as a plain screenshot does. The default mode is
// a display setting (Mullion.settings().compareMode); a figure's own mode
// buttons override it for that figure until the setting changes again.
//
// The two captures should be the same size for the overlay modes to line up;
// Capture-Window.ps1 takes both at the same window size for that reason.
(function () {
  "use strict";

  var MODES = [
    { id: "hover", label: "Hover", hint: "Before; hover for after" },
    { id: "split", label: "Split", hint: "Drag the divider" },
    { id: "side", label: "Side by side", hint: "Both at once" },
    { id: "difference", label: "Difference", hint: "After; hover to light up what changed" }
  ];
  var MODE_IDS = MODES.map(function (m) { return m.id; });

  function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  function layer(which, base, alt) {
    return '<div class="compare-layer compare-' + which + '">' +
      '<img class="shot-light" src="media/' + esc(base) + '-light.png" alt="' + esc(alt) + '" loading="lazy" draggable="false" />' +
      '<img class="shot-dark" src="media/' + esc(base) + '-dark.png" alt="' + esc(alt) + '" loading="lazy" draggable="false" />' +
      '<span class="compare-label">' + (which === "before" ? "Before" : "After") + "</span>" +
      "</div>";
  }

  // item: { kind: "compare", before: "<media base>", after: "<media base>", caption, mode? }
  // A fixed `mode` pins the figure to it and drops the mode buttons: "stack" (before
  // above after) suits two evidence panels of text, which have nothing to blend or split.
  function html(item) {
    var caption = item.caption || "";
    var fixed = item.mode === "stack" || MODE_IDS.indexOf(item.mode) >= 0 ? item.mode || "" : "";
    var buttons = "";
    for (var i = 0; i < MODES.length && !fixed; i++) {
      buttons += '<button type="button" role="radio" aria-checked="false" data-compare-mode="' + MODES[i].id +
        '" data-tip="' + esc(MODES[i].hint) + '">' + MODES[i].label + "</button>";
    }
    return '<figure class="compare" data-compare' + (fixed ? ' data-fixed-mode="' + fixed + '"' : "") + ">" +
      (fixed ? "" :
      '<div class="compare-bar">' +
        '<span class="compare-chip" aria-live="polite"></span>' +
        '<div class="compare-modes" role="radiogroup" aria-label="Compare view">' + buttons + "</div>" +
      "</div>") +
      '<div class="compare-stage" tabindex="0" aria-label="' + esc(caption) + ' (before and after)">' +
        layer("before", item.before, caption + " — upstream Windows Terminal (before)") +
        layer("after", item.after, caption + " — Mullion (after)") +
        '<div class="compare-handle" role="slider" tabindex="-1" aria-label="Divider between before and after" ' +
          'aria-valuemin="0" aria-valuemax="100" aria-valuenow="50"><span aria-hidden="true">&#8644;</span></div>' +
      "</div>" +
      '<div class="pending">The before/after captures for this one are still to be taken.</div>' +
      '<figcaption>' + esc(caption) + "</figcaption>" +
      "</figure>";
  }

  // One capture is taken at 150% scaling; show it at its on-screen size.
  var CAPTURE_SCALE = 1.5;

  function wireOne(fig) {
    var stage = fig.querySelector(".compare-stage");
    var handle = fig.querySelector(".compare-handle");
    var chip = fig.querySelector(".compare-chip");
    var buttons = fig.querySelectorAll("[data-compare-mode]");
    var override = null;
    var peeking = false;
    var split = 50;

    var fixed = fig.getAttribute("data-fixed-mode");

    function mode() {
      if (fig.classList.contains("compare-no-before")) { return "after-only"; }
      if (fixed) { return fixed; }
      if (override) { return override; }
      var s = window.Mullion && Mullion.settings ? Mullion.settings().compareMode : "hover";
      return MODE_IDS.indexOf(s) >= 0 ? s : "hover";
    }

    function render() {
      var m = mode();
      fig.setAttribute("data-mode", m);
      fig.classList.toggle("peek", peeking);
      for (var i = 0; i < buttons.length; i++) {
        buttons[i].setAttribute("aria-checked", String(buttons[i].getAttribute("data-compare-mode") === m));
      }
      stage.style.setProperty("--split", split + "%");
      handle.setAttribute("aria-valuenow", String(Math.round(split)));
      handle.tabIndex = m === "split" ? 0 : -1;
      var text = "";
      if (m === "hover") { text = peeking ? "After" : "Before"; }
      else if (m === "difference") { text = peeking ? "Difference" : "After"; }
      if (chip) {
        chip.textContent = text;
        chip.hidden = !text;
      }
    }

    for (var i = 0; i < buttons.length; i++) {
      buttons[i].addEventListener("click", function () {
        override = this.getAttribute("data-compare-mode");
        peeking = false;
        render();
      });
    }
    var modeGroup = fig.querySelector(".compare-modes");
    if (modeGroup) modeGroup.addEventListener("keydown", function (e) {
      if (e.key !== "ArrowRight" && e.key !== "ArrowLeft") { return; }
      e.preventDefault();
      var at = MODE_IDS.indexOf(mode());
      var next = (at + (e.key === "ArrowRight" ? 1 : MODE_IDS.length - 1)) % MODE_IDS.length;
      override = MODE_IDS[next];
      render();
      buttons[next].focus();
    });

    // Peek: hover, keyboard focus, or a tap on touch screens (no hover there).
    function setPeek(on) { if (peeking !== on) { peeking = on; render(); } }
    stage.addEventListener("pointerenter", function (e) { if (e.pointerType === "mouse") { setPeek(true); } });
    stage.addEventListener("pointerleave", function (e) { if (e.pointerType === "mouse") { setPeek(false); } });
    stage.addEventListener("focus", function () { if (mode() !== "split") { setPeek(true); } });
    stage.addEventListener("blur", function () { setPeek(false); });
    stage.addEventListener("click", function (e) {
      if (e.pointerType && e.pointerType !== "mouse" && mode() !== "split") { setPeek(!peeking); }
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

    // A missing "before" leaves the "after" on its own; a missing "after"
    // means nothing to show yet.
    function track(selector, cls) {
      var imgs = fig.querySelectorAll(selector + " img");
      function check() {
        var gone = 0;
        for (var k = 0; k < imgs.length; k++) { if (imgs[k].classList.contains("absent")) { gone++; } }
        fig.classList.toggle(cls, gone === imgs.length);
        render();
      }
      for (var k = 0; k < imgs.length; k++) {
        imgs[k].addEventListener("error", function () { this.classList.add("absent"); check(); });
        imgs[k].addEventListener("load", function () {
          this.classList.remove("absent");
          if (selector === ".compare-after" && this.naturalWidth) {
            stage.style.maxWidth = Math.round(this.naturalWidth / CAPTURE_SCALE) + "px";
          }
          check();
        });
      }
    }
    track(".compare-before", "compare-no-before");
    track(".compare-after", "no-media");

    fig._compareReset = function () { override = null; peeking = false; render(); };
    render();
  }

  function wire(scope) {
    var figs = (scope || document).querySelectorAll("figure[data-compare]");
    for (var i = 0; i < figs.length; i++) {
      if (!figs[i]._compareReset) { wireOne(figs[i]); }
    }
  }

  // Changing the default mode resets every figure's own choice, as the notes
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

  window.MullionCompare = { html: html, wire: wire, modes: MODES };
})();
