// Shared by every page: display settings (theme, which feature-page sections
// show, relative or absolute dates), the settings popover in the header, and
// the date formatter the pages call after they render.
//
// Loaded synchronously from <head> so the theme and the section visibility are
// on <html> before the first paint -- no flash of the wrong theme, and no flash
// of a section that is about to be hidden.
(function () {
  "use strict";

  var KEY = "mullion-settings";
  // Keys earlier versions of the site used for the theme alone. Read once, so a
  // visitor's existing choice survives; never written again.
  var LEGACY_THEME_KEYS = ["theme", "wtfork-theme"];

  // Every section feature.html can render, in the order it renders them. The
  // ids are what the pages put in data-section="..." and what html.hide-<id>
  // hides in site.css.
  var SECTIONS = [
    { id: "media", label: "Screenshots", hint: "What it looks like" },
    { id: "problem", label: "Why it exists" },
    { id: "how", label: "How it works" },
    { id: "steps", label: "Try it" },
    { id: "settings", label: "Settings keys" },
    { id: "notes", label: "Worth knowing" },
    { id: "upstream", label: "What upstream did" },
    { id: "stats", label: "Change stats", hint: "Lines added and removed, files, commits" },
    { id: "commits", label: "Commits", hint: "Links to each commit on GitHub" }
  ];

  var DEFAULTS = {
    theme: "system",
    relativeDates: false,
    sections: {
      media: true, problem: true, how: true, steps: true, settings: true,
      notes: true, upstream: true, stats: false, commits: false
    }
  };

  function defaults() { return JSON.parse(JSON.stringify(DEFAULTS)); }

  function sanitize(raw) {
    var s = defaults();
    if (!raw || typeof raw !== "object") { return s; }
    if (raw.theme === "light" || raw.theme === "dark" || raw.theme === "system") { s.theme = raw.theme; }
    if (typeof raw.relativeDates === "boolean") { s.relativeDates = raw.relativeDates; }
    if (raw.sections && typeof raw.sections === "object") {
      for (var i = 0; i < SECTIONS.length; i++) {
        var id = SECTIONS[i].id;
        if (typeof raw.sections[id] === "boolean") { s.sections[id] = raw.sections[id]; }
      }
    }
    return s;
  }

  function load() {
    try {
      var stored = localStorage.getItem(KEY);
      if (stored) { return sanitize(JSON.parse(stored)); }
      var s = defaults();
      for (var i = 0; i < LEGACY_THEME_KEYS.length; i++) {
        var t = localStorage.getItem(LEGACY_THEME_KEYS[i]);
        if (t === "light" || t === "dark" || t === "system") { s.theme = t; break; }
      }
      return s;
    } catch (e) {
      // Storage blocked, private mode, or a corrupt value: defaults, and the
      // page still works -- choices just last for this page view only.
      return defaults();
    }
  }

  function save() {
    try { localStorage.setItem(KEY, JSON.stringify(settings)); } catch (e) { /* not persisted */ }
  }

  var settings = load();
  var root = document.documentElement;
  var listeners = [];

  // ---------------------------------------------------------------- theme
  var darkQuery = window.matchMedia ? window.matchMedia("(prefers-color-scheme: dark)") : null;

  function effectiveTheme() {
    if (settings.theme !== "system") { return settings.theme; }
    return darkQuery && darkQuery.matches ? "dark" : "light";
  }

  // <picture> picks its <source> from a media query, which knows nothing about
  // an explicit Light/Dark choice. Rewrite the dark source's query to match it.
  function syncPictures(scope) {
    var sources = (scope || document).querySelectorAll('source[data-theme-source="dark"]');
    var media = settings.theme === "system" ? "(prefers-color-scheme: dark)"
      : settings.theme === "dark" ? "all" : "not all";
    for (var i = 0; i < sources.length; i++) { sources[i].media = media; }
  }

  function applyTheme() {
    if (settings.theme === "system") { root.removeAttribute("data-theme"); }
    else { root.setAttribute("data-theme", settings.theme); }
    syncPictures();
  }

  function applySections() {
    for (var i = 0; i < SECTIONS.length; i++) {
      var id = SECTIONS[i].id;
      root.classList.toggle("hide-" + id, !settings.sections[id]);
    }
  }

  // ---------------------------------------------------------------- dates
  var MONTHS = ["January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"];

  function parseDate(iso) {
    var m = /^(\d{4})-(\d{2})-(\d{2})/.exec(iso || "");
    return m ? new Date(+m[1], +m[2] - 1, +m[3]) : null;
  }

  function longDate(d) { return d.getDate() + " " + MONTHS[d.getMonth()] + " " + d.getFullYear(); }

  var rtf = null;
  try { rtf = new Intl.RelativeTimeFormat("en", { numeric: "auto" }); } catch (e) { rtf = null; }

  function relative(iso, now) {
    var d = parseDate(iso);
    if (!d) { return iso; }
    now = now || new Date();
    var today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    var days = Math.round((d - today) / 86400000);   // negative = past
    var abs = Math.abs(days);
    var value, unit;
    if (abs < 14) { value = days; unit = "day"; }
    else if (abs < 60) { value = Math.round(days / 7); unit = "week"; }
    else if (abs < 365) { value = Math.round(days / 30.44); unit = "month"; }
    else { value = Math.round(days / 365.25); unit = "year"; }
    if (rtf) { return rtf.format(value, unit); }
    if (value === 0) { return "today"; }
    var n = Math.abs(value);
    var text = n + " " + unit + (n === 1 ? "" : "s");
    return value < 0 ? text + " ago" : "in " + text;
  }

  // Every date on the site is <time datetime="YYYY-MM-DD" data-date>. Its text
  // is whichever form the setting asks for; its tooltip is the other one.
  function formatDates(scope) {
    var els = (scope || document).querySelectorAll("time[data-date]");
    for (var i = 0; i < els.length; i++) {
      var el = els[i];
      var iso = el.getAttribute("datetime");
      var d = parseDate(iso);
      if (!d) { continue; }
      if (settings.relativeDates) {
        el.textContent = relative(iso);
        el.title = longDate(d) + " (" + iso + ")";
      } else {
        el.textContent = iso;
        el.title = longDate(d) + ", " + relative(iso);
      }
    }
  }

  function timeTag(iso) {
    return '<time datetime="' + iso + '" data-date>' + iso + "</time>";
  }

  // ---------------------------------------------------------------- apply
  function applyAll() {
    applyTheme();
    applySections();
    formatDates();
    syncControls();
    for (var i = 0; i < listeners.length; i++) {
      try { listeners[i](settings); } catch (e) { /* a page hook must not break the rest */ }
    }
  }

  function update(mutate) {
    mutate(settings);
    save();
    applyAll();
  }

  applyTheme();
  applySections();

  if (darkQuery) {
    var onScheme = function () { if (settings.theme === "system") { applyAll(); } };
    if (darkQuery.addEventListener) { darkQuery.addEventListener("change", onScheme); }
    else if (darkQuery.addListener) { darkQuery.addListener(onScheme); }
  }

  // Another tab changed a setting: follow it.
  window.addEventListener("storage", function (e) {
    if (e.key !== KEY) { return; }
    try { settings = sanitize(JSON.parse(e.newValue)); } catch (err) { settings = defaults(); }
    applyAll();
  });

  // ---------------------------------------------------------------- popover
  var pop = null, trigger = null;

  function el(tag, attrs, html) {
    var n = document.createElement(tag);
    for (var k in attrs) { if (Object.prototype.hasOwnProperty.call(attrs, k)) { n.setAttribute(k, attrs[k]); } }
    if (html != null) { n.innerHTML = html; }
    return n;
  }

  function switchRow(name, label, hint) {
    var id = "set-" + name.replace(/\./g, "-");
    return '<label class="switch-row" for="' + id + '">' +
      '<span class="switch-text"><span class="switch-label">' + label + "</span>" +
      (hint ? '<span class="switch-hint">' + hint + "</span>" : "") + "</span>" +
      '<input type="checkbox" role="switch" class="switch" id="' + id + '" data-setting="' + name + '" />' +
      "</label>";
  }

  function buildPopover() {
    var themes = [
      { v: "light", label: "Light", icon: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>' },
      { v: "system", label: "System", icon: '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/>' },
      { v: "dark", label: "Dark", icon: '<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/>' }
    ];
    var seg = "";
    for (var i = 0; i < themes.length; i++) {
      var t = themes[i];
      seg += '<label class="seg-option"><input type="radio" name="theme" value="' + t.v + '" data-setting="theme" />' +
        '<svg viewBox="0 0 24 24" aria-hidden="true">' + t.icon + "</svg><span>" + t.label + "</span></label>";
    }
    var rows = "";
    for (var j = 0; j < SECTIONS.length; j++) {
      rows += switchRow("sections." + SECTIONS[j].id, SECTIONS[j].label, SECTIONS[j].hint);
    }

    var html =
      '<div class="pop-head">' +
        '<h2 id="settings-title">Display settings</h2>' +
        '<button type="button" class="icon-btn" data-close aria-label="Close settings">' +
          '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg></button>' +
      "</div>" +
      '<div class="pop-body">' +
        '<fieldset class="pop-group"><legend>Theme</legend>' +
          '<div class="segmented">' + seg + "</div></fieldset>" +
        '<fieldset class="pop-group"><legend>Dates</legend>' +
          switchRow("relativeDates", "Show dates as relative time", "&ldquo;3 weeks ago&rdquo; instead of 2026-09-13; hover a date for the other form") +
        "</fieldset>" +
        '<fieldset class="pop-group"><legend>Sections on feature pages</legend>' + rows +
          '<p class="pop-note">Change stats and commit links also apply to the feature list.</p>' +
        "</fieldset>" +
      "</div>" +
      '<div class="pop-foot"><button type="button" class="btn btn-quiet btn-sm" data-reset>Reset to defaults</button>' +
        '<span class="pop-saved" aria-live="polite"></span></div>';

    pop = el("div", { id: "settings-popover", "class": "settings-pop", role: "dialog", "aria-labelledby": "settings-title" }, html);
    document.body.appendChild(pop);

    pop.addEventListener("change", function (e) {
      var input = e.target;
      var name = input.getAttribute("data-setting");
      if (!name) { return; }
      update(function (s) {
        if (name === "theme") { s.theme = input.value; }
        else if (name === "relativeDates") { s.relativeDates = input.checked; }
        else if (name.indexOf("sections.") === 0) { s.sections[name.slice(9)] = input.checked; }
      });
    });
    pop.querySelector("[data-reset]").addEventListener("click", function () {
      update(function (s) { var d = defaults(); s.theme = d.theme; s.relativeDates = d.relativeDates; s.sections = d.sections; });
      var note = pop.querySelector(".pop-saved");
      note.textContent = "Defaults restored";
      setTimeout(function () { note.textContent = ""; }, 2000);
    });
  }

  function syncControls() {
    if (!pop) { return; }
    var inputs = pop.querySelectorAll("[data-setting]");
    for (var i = 0; i < inputs.length; i++) {
      var input = inputs[i], name = input.getAttribute("data-setting");
      if (name === "theme") { input.checked = input.value === settings.theme; }
      else if (name === "relativeDates") { input.checked = settings.relativeDates; }
      else if (name.indexOf("sections.") === 0) { input.checked = !!settings.sections[name.slice(9)]; }
    }
  }

  function position() {
    var r = trigger.getBoundingClientRect();
    var top = Math.round(r.bottom + 8);
    pop.style.top = top + "px";
    pop.style.right = Math.max(16, Math.round(window.innerWidth - r.right)) + "px";
    pop.style.maxHeight = "calc(100vh - " + (top + 16) + "px)";
  }

  function focusFirst() {
    var target = pop.querySelector('input[name="theme"]:checked') || pop.querySelector("input, button");
    if (target) { target.focus(); }
  }

  var native = typeof HTMLElement !== "undefined" && Object.prototype.hasOwnProperty.call(HTMLElement.prototype, "popover");

  function isOpen() {
    if (native) { try { return pop.matches(":popover-open"); } catch (e) { return false; } }
    return !pop.hidden;
  }

  function closePopover(returnFocus) {
    if (!isOpen()) { return; }
    if (native) { pop.hidePopover(); } else { pop.hidden = true; trigger.setAttribute("aria-expanded", "false"); }
    if (returnFocus) { trigger.focus(); }
  }

  function wirePopover() {
    trigger = document.querySelector("[data-settings-trigger]");
    if (!trigger) { return; }
    buildPopover();
    syncControls();
    trigger.setAttribute("aria-controls", pop.id);
    trigger.setAttribute("aria-haspopup", "dialog");
    trigger.setAttribute("aria-expanded", "false");
    trigger.hidden = false;

    if (native) {
      // popover="auto" gives Esc, light dismiss on an outside click, the top
      // layer, and focus returning to the trigger -- popovertarget is what lets
      // a click on the trigger close it instead of dismiss-then-reopen.
      pop.setAttribute("popover", "auto");
      trigger.setAttribute("popovertarget", pop.id);
      pop.addEventListener("beforetoggle", function (e) {
        if (e.newState === "open") { syncControls(); position(); }
      });
      pop.addEventListener("toggle", function (e) {
        var open = e.newState === "open";
        trigger.setAttribute("aria-expanded", String(open));
        if (open) { focusFirst(); }
      });
    } else {
      pop.hidden = true;
      trigger.addEventListener("click", function () {
        if (isOpen()) { closePopover(true); return; }
        syncControls();
        pop.hidden = false;
        trigger.setAttribute("aria-expanded", "true");
        position();
        focusFirst();
      });
      document.addEventListener("keydown", function (e) {
        if (e.key === "Escape" && isOpen()) { closePopover(true); }
      });
      document.addEventListener("mousedown", function (e) {
        if (isOpen() && !pop.contains(e.target) && !trigger.contains(e.target)) { closePopover(false); }
      });
    }

    pop.querySelector("[data-close]").addEventListener("click", function () { closePopover(true); });
    // Tabbing out of a non-modal popover closes it rather than leaving it
    // floating over content the keyboard has moved on to.
    pop.addEventListener("focusout", function (e) {
      var next = e.relatedTarget;
      if (next && !pop.contains(next) && next !== trigger) { closePopover(false); }
    });
    window.addEventListener("resize", function () { if (isOpen()) { position(); } });
  }

  function onReady() {
    wirePopover();
    syncPictures();
    formatDates();
  }

  if (document.readyState === "loading") { document.addEventListener("DOMContentLoaded", onReady); }
  else { onReady(); }

  window.Mullion = {
    settings: function () { return settings; },
    sections: SECTIONS,
    onChange: function (fn) { listeners.push(fn); },
    formatDates: formatDates,
    syncPictures: syncPictures,
    timeTag: timeTag,
    relative: relative,
    effectiveTheme: effectiveTheme
  };
})();
