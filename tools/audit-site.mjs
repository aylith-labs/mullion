#!/usr/bin/env node
// Renders every page of mullion.aylith.com in headless Chrome, light and dark,
// desktop and phone, and fails on what a reader would see as broken:
//   - an image that failed to load and is still on screen (a broken-image icon)
//   - two themes of the same shot visible at once (a light and a dark copy)
//   - a script error, or a page wider than the screen on a phone
//
//   node tools/audit-site.mjs                      the live site
//   node tools/audit-site.mjs file:///C:/.../docs  a local copy
//
// Needs Chrome at its default path; drives it over CDP with no dependencies.
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const BASE = (process.argv[2] ?? "https://mullion.aylith.com").replace(/\/$/, "");
const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";
const PORT = 9350 + Math.floor(Math.random() * 40);
const profile = mkdtempSync(join(tmpdir(), "audit-site-"));
const chrome = spawn(CHROME, ["--headless=new", "--disable-gpu", "--allow-file-access-from-files", `--remote-debugging-port=${PORT}`, `--user-data-dir=${profile}`, "about:blank"]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let targets;
for (let i = 0; i < 60 && !targets; i++) {
  await sleep(250);
  try { targets = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); } catch {}
}
const ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0;
const pending = {};
const errors = [];
ws.onmessage = (m) => {
  const d = JSON.parse(m.data);
  if (d.method === "Runtime.exceptionThrown") errors.push(d.params.exceptionDetails.exception?.description?.split("\n")[0] ?? "script error");
  if (pending[d.id]) { pending[d.id](d.result); delete pending[d.id]; }
};
const cmd = (method, params = {}) => new Promise((r) => { pending[++id] = r; ws.send(JSON.stringify({ id, method, params })); });
const ev = async (x) => (await cmd("Runtime.evaluate", { expression: x, returnByValue: true, awaitPromise: true })).result?.value;
await cmd("Page.enable");
await cmd("Runtime.enable");

// Feature ids, from the catalogue the site itself serves.
async function featureIds() {
  await cmd("Page.navigate", { url: `${BASE}/features.html` });
  await sleep(1200);
  return ev("(window.FEATURES||[]).map(f=>f.id)");
}

const PROBE = `(async () => {
  // Let lazy images start: bring everything into range, then wait for them.
  document.querySelectorAll('img[loading=lazy]').forEach(i => i.loading = 'eager');
  await new Promise(r => setTimeout(r, 900));
  await Promise.all([...document.images].map(i => i.complete ? 0 : new Promise(r => { i.onload = i.onerror = r; setTimeout(r, 3000); })));
  const shown = el => { const s = getComputedStyle(el); const r = el.getBoundingClientRect(); return s.display !== 'none' && s.visibility !== 'hidden' && r.width > 0 && r.height > 0 && !el.closest('[hidden]'); };
  const broken = [...document.images].filter(i => shown(i) && i.complete && i.naturalWidth === 0).map(i => i.getAttribute('src'));
  // Two themes at once: inside one shot container, a visible light and a visible dark image.
  const doubled = [];
  for (const box of document.querySelectorAll('figure:not(.compare), .compare-layer, .thumb, picture')) {
    const l = [...box.querySelectorAll(':scope > .shot-light, :scope > img.shot-light')].some(shown);
    const d = [...box.querySelectorAll(':scope > .shot-dark, :scope > img.shot-dark')].some(shown);
    if (l && d) doubled.push((box.querySelector('img')||{}).getAttribute?.('src') || box.className);
  }
  const overflow = document.documentElement.scrollWidth - innerWidth;
  // A documentation topic that never rendered (its markdown failed to load).
  const failedDoc = !!document.querySelector('.mdoc .mdoc-loading');
  return { broken, doubled, overflow, failedDoc };
})()`;

async function audit(path, setup) {
  const problems = [];
  for (const [width, dark] of [[1300, false], [1300, true], [390, false]]) {
    await cmd("Emulation.setDeviceMetricsOverride", { width, height: 1000, deviceScaleFactor: 1, mobile: width < 600 });
    await cmd("Emulation.setEmulatedMedia", { features: [{ name: "prefers-color-scheme", value: dark ? "dark" : "light" }] });
    errors.length = 0;
    await cmd("Page.navigate", { url: `${BASE}/${path}` });
    await sleep(1200);
    if (setup) await ev(setup);
    const r = await ev(PROBE);
    const where = `${path} [${width}px ${dark ? "dark" : "light"}]`;
    for (const src of r?.broken ?? []) problems.push(`${where}: broken image ${src}`);
    for (const src of r?.doubled ?? []) problems.push(`${where}: both themes visible ${src}`);
    if (width < 600 && r?.overflow > 0) problems.push(`${where}: ${r.overflow}px wider than the screen`);
    if (r?.failedDoc && !BASE.startsWith("file:")) problems.push(`${where}: documentation topic did not load`);
    for (const e of errors) problems.push(`${where}: ${e}`);
  }
  return problems;
}

const pages = [
  ["index.html"], ["upstream.html"], ["mentions.html"],
  ["features.html?view=cards"], ["features.html?view=preview"], ["features.html?view=table"],
];
for (const fid of await featureIds()) pages.push([`feature.html?id=${fid}`]);
// The documentation, list and every topic (it fetches its markdown, so a
// file:// base only checks the list).
pages.push(["docs/"]);
for (const t of readFileSync(new URL("../docs/docs/index.html", import.meta.url), "utf8").matchAll(/\{ id: "([a-z0-9-]+)", title:/g))
  pages.push([`docs/?t=${t[1]}`]);

const all = [];
for (const [path, setup] of pages) {
  const p = await audit(path, setup);
  all.push(...p);
  process.stdout.write(p.length ? "x" : ".");
}
console.log(`\n${pages.length} pages x 3 renderings · ${all.length} problems`);
for (const p of all) console.log(`  ${p}`);
chrome.kill();
try { rmSync(profile, { recursive: true, force: true }); } catch {}
process.exit(all.length ? 1 : 0);
