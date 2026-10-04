# Mullion

**Windows Terminal, with more in it.** A personal fork of
[microsoft/terminal](https://github.com/microsoft/terminal) that evolves on its own terms:
features land here because they belong in *this* terminal, whether or not upstream would
ever take them. Named for the upright bar between window panes.

[![Build](https://github.com/aylith-labs/mullion/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/aylith-labs/mullion/actions/workflows/build.yml)
&nbsp;·&nbsp; **[mullion.aylith.com](https://mullion.aylith.com)**: the feature tour, with screenshots

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/media/link-card-dark.png">
  <img alt="A link tooltip card in Mullion, with a rendered preview and action buttons" src="docs/media/link-card-light.png" width="720">
</picture>

## What it adds

### Links that do something

- **Paths are links.** Windows, UNC and POSIX paths in the output are detected, `~/...`
  included, and resolved in the right WSL distribution. That includes `file://` links
  with fragments and `/mnt/<drive>/` paths. A path that exists in two distributions is
  shown as ambiguous rather than guessed.
- **Click without Ctrl, if you like**, even inside tmux and other mouse-mode apps. The
  primary and alternative click gestures, modifiers and actions are all configurable,
  and detection is separate from clickability.
- **A link card instead of a tooltip.** It has a show/hide delay and action buttons:
  Open, Copy link, Copy path, Show in Explorer and Show in pane. It renders Markdown and
  stays inside the pane on both axes.
- **File previews** for text, Markdown (rendered or raw), syntax-highlighted source and
  Office documents, plus images and media fitted to the card or to a dedicated preview
  pane.
- **Integrations** for GitHub, Jira, Slack, Stith, shefrd and Unblocked Code. An issue
  key, PR link or Slack permalink turns into a live preview, and Jira tables, callouts
  and comments survive the trip.
- **Tooltip rules** written as ICU regexes, with presets, per-rule actions and plain-text
  matching, so any token in the output can become a link.

Path policy, file types and integration manifests are shared with a sibling Tabby fork
through **Lintel**, so both terminals resolve the same text the same way.

### Tabs, windows and panes

- **Tab strip on any edge**, with the new-tab button where you want it, tab numbers, and
  window-level overrides for tab icons and close buttons.
- **Dock the window** to a screen edge at a fixed size, and **remember window
  geometry**, so each window reopens where it was.
- **Pane title bars** with draggable dividers, and a configurable **pane resize step**.
- **Safer closing.** Warn only above a tab threshold, and never ask a lone window whether
  to "close all windows".
- **Motion** follows Windows or overrides it in either direction.
- **Colour schemes in light/dark pairs** that follow the OS theme, with per-scheme
  control of how indistinguishable text is adjusted.

### Coming back to where you were

- **Session restore that resumes work.** Coding agents and multiplexers (tmux, screen,
  zellij, herdr, shefrd) are recognised, and restored panes attach or resume instead of
  starting a bare shell.
- **Pane contents saved periodically**, on a clock, not only at exit.

### Scriptable from outside

- **Control pipe.** A per-process named pipe, restricted to the current user, that can
  list panes, capture their text, send input and focus a pane. See
  [`doc/control-pipe.md`](doc/control-pipe.md).
- **Activity log.** A record of what the Terminal started and what was handed to it,
  readable in a pane and on its own Settings page.

### A Settings UI that knows what changed

- **New pages:** Link Tooltip, Integrations, Session Restore, Activity, and in-app
  Documentation.
- **Search** that is always a box and remembers what you chose. The shortcut list is
  searchable and lets you type a chord to find it.
- **Hover Save or Discard** to see exactly what would change.
- **Fork marks**, if you want them: a filled mark on every setting this fork adds, and a
  hollow ◇ on settings upstream supports but only exposes in JSON. The ones still
  without a UI are listed in [`doc/json-only-settings.md`](doc/json-only-settings.md).

## Getting a build

**Builds happen in CI, never locally.** A warm local tree runs to about 40 GB per
checkout. Every push to `main` runs [`build.yml`](.github/workflows/build.yml), which
builds and tests both slots, packages them, and uploads one artifact for both.

This machine runs two Terminals built from this repo, side by side:

| Slot | Alias | What it is |
|---|---|---|
| Dev | `muld` | The one I live in. It changes only when I press **promote** inside it. |
| Test | `mult` | Disposable. Refreshed on demand to verify a build in a window of its own. |

- [`tools/Fetch-CIBuild.ps1`](tools/Fetch-CIBuild.ps1) stages the newest successful
  build for both slots.
- [`tools/Install-CIBuildPoller.ps1`](tools/Install-CIBuildPoller.ps1) does that on a
  timer. The interval is a Settings option.
- `muld` notices a staged build and offers to promote it.
- [`tools/Refresh-TestSlot.ps1`](tools/Refresh-TestSlot.ps1) puts the staged Test build
  into `mult`.

When a build will not start, read [`doc/troubleshooting.md`](doc/troubleshooting.md)
first. For crashes and silent XAML failures, use the `terminal-fork-diagnostics` skill
in [`.claude/skills/`](.claude/skills/).

The process is `mullion.exe`. Package identities, the window class and credential keys
keep their Windows Terminal names on purpose, so existing settings and saved tokens
survive the rename.

## Working on it

[`AGENTS.md`](AGENTS.md) is the rulebook, for people and agents alike: the two slots,
what may and may not be restarted, line endings, CI etiquette, and the traps that each
cost a 40-minute CI round to learn.

After cloning, enable the hook that refuses commits which flip a file's line endings
wholesale:

```powershell
git config core.hooksPath tools/githooks
```

## Relationship to upstream

Upstream is **merged in**, never rebased onto, for its bug and security fixes. Its
issues and pull requests are read before anything here is built. **Nothing goes back:**
no pull requests to microsoft/terminal, and the `upstream` remote cannot push. Conflicts
resolve in favour of this fork's behaviour, and divergence is the point.

For the Terminal itself, its documentation and its history, see
[microsoft/terminal](https://github.com/microsoft/terminal).

## License

MIT, as upstream. See [LICENSE](LICENSE). Windows Terminal is © Microsoft Corporation;
this fork's changes are © their author.
