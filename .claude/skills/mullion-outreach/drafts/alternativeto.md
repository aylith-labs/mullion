# AlternativeTo: fields for "Suggest new application"

Steven submits this through the web form at https://alternativeto.net (signed in, then "Suggest
new application"). It is a directory listing, so prepared text is fine here. AlternativeTo
declines clones, so the description leads with what differs from Windows Terminal. Every claim
was checked against `main` on 2026-11-01.

| Field | Value |
|---|---|
| Name | Mullion |
| Website | https://mullion.aylith.com |
| Alternative to | Windows Terminal (it is also a fair alternative to Tabby and ConEmu, but name one) |
| Platforms | Windows |
| License | Open source (MIT), free |
| Source code | https://github.com/aylith-labs/mullion |
| Categories | OS & Utilities (the same as Windows Terminal's listing) |
| Features (tags) | Tabbed interface, WSL integration, Dark Mode, Portable, Session restore, Link preview, Vertical tabs, Ad-free |
| Icon | `drafts/mullion-icon-256.png`, the site's own mark. Never the `res/terminal/images/` tiles: those are upstream's Windows Terminal icon |
| Screenshots | `docs/media/integration-previews-github-dark.png`, `docs/media/tab-edges-left-dark.png`, `docs/media/file-previews-markdown-dark.png` (dark shows best on their white page; none show accounts or work names) |

## Short description (tagline)

A fork of Windows Terminal with hover previews for links and files, tabs on any edge, and sessions that come back after a restart.

## Description

Mullion is my fork of Windows Terminal, free and MIT-licensed, with the features I wanted in my own terminal:

- Hover previews: GitHub issues and PRs, Jira keys, Slack links and local files (rendered Markdown, highlighted source, images, PDFs) open in a card without leaving the terminal.
- Paths are links: Windows, UNC, WSL and ~/ paths open in the right WSL distribution.
- Tabs on any edge: top, bottom, or a resizable sidebar on the left or right.
- Sessions come back: after a restart a pane reattaches the tmux or zellij session it was running.
- A Settings UI with a filter, drag-to-reorder profiles, and a Save button that lists what will change.

It runs from a portable ZIP: no installer, no Developer Mode. Every feature is shown beside upstream Windows Terminal, in light and dark, at mullion.aylith.com. Not affiliated with or endorsed by Microsoft.

## Before submitting

- The portable ZIP (`portable-52713ba`, 2026-10-23) is 30 commits behind `main`. It is fine
  to submit with it; a fresh release from a green build would also carry the link-card fixes.
- After it is approved, add the listing to `docs/mentions.html` and a row to `log.md`.
