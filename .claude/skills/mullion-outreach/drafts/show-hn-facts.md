# Show HN: facts to write from (not text to post)

Hacker News bans generated or AI-edited text, so this is material for Steven's own words:
verified facts, an outline, and the gates. Nothing here is meant to be pasted.

## Gates (from log.md)

- Close-pane crash fix: in the portable release `portable-52713ba` (2026-10-23). Verify
  in a fresh portable copy (extract, run, close a pane with Ctrl+Shift+W) before posting.
- afterTabs vertical-strip bug: **unclear which bug this means.** fb072334c ("Paint the
  sidebar, so afterTabs does not show the desktop through it", 2026-09-09) is already in the
  release and predates the gate (2026-10-20), so the gate names something newer. Steven to
  say which, or check `tabsPosition` left/right with `afterTabs` content in the portable copy.
- Steven free to stay in the thread for the first few hours. Never ask for votes.

## Title shape

`Show HN: Mullion – <what it is, in plain words>`. HN's rules: no superlatives, nothing
editorialised, and say what it is. A Windows Terminal fork is the honest anchor.

## Facts, each checked against the code or the site on 2026-10-23

- A fork of microsoft/terminal, MIT, one maintainer, developed in the open:
  https://github.com/aylith-labs/mullion. 49 features catalogued with light and dark
  screenshots at https://mullion.aylith.com/features, each beside upstream where upstream
  has a counterpart.
- Install: a portable ZIP, with no MSIX and no Developer Mode needed. Settings live beside the
  exe. Windows 10 2004+, x64. https://github.com/aylith-labs/mullion/releases/latest
- Hover previews: GitHub issues and PRs (title, state, checks, comments), Jira keys, Slack
  permalinks, and local files (rendered Markdown, highlighted source, images, the first page
  of a PDF, Office text) in a card. A preview pane can follow the pointer.
- Paths are links: Windows, UNC, WSL `/home/...`, `~/` and `file://` paths resolve in the
  right distribution. A path in two distributions is shown as ambiguous rather than guessed.
- Tabs on any edge: top, bottom, or a resizable vertical sidebar left or right.
- Sessions come back: after a restart a pane reattaches the tmux or zellij session it was
  running, or a coding agent returns to its conversation. Otherwise its scrollback is repainted.
- A local control pipe (`\\.\pipe\wt-control-<pid>`): list panes, read what a pane printed,
  type into it, run an action. Every request must say why and who is asking, and it is logged
  on a Settings page.
- Settings UI: grouped settings, a filter, drag-to-reorder profiles (Ctrl+Shift+1-9 follow
  the order), and a Save button that lists what it would change.
- Upstream issue map: https://mullion.aylith.com/upstream lists the microsoft/terminal
  issues each feature answers, in full or in part.

## Things to be ready for in the comments

- "Why not upstream?" The fork evolves on its own terms. Some of it upstream would not want,
  and nothing is sent back. Avoid anything that reads as a dig at the maintainers.
- "Is it safe to run?" It is a portable ZIP built by GitHub Actions from public source. The
  release lists the source commit and its SHA-256.
- "Windows only?" Yes. It is a Windows Terminal fork.
- "Does it replace Windows Terminal?" **Unverified:** whether a portable copy starts while
  Windows Terminal is already open, or hands off to it (the window class keeps upstream's
  name). Test this in the portable copy before answering.

## Not to claim

- No numbers of users, stars or downloads.
- No "faster than", unless measured on the day.
- Nothing about the Microsoft block or the apologies.
