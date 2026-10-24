// The site's table and filter components, shared by features.html,
// upstream.html and mentions.html so every list sorts, filters and picks its
// fields the same way.
//
//   Mullion.dataTable(table, opts)   sortable columns with the arrow, an empty
//                                    row, and only the columns the picker shows
//   Mullion.fieldPicker(root, opts)  the Fields menu: a checkbox per column or
//                                    detail, remembered in this browser
//   Mullion.chipRow(el, opts)        one-of filter pills with counts
//   Mullion.searchBox(input, onText) "/" focuses it, Esc clears it
//
// A cell that carries a description renders it as <div class="desc"> beside a
// title marked class="has-desc": site.js shows it in a popover on hover while
// the table is compact, and the "desc" field (class "show-desc" on the table)
// shows it inline instead.
(function () {
  "use strict";

  function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  // columns: [{ key, label, cell(row) -> "<td>…</td>", cmp(a, b), first: 1|-1,
  //             sortable: false }]
  //   first   the direction of a first click (-1: newest or largest first)
  // opts: { columns, sort: { key, dir }, onSort(sort), tie(a, b), rowAttrs(row),
  //         visible(key) -> bool, empty, desc: bool }
  //   onSort  when the page owns the sort (features.html keeps it in the URL);
  //           otherwise the table keeps it
  function dataTable(table, opts) {
    var head = table.querySelector("thead");
    var body = table.querySelector("tbody");
    var sort = opts.sort || { key: opts.columns[0].key, dir: 1 };
    var rows = [];

    function columns() {
      return opts.columns.filter(function (c) { return !opts.visible || opts.visible(c.key); });
    }
    function byKey(key) { return opts.columns.filter(function (c) { return c.key === key; })[0]; }

    function render(list) {
      if (list) { rows = list; }
      var cols = columns();
      var col = byKey(sort.key);
      if (opts.desc !== undefined) { table.classList.toggle("show-desc", !!opts.desc()); }
      head.innerHTML = "<tr>" + cols.map(function (c) {
        if (c.sortable === false) {
          return '<th scope="col" data-key="' + c.key + '"><span class="plain">' + esc(c.label) + "</span></th>";
        }
        var aria = c.key === sort.key ? ' aria-sort="' + (sort.dir === 1 ? "ascending" : "descending") + '"' : "";
        return '<th scope="col" data-key="' + c.key + '"' + aria + '><button type="button">' + esc(c.label) + "</button></th>";
      }).join("") + "</tr>";
      var sorted = !col || !col.cmp ? rows : rows.slice().sort(function (a, b) {
        return col.cmp(a, b) * sort.dir || (opts.tie ? opts.tie(a, b) : 0);
      });
      body.innerHTML = sorted.length
        ? sorted.map(function (r) {
            return "<tr" + (opts.rowAttrs ? opts.rowAttrs(r) : "") + ">" + cols.map(function (c) { return c.cell(r); }).join("") + "</tr>";
          }).join("")
        : '<tr class="empty-row"><td colspan="' + cols.length + '">' + esc(opts.empty || "Nothing here matches the filter.") + "</td></tr>";
      if (window.Mullion && window.Mullion.formatDates) { window.Mullion.formatDates(body); }
    }

    head.addEventListener("click", function (e) {
      var th = e.target.closest("th[data-key]");
      if (!th || !th.querySelector("button")) { return; }
      var key = th.getAttribute("data-key");
      var next = key === sort.key ? { key: key, dir: -sort.dir } : { key: key, dir: byKey(key).first || 1 };
      if (opts.onSort) { opts.onSort(next); } else { sort = next; render(); }
      var again = head.querySelector('th[data-key="' + key + '"] button');
      if (again) { again.focus(); }
    });

    return {
      render: render,
      setSort: function (s) { sort = s; },
      sort: function () { return sort; },
      body: body
    };
  }

  // fields: [{ key, label, on }]. Builds the trigger and the menu inside root.
  function fieldPicker(root, opts) {
    var shown = {};
    opts.fields.forEach(function (f) { shown[f.key] = f.on; });
    try {
      var saved = JSON.parse(localStorage.getItem(opts.storageKey) || "null");
      if (saved) { opts.fields.forEach(function (f) { if (typeof saved[f.key] === "boolean") { shown[f.key] = saved[f.key]; } }); }
    } catch (e) { /* defaults */ }

    var id = opts.storageKey.replace(/[^a-z0-9]+/gi, "-") + "-pop";
    root.classList.add("fields-menu");
    root.innerHTML =
      '<button type="button" class="dropdown-trigger" aria-expanded="false" aria-controls="' + id + '"><span>Fields</span>' +
      '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 9l6 6 6-6"/></svg></button>' +
      '<div class="fields-pop" id="' + id + '" role="group" aria-label="Fields to show" hidden>' +
      opts.fields.map(function (f) {
        return '<label><input type="checkbox" data-field="' + f.key + '"' + (shown[f.key] ? " checked" : "") + " /> " + esc(f.label) + "</label>";
      }).join("") +
      '<button type="button" class="fields-reset">Reset</button></div>';
    var trigger = root.querySelector(".dropdown-trigger");
    var pop = root.querySelector(".fields-pop");

    function setOpen(open) { pop.hidden = !open; trigger.setAttribute("aria-expanded", String(open)); }
    function save() { try { localStorage.setItem(opts.storageKey, JSON.stringify(shown)); } catch (e) { /* not kept */ } }
    trigger.addEventListener("click", function () { setOpen(pop.hidden); });
    document.addEventListener("pointerdown", function (e) { if (!pop.hidden && !root.contains(e.target)) { setOpen(false); } });
    root.addEventListener("keydown", function (e) { if (e.key === "Escape" && !pop.hidden) { setOpen(false); trigger.focus(); } });
    pop.addEventListener("change", function (e) {
      var key = e.target.getAttribute("data-field");
      if (!key) { return; }
      shown[key] = e.target.checked;
      save();
      opts.onChange();
    });
    pop.querySelector(".fields-reset").addEventListener("click", function () {
      opts.fields.forEach(function (f) { shown[f.key] = f.on; });
      Array.prototype.forEach.call(pop.querySelectorAll("input[data-field]"), function (cb) { cb.checked = shown[cb.getAttribute("data-field")]; });
      save();
      opts.onChange();
    });

    return { shown: function (key) { return shown[key] !== false; } };
  }

  // options: [{ id, label }]; count(id) -> number, or null for no count.
  function chipRow(el, opts) {
    el.addEventListener("click", function (e) {
      var b = e.target.closest(".chip");
      if (b) { opts.onSelect(b.getAttribute("data-v")); }
    });
    return function paint(selected) {
      el.innerHTML = opts.options.map(function (o) {
        var n = opts.count ? opts.count(o.id) : null;
        return '<button type="button" class="chip" data-v="' + esc(o.id) + '" aria-pressed="' + (o.id === selected) + '">' +
          esc(o.label) + (n === null ? "" : ' <span class="count">' + n + "</span>") + "</button>";
      }).join("");
    };
  }

  // "/" jumps to the box, as on GitHub; Esc in it clears it.
  function searchBox(input, onText) {
    input.addEventListener("input", function () { onText(input.value.trim()); });
    input.addEventListener("keydown", function (e) {
      if (e.key === "Escape" && input.value) { input.value = ""; onText(""); }
    });
    document.addEventListener("keydown", function (e) {
      var a = document.activeElement;
      if (e.key === "/" && !e.ctrlKey && !e.metaKey && !e.altKey && a !== input &&
          !(a && (/^(INPUT|TEXTAREA|SELECT)$/.test(a.tagName) || a.isContentEditable))) {
        e.preventDefault();
        input.focus();
      }
    });
  }

  window.Mullion = window.Mullion || {};
  window.Mullion.dataTable = dataTable;
  window.Mullion.fieldPicker = fieldPicker;
  window.Mullion.chipRow = chipRow;
  window.Mullion.searchBox = searchBox;
})();
