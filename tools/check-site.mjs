#!/usr/bin/env node
// Checks mullion.aylith.com (docs/) against the feature blueprint: every
// feature is in the catalogue, documented, shown in both themes, compared with
// upstream, and mapped to the upstream issues it answers. See
// .claude/skills/mullion-feature-showcase/SKILL.md for what each rule is for.
//
//   node tools/check-site.mjs            errors fail; missing before/after
//                                        comparisons are listed as backfill
//   node tools/check-site.mjs --strict   a feature without a comparison fails
//
// Errors are things that are wrong (a broken image path, a settings key the
// code does not have, stats that do not match git). Backfill is work not yet
// done; --strict is for a newly added feature, which may not ship without it.
import { execFileSync } from "node:child_process";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { meanLuma } from "./png-luma.mjs";

// Theme check thresholds, from the site's own captures: every real dark shot
// measures under 0.40 and every light one at least 0.15 above its dark twin.
const DARK_MAX = 0.5;
const LIGHT_OVER_DARK = 0.15;

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const DOCS = join(ROOT, "docs");
const strict = process.argv.includes("--strict");
const errors = [];
const backfill = [];
const err = (id, msg) => errors.push(`${id}: ${msg}`);

function load(file, name) {
  const window = {};
  // biome-ignore lint/security/noGlobalEval: the site's own data files, by design plain scripts
  eval(readFileSync(join(DOCS, file), "utf8"));
  return window[name];
}
const FEATURES = load("features.js", "FEATURES");
const DETAILS = load("feature-details.js", "FEATURE_DETAILS");

const git = (...args) => execFileSync("git", args, { cwd: ROOT, encoding: "utf8" });
function stats(shas) {
  const files = new Set();
  let ins = 0;
  let del = 0;
  for (const sha of shas) {
    for (const line of git("show", "--numstat", "--format=", sha).split(/\r?\n/)) {
      const m = /^(\d+|-)\t(\d+|-)\t(.+)$/.exec(line);
      if (!m) continue;
      files.add(m[3]);
      if (m[1] !== "-") ins += Number(m[1]);
      if (m[2] !== "-") del += Number(m[2]);
    }
  }
  return { files: files.size, ins, del };
}

// Settings keys the code actually has: MTSMSettings.h and defaults.json name
// nearly all of them; a grep of the settings model catches the rest.
const settingsSource = ["src/cascadia/TerminalSettingsModel"]
  .flatMap((dir) => readdirSync(join(ROOT, dir)).filter((f) => /\.(h|cpp|json|idl)$/.test(f)).map((f) => join(ROOT, dir, f)))
  .map((f) => readFileSync(f, "utf8"))
  .join("\n");

const media = (base) => existsSync(join(DOCS, "media", `${base}`));
const ids = new Set();
for (const f of FEATURES) {
  const id = f.id;
  if (ids.has(id)) err(id, "duplicate id");
  ids.add(id);
  for (const key of ["title", "cat", "catLabel", "dateAdded", "desc"]) if (!f[key]) err(id, `features.js is missing ${key}`);
  if (!["feature", "improvement", "fix"].includes(f.kind)) err(id, `kind "${f.kind}" is not feature, improvement or fix`);
  if (!/^\d{4}-\d{2}-\d{2}$/.test(f.dateAdded ?? "")) err(id, `dateAdded "${f.dateAdded}" is not YYYY-MM-DD`);
  // A hub's page lists other entries, each with commits, media and upstream
  // rows of its own; it carries none of those itself.
  if (f.hub) {
    if (!Array.isArray(f.hub) || f.hub.length === 0) err(id, "hub lists no entries");
    for (const m of f.hub) {
      const member = FEATURES.find((x) => x.id === m);
      if (!member) err(id, `hub lists unknown entry "${m}"`);
      else if (member.hub) err(id, `hub lists another hub, "${m}"`);
    }
    for (const key of ["commits", "files", "ins", "del"]) if (key in f) err(id, `a hub has no ${key}; its entries carry them`);
    if (DETAILS[id]?.media) err(id, "a hub has no media; its entries carry them");
    continue;
  }
  if (!Array.isArray(f.commits) || f.commits.length === 0) err(id, "no commits");
  else {
    let ok = true;
    for (const sha of f.commits) {
      try { git("cat-file", "-e", `${sha}^{commit}`); } catch { err(id, `commit ${sha} does not exist`); ok = false; }
    }
    if (ok) {
      const s = stats(f.commits);
      if (s.files !== f.files || s.ins !== f.ins || s.del !== f.del)
        err(id, `stats say files ${f.files}, +${f.ins} -${f.del}; git says files ${s.files}, +${s.ins} -${s.del} (node tools/feature-stats.mjs ${f.commits.join(" ")})`);
    }
  }

  const d = DETAILS[id];
  if (!d) { err(id, "no feature-details.js entry"); continue; }
  for (const key of ["problem", "how"]) if (!d[key]) err(id, `feature-details.js is missing ${key}`);
  let compares = 0;
  for (const m of d.media ?? []) {
    if (m.kind === "compare") {
      compares++;
      if ("mode" in m) err(id, `compare ${m.after}: "mode" is gone; mark a pixel-aligned pair aligned: true, or leave it out`);
      for (const side of ["before", "after"])
        for (const theme of ["light", "dark"])
          if (!m[side] || !media(`${m[side]}-${theme}.png`)) backfill.push(`${id}: compare ${side} ${theme} capture media/${m[side]}-${theme}.png not taken yet`);
    } else if (m.kind === "evidence") {
      // Text evidence is a table in the page, not a file: check its shape.
      if (m.before) compares++;
      const cols = m.columns?.length ?? 0;
      if (!cols) err(id, "evidence has no columns");
      for (const side of ["before", "after"]) {
        const t = m[side];
        if (!t) { if (side === "after") err(id, "evidence has no after table"); continue; }
        if (!t.label || !t.rows?.length) err(id, `evidence ${side} table needs a label and rows`);
        for (const r of t.rows ?? []) if (r.length !== cols) err(id, `evidence ${side} row has ${r.length} cells, not ${cols}: ${JSON.stringify(r)}`);
      }
    } else {
      const ext = m.kind === "video" ? "mp4" : "png";
      for (const theme of ["light", "dark"])
        if (!media(`${m.base}-${theme}.${ext}`)) err(id, `media/${m.base}-${theme}.${ext} is missing`);
    }
    if (!m.caption) err(id, `a media item (${m.base ?? m.after}) has no caption`);
  }
  if (compares === 0) (strict ? errors : backfill).push(`${id}: no before/after comparison with upstream`);
  for (const s of d.settings ?? []) {
    // Keys are declared whole ("hyperlink.tooltipShowDelay"). A profile key
    // documented with its JSON path ("profiles.defaults.colorScheme") and a
    // "key[...]" suffix are trimmed first.
    const key = String(s.key).replace(/^profiles\.(defaults|list\[\])\./, "").replace(/\[.*$/, "").trim();
    if (!settingsSource.includes(`"${key}"`)) err(id, `settings key "${s.key}" is not in the settings model`);
  }
}

// upstream.html rows point at real features, never at a hub (a row belongs
// on the page of the entry that answers it), and every related issue a row
// lists (rel) has its title and date in RELATED.
const up = readFileSync(join(DOCS, "upstream.html"), "utf8");
const relStart = up.indexOf("var RELATED = {");
const RELATED = relStart < 0 ? {} : eval(`(${up.slice(up.indexOf("{", relStart), up.indexOf("\n  };", relStart) + 4)})`);
const hubs = new Set(FEATURES.filter((f) => f.hub).map((f) => f.id));
for (const name of ["OPEN", "PRS", "CLOSED"]) {
  const start = up.indexOf(`var ${name} = [`);
  if (start < 0) { err("upstream.html", `no ${name} list`); continue; }
  const open = up.indexOf("[", start);
  const rows = eval(up.slice(open, up.indexOf("\n  ];", open) + 4));
  for (const r of rows) {
    if (!ids.has(r.f)) err("upstream.html", `#${r.n} points at unknown feature "${r.f}"`);
    else if (hubs.has(r.f)) err("upstream.html", `#${r.n} points at the hub "${r.f}"; point it at the entry that answers it`);
    for (const n of r.rel ?? []) {
      const i = RELATED[n];
      if (!i?.t || !/^(open|closed)$/.test(i.s ?? "") || !/^\d{4}-\d{2}-\d{2}$/.test(i.d ?? "")) err("upstream.html", `#${r.n} lists related #${n} with no title, state and date in RELATED`);
    }
  }
}

// A capture must look like the theme its name says. One Canary shot whose
// theme had not switched once shipped as a "-dark" image with light pixels.
// A dark capture is mostly dark, and its light twin is clearly lighter; a
// pair whose two files are byte-identical is theme-neutral and passes.
{
  const dir = join(DOCS, "media");
  for (const file of readdirSync(dir).filter((f) => f.endsWith("-dark.png"))) {
    const base = file.slice(0, -"-dark.png".length);
    const lightFile = `${base}-light.png`;
    const dark = readFileSync(join(dir, file));
    if (existsSync(join(dir, lightFile)) && dark.equals(readFileSync(join(dir, lightFile)))) continue;
    const d = meanLuma(join(dir, file));
    if (d != null && d > DARK_MAX) {
      err(`media/${file}`, `mean luminance ${d.toFixed(2)} is a light image named dark (dark captures are below ${DARK_MAX}); retake it with the Terminal's theme set to dark`);
      continue;
    }
    if (!existsSync(join(dir, lightFile))) continue;
    const l = meanLuma(join(dir, lightFile));
    if (d != null && l != null && l - d < LIGHT_OVER_DARK)
      err(`media/${lightFile}`, `mean luminance ${l.toFixed(2)} is not clearly lighter than its dark twin's ${d.toFixed(2)}; retake it with the Terminal's theme set to light`);
  }
}

// Documentation: mullion.aylith.com/docs and the Settings UI's Documentation
// page show the same files (docs/docs/<id>.md, packaged by
// CascadiaResources.build.items). The site's topic list must match the app's
// order (Documentation.xaml's Tag values) and words (Documentation_Topic*).
{
  const xaml = readFileSync(join(ROOT, "src/cascadia/TerminalSettingsEditor/Documentation.xaml"), "utf8");
  const resw = readFileSync(join(ROOT, "src/cascadia/TerminalSettingsEditor/Resources/en-US/Resources.resw"), "utf8");
  const app = [...xaml.matchAll(/x:Uid="(Documentation_Topic\w+)"[\s\S]*?Tag="([a-z0-9-]+)"/g)].map(([, uid, id]) => {
    const value = (key) => (new RegExp(`<data name="${uid}\\.${key}"[^>]*>\\s*<value>([^<]*)</value>`).exec(resw) ?? [])[1];
    return { id, title: value("Header"), desc: value("Description") };
  });
  const page = readFileSync(join(DOCS, "docs", "index.html"), "utf8");
  const site = [...page.matchAll(/\{ id: "([a-z0-9-]+)", title: "([^"]*)", icon: "[a-z]+",\s*desc: "([^"]*)" \}/g)].map(([, id, title, desc]) => ({ id, title, desc }));
  if (app.length === 0) err("docs", "no topics found in Documentation.xaml");
  if (JSON.stringify(app.map((t) => t.id)) !== JSON.stringify(site.map((t) => t.id)))
    err("docs", `topic order differs: app ${app.map((t) => t.id).join(",")} / site ${site.map((t) => t.id).join(",")}`);
  for (const t of app) {
    const s = site.find((x) => x.id === t.id);
    if (!existsSync(join(DOCS, "docs", `${t.id}.md`))) err("docs", `docs/docs/${t.id}.md is missing (the app shows "not installed")`);
    if (s && (s.title !== t.title || s.desc !== t.desc)) err("docs", `${t.id}: title or description differs from the app's Documentation_Topic strings`);
  }
}

for (const e of errors) console.log(`ERROR  ${e}`);
for (const b of backfill) console.log(`TODO   ${b}`);
console.log(`\n${FEATURES.length} features · ${errors.length} errors · ${backfill.length} backfill items`);
process.exit(errors.length ? 1 : 0);
