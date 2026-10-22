#!/usr/bin/env node
// Which mullion.aylith.com pages a commit belongs on: the features whose
// catalogue entry lists it (docs/features.js) and the upstream issues it
// answers. tools\Write-ReleaseNotes.ps1 attaches the result to each commit, so
// the promote dialog can link a commit to its feature page and issue rows.
//
//   node tools/commit-links.mjs <sha> [<sha> ...]
//
// Prints {"<sha as given>": {"features": [{"id","title"}], "issues": [n],
// "refs": [n]}} for every sha with at least one link. "issues" have a row on
// upstream.html; "refs" are upstream issues the commit names that have none,
// for a link straight to GitHub. A commit that names issues (GH#1234) gets
// those; one that names none gets its feature's rows.
//
// Never fails on history it cannot see: CI clones a few hundred commits deep,
// so a catalogued sha older than that is simply skipped, as is an unknown one.
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const shas = process.argv.slice(2);
const git = (args, input) => execFileSync("git", args, { cwd: ROOT, encoding: "utf8", input, maxBuffer: 64 << 20 });

function load(file, name) {
  const window = {};
  // biome-ignore lint/security/noGlobalEval: the site's own data file, a plain script by design
  eval(readFileSync(join(ROOT, "docs", file), "utf8"));
  return window[name] ?? [];
}

// sha -> full sha, or nothing for one this clone does not have. cat-file
// prints "<sha>^{commit} missing" for those instead of failing the batch.
function resolve(list) {
  const out = new Map();
  if (!list.length) return out;
  const lines = git(["cat-file", "--batch-check=%(objectname)"], list.map((s) => `${s}^{commit}`).join("\n") + "\n").split(/\r?\n/);
  list.forEach((s, i) => { if (/^[0-9a-f]{40}$/.test(lines[i] ?? "")) out.set(s, lines[i]); });
  return out;
}

const features = load("features.js", "FEATURES");
const fullOf = resolve([...new Set(features.flatMap((f) => f.commits ?? []))]);
const featuresByCommit = new Map();
for (const f of features)
  for (const s of f.commits ?? []) {
    const full = fullOf.get(s);
    if (!full) continue;
    if (!featuresByCommit.has(full)) featuresByCommit.set(full, []);
    featuresByCommit.get(full).push({ id: f.id, title: f.title });
  }

// Feature id -> issue numbers with a row, and every number that has a row.
const issuesByFeature = new Map();
const onSite = new Set();
const up = readFileSync(join(ROOT, "docs", "upstream.html"), "utf8");
for (const name of ["OPEN", "PRS", "CLOSED"]) {
  const start = up.indexOf(`var ${name} = [`);
  if (start < 0) continue;
  const open = up.indexOf("[", start);
  for (const row of eval(up.slice(open, up.indexOf("\n  ];", open) + 4))) {
    if (!issuesByFeature.has(row.f)) issuesByFeature.set(row.f, new Set());
    issuesByFeature.get(row.f).add(row.n);
    onSite.add(row.n);
  }
}

// Every asked-for commit's message, in one git call.
const asked = resolve(shas);
const messages = new Map();
if (asked.size) {
  const out = git(["log", "--no-walk=unsorted", "--format=%H%x00%B%x1e", ...new Set(asked.values())]);
  for (const record of out.split("\x1e")) {
    const [full, body] = record.replace(/^\s+/, "").split("\0");
    if (full) messages.set(full, body ?? "");
  }
}

const result = {};
for (const sha of shas) {
  const full = asked.get(sha);
  if (!full) continue;
  const linked = featuresByCommit.get(full) ?? [];
  const named = [...(messages.get(full) ?? "").matchAll(/\b(?:GH|microsoft\/terminal)#(\d{2,6})\b/g)].map((m) => Number(m[1]));
  const numbers = new Set(named.length ? named : linked.flatMap((f) => [...(issuesByFeature.get(f.id) ?? [])]));
  const issues = [...numbers].filter((n) => onSite.has(n)).sort((a, b) => a - b);
  const refs = [...numbers].filter((n) => !onSite.has(n)).sort((a, b) => a - b);
  if (linked.length || issues.length || refs.length) result[sha] = { features: linked, issues, refs };
}
process.stdout.write(JSON.stringify(result));
