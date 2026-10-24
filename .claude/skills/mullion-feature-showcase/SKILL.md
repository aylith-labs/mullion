---
name: mullion-feature-showcase
description: The definition of done for anything user-visible that lands in Mullion — a new feature, a fix for an upstream issue, an upstream PR taken early. Use when a feature or fix is committed, before calling it done, and when asked to document, screenshot, showcase or backfill features on mullion.aylith.com (docs/). Covers the catalogue entry (docs/features.js), the long-form page (docs/feature-details.js), the upstream issue map (docs/upstream.html), before/after captures in light and dark (compare.js), the home page, the triage note in the notes repo, and the checker that enforces all of it (tools/check-site.mjs).
---

# Showcasing a Mullion feature

A feature is not done when CI is green. It is done when someone reading
mullion.aylith.com can see what it does, see it next to upstream, and try it.
Every user-visible change goes through the steps below, in the same push as the
feature or the one right after its verification. Backfilling past features
follows the same steps.

## 1. Catalogue entry — `docs/features.js`

One object per feature: `id` (kebab-case, permanent: it is the page URL),
`title` (what the user gets, not the implementation), `cat` + `catLabel` (reuse an
existing pair: automation, deploy, links, settings, ui), `kind` (`feature`: something
upstream cannot do; `improvement`: upstream does it, Mullion does it better; `fix`: an
upstream bug fixed), `dateAdded` (the date the
first commit landed), `commits`, `files`, `ins`, `del`, `desc` (two to four
sentences; name upstream issue numbers it answers). `standout: true` only for a
headline feature.

- Numbers come from git, never by hand:
  `node tools/feature-stats.mjs <sha> [<sha> ...]` prints the `commits`/`files`/
  `ins`/`del` fields (files = unique files touched).
- A fix or follow-up to an existing feature joins that feature's `commits` and
  its stats are recomputed; it does not get a card of its own.
- An upstream PR taken before it merges joins `early-upstream-fixes`.

## 2. Long-form page — `docs/feature-details.js`

Keyed by the same id: `problem`, `how`, `steps`, `settings`, `media`, `notes`,
`upstream`. Everything is checked against the code at HEAD:

- a settings key and its default are read from
  `src/cascadia/TerminalSettingsModel/MTSMSettings.h` / `defaults.json`, never from
  memory (the checker rejects a key the model does not declare);
- `upstream` says what Microsoft did, with issue and PR numbers.

## 3. Before/after, light and dark — `media`

Every feature has at least one comparison with upstream:

```js
{ kind: "compare", before: "<base>-upstream", after: "<base>", caption: "…" }
```

It renders with `docs/compare.js`: Hover, Split, Side by side and Difference
(hovering blends the two so only what changed lights up). Each side needs
`media/<base>-light.png` and `media/<base>-dark.png`. Until the before shot exists
the figure falls back to the after shot alone, so a page never looks worse.

**The four modes only where the pixels line up.** Hover, Split and Difference mean
something only when the two shots are the same size and the same scene, so the
feature's change is the only change (a link card over the same pane, a badge in the
same tab row). Otherwise pin the figure, which drops the buttons:
- `mode: "side"`: different layouts of the same thing (two settings pages, two
  outputs in different fonts, tabs moved to a sidebar);
- `mode: "stack"`: wide strips (a tab row) and text evidence panels;
- no upstream counterpart at all: a single `{ base, caption }` image, not a compare.

Look at both shots before choosing. A "before" that does not show the thing the
caption names (an upstream About pair once had no About dialog in it) is worse than
no before: drop it to a single image until it is retaken.

What "before" is, in order of preference:

1. **Upstream**: the same view in Windows Terminal Canary (installed, upstream's
   own build), same window size, same content.
2. **Mullion with the feature off**: when upstream has no such view (deploy and
   automation features), the setting switched off in `mult`.
3. **The evidence, rendered**: for something with no pixels (a UIA landmark, a
   pipe protocol, a CI run), the evidence (UIA tree, command output, run summary)
   laid out as a themed panel and captured in both themes.

The caption says what the reader should look at, before vs after, in one
sentence.

### Capturing

- **Only `mult` (Test slot) and Canary.** Never `muld`: it is production and its
  windows show live sessions. Refresh mult first with `tools\Refresh-TestSlot.ps1`
  when the feature is new.
- **Both themes** come from the Terminal's own setting, not the OS: set `"theme"`
  in mult's `settings.json`
  (`%LOCALAPPDATA%\Packages\WindowsTerminalTest_8wekyb3d8bbwe\LocalState\`) and in
  Canary's (`…\Microsoft.WindowsTerminalCanary_8wekyb3d8bbwe\LocalState\`) to
  `"light"` then `"dark"`; settings hot-reload, no restart. Back Canary's file up
  first and restore it after.
- **Same size, same content** on both sides, so Split and Difference line up:
  fixed window size (`--size`/`--pos` on launch), same profile, same font, same
  text in the pane.
- **`tools/Capture-Window.ps1`** takes the window's own pixels with PrintWindow
  (no focus change). The pane surface only renders while the window is visible,
  so a shot that shows pane content needs the window on screen, which means taking
  focus. That is allowed only when the user is idle: use AgentDriver
  (`~/projects/windows-control/agent-driver/`) for every gesture, re-check idle
  before each, and stop the moment it reports exit 2.
- **The promote button and dialog** exist only in `muld`, so mult shows them on
  request: set `MULLION_PROMOTE_PREVIEW` to a marker file (same schema as
  `dev-pending-ci.json`; `tools/promote-preview.sample.json` is a neutral one with
  feature and issue links) and start mult *from that shell*, with no mult already
  running — a running one owns the instance and never sees the variable:
  `$env:MULLION_PROMOTE_PREVIEW = "$PWD\tools\promote-preview.sample.json"; mult`.
  Only the Test package reads it. Every dialog button just closes the dialog. The
  age shown comes from the marker's `timestamp`, so refresh it in a copy for a
  recent-looking shot.
- Captures are taken at 150% display scaling (the page shows them at size ÷ 1.5).
- **Virtual desktop 2 keeps no-input shots off the user's screen.** A `mult` window moved
  there still captures correctly with PrintWindow, and UI Automation reads it. But
  **anything that activates the window switches the user's visible desktop to it**: a
  summon hotkey, a menu item picked through UIA, a click. It happened in three of three
  capture runs. Do activating steps on desktop 1 inside an idle-gated AgentDriver session,
  then park the window back on desktop 2. Never leave a hidden window holding focus.
- **Never close a capture window while it holds the foreground.** Windows hands the
  foreground to the next window in z-order, which can be `muld` with live sessions in it.
  Give the foreground back to the taskbar first, and refuse the close if that fails.
- **Hover shots:** `AgentDriver hover <x> <y> --dwell-ms 1200` inside a `session begin
  --target-hwnd`, capturing before `session end`. What it took to make them work:
  - Upstream's link tooltip opens only on a pointer *entering* the top ~12 px of the link's
    first cell. Approach from below, leave, come back up, then nudge 1-3 px.
  - XAML tooltips and link cards are separate popup windows (`Xaml_WindowedPopupClass`), so
    PrintWindow of the main window misses them. Capture each popup and composite it over the
    window shot.
  - A UIA Invoke/Expand/Select counts as outside input and ends the AgentDriver session.
  - A settings reload (e.g. the theme switch) leaves hover dead in that window: use a fresh
    window per theme.
- **Demo content must render well, not just exist.** A Markdown preview of a file that is
  not real Markdown reads as broken rendering.
- **Check every shot for work identifiers before committing.** Stage scenes with stand-in data
  (`ACME-8209`, `example.atlassian.net`), and OCR each PNG (`Windows.Media.Ocr`): an
  employer's Jira key once shipped in two screenshots.
- Privacy: no real accounts, tokens, work hosts or personal files in a shot.

## 4. Upstream map — `docs/upstream.html`

One row per upstream issue (`OPEN` or `CLOSED`) or PR (`PRS`) the feature
answers: `n`, `d` (opened), `t`, `f` (feature id), `full: true` when the ask as
written is met, and a `note` that says what is covered and what is not. Rows are
in number order. A feature that answers an issue only in part says which part.

## 5. Home page

Nothing to do by hand: `index.html` builds "Every feature, by area" and "Recently
added" from `features.js`. If the feature belongs in one of the hand-written
groups or spotlights, add a line there too.

## 6. The triage note

In the notes repo, the matching note under
`libraries/default/mullion/upstream/{issues,discussions}/` changes its verdict to
`shipped` (or `partial`) with `origin: fork` and the commit in `evidence:`; commit
it with `scripts/commit-triage-batches.sh` conventions (`--only`, exact paths).

## 7. Check, publish, verify live

```bash
node tools/check-site.mjs            # 0 errors; backfill lists captures still to take
node tools/check-site.mjs --strict   # for a new feature: no comparison = error
```

- Docs pushes do not trigger the build (`docs/**` is in build.yml's
  `paths-ignore`); `tools/` changes do.
- **When site.css or a script changes, bump the `?v=` token on every page's asset
  links.** Pages are cached for 10 minutes and a new page with an old stylesheet
  renders unstyled; that shipped once.
- After GitHub Pages deploys, run `node tools/audit-site.mjs` (the live site; pass a
  `file:///…/docs` URL for a local copy). It renders every page, list view, feature
  page and documentation topic in light, dark and at phone width, and fails on a
  broken image, two themes of one shot showing at once, a script error, a topic that
  did not load, or a page wider than a phone. Counting images is not enough: an
  earlier check did that and passed a page showing both themes.

## Documentation topics

The Settings UI's Documentation page and mullion.aylith.com/docs show the same
files: `docs/docs/<id>.md`, packaged into the app as `Docs\<id>.md` by
`src/cascadia/CascadiaResources.build.items`. Edit a topic there and both change.
A new topic needs a card in `TerminalSettingsEditor/Documentation.xaml`, its
`Documentation_Topic*` strings, and the same entry in `docs/docs/index.html`'s
`TOPICS`; `check-site.mjs` fails if the site's list and the app's drift apart.
Documentation pushes do not trigger a build, so the app picks a topic change up
with the next build that does.

## Links back from the app

The promote dialog links each commit to its feature page and upstream issue rows,
from `features.js` and `upstream.html` at build time (`tools/commit-links.mjs`, via
`tools/Write-ReleaseNotes.ps1`). That is one more reason a feature's `commits` and
its upstream rows must be complete: a commit missing from them gets no links.
