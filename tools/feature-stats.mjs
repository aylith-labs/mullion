#!/usr/bin/env node
// The numbers a docs/features.js entry carries, measured from git rather than
// estimated: files touched, lines added and removed across a feature's commits.
//
//   node tools/feature-stats.mjs <sha> [<sha> ...]
//
// A file touched by several commits counts once (it is "files touched");
// insertions and deletions are summed per commit, as `git show --numstat`
// reports them. Binary files count as files with no lines.
import { execFileSync } from "node:child_process";

const shas = process.argv.slice(2);
if (shas.length === 0) {
  console.error("usage: node tools/feature-stats.mjs <sha> [<sha> ...]");
  process.exit(2);
}

const files = new Set();
let ins = 0;
let del = 0;
for (const sha of shas) {
  const out = execFileSync("git", ["show", "--numstat", "--format=", sha], { encoding: "utf8" });
  for (const line of out.split(/\r?\n/)) {
    const m = /^(\d+|-)\t(\d+|-)\t(.+)$/.exec(line);
    if (!m) continue;
    files.add(m[3]);
    if (m[1] !== "-") ins += Number(m[1]);
    if (m[2] !== "-") del += Number(m[2]);
  }
}
const short = shas.map((s) => execFileSync("git", ["rev-parse", "--short=9", s], { encoding: "utf8" }).trim());
console.log(`commits: ${JSON.stringify(short)}, files: ${files.size}, ins: ${ins}, del: ${del},`);
