---
name: mullion-outreach
description: Tell people about Mullion without getting flagged, banned or ignored. Use before any outreach step (awesome-list PRs, Show HN, Lobsters, Reddit, Stack Exchange answers, AlternativeTo, blog posts, replying anywhere with a link to Mullion), before drafting text that will be posted under Steven's name, and when deciding the next step. Holds the hard rules (nothing in Microsoft repos, one step a day, no AI-written prose where a community bans it), what each outlet accepts, how to check a claim before making it, the watchers that report responses, and log.md, the running record of every step taken and what it taught.
---

# Mullion outreach

Every step here is public, under Steven's name, and hard to take back. The rules
below were paid for; read them before drafting anything, and add to them after.

## Hard rules

1. **Nothing in any Microsoft-org repo. Ever.** On 2026-10-19 ten polite, accurate
   replies on microsoft/terminal issues, each linking to a feature page, got the
   account blocked from the whole Microsoft GitHub org. Steven had already told the
   maintainers in #20390 that he would stop; a maintainer wrote "last warning", and a
   scheduled script posted again 20 minutes later. That includes microsoft/winget-pkgs,
   so winget is out too. The block is not to be worked around: no second account, no
   asking someone else to post.
2. **One outreach step per day.** Before each, read how the previous one landed
   (log.md, the watchers) and adjust the wording.
3. **Never automate posting.** No loop, schedule or queue that posts on its own. If
   several posts are approved, post them one at a time and check for responses
   between them. A reply that says stop ends the whole batch.
4. **Stop at the first sign of "no".** A warning, a "please don't", a closed or
   locked thread, a downvote pile: stop, tell Steven, and do not reply in kind.
5. **Prose is Steven's own.** Hacker News ("don't post generated or AI-edited
   text"), Lobsters (flags content "created without meaningful human authorship"),
   Stack Exchange (bans generative-AI answers network-wide) and
   0PandaDEV/awesome-windows (rejects AI-made PRs and "vibecoded slop") all forbid
   it. For those, give Steven facts and an outline, never finished text. One-line
   list entries in a list's own format are not prose, and are fine to prepare.
6. **No AI attribution anywhere**, per AGENTS.md. Rule 5 is about who writes the
   words, not about crediting tools.

## Before any step

- **Check every claim against the code at HEAD**, not against the docs pages: the
  setting key, its default, what is and is not covered. Say what is missing when the
  match is partial. An agent reviewing the 2026-10-19 replies found the docs had
  overclaimed once (Slack-style links were said to render as their label; they don't).
- **Read the venue's own rules today**, not from memory: CONTRIBUTING, the subreddit's
  `/about/rules` (Reddit blocks unauthenticated reads, so Steven checks while logged
  in), Show HN's page.
- **Disclose authorship** ("my fork", "I made it") everywhere.
- **Look at what newcomers will hit first.** The portable ZIP is what most people
  download. Before a big post (Show HN), it must come from a recent green build, and
  known crashes must be fixed first (the close-pane crash fixed in a9f899e0d would
  have crashed the window within minutes of anyone trying it).
- **Privacy in anything shown**: no real accounts, tokens, work hosts, other apps on
  the desktop, or personal files in screenshots. The Jira and Slack integration pages
  show the signed-in user and the work site, and must be cropped below the account
  block or not used.

## Outlets

| Outlet | Fit | Notes |
|---|---|---|
| [cdleon/awesome-terminals](https://github.com/cdleon/awesome-terminals) | yes | Windows section, alphabetical, `- [Name](repo) - Description. [site](site)`. Merges in batches every 2-6 weeks; 43 PRs were open on 2026-10-20. Don't ping. |
| [sirredbeard/awesome-wsl](https://github.com/sirredbeard/awesome-wsl) | yes | Terminals section. Wants "good and notable or unique" and active development; lead with WSL features (WSL file:// links, path resolution per distro, session resume). Include its trademark note. |
| [thechampagne/awesome-windows](https://github.com/thechampagne/awesome-windows) | yes | Terminal section, no star minimum, OSS icon, description ends with a full stop. |
| 0PandaDEV/awesome-windows | risky | Bans AI-made PRs. Only if Steven writes and submits it himself. |
| AlternativeTo | yes, Steven submits | "Suggest new application" as an alternative to Windows Terminal. Clones are declined, so lead with what differs. |
| Show HN | yes, Steven writes | Must be runnable and non-trivial; Steven stays to answer comments; never ask for votes. |
| Lobsters | not yet | Invite-only; new users (70 days) can't self-promote or use `show`. |
| Reddit | check first | r/commandline first, one subreddit at a time, a week apart; rules read logged in. |
| Stack Exchange | Steven writes | Only 4 questions genuinely fit (2026-10-20): superuser 1815003, 1777569, 1892849; stackoverflow 78253082. Each answer must stand alone, name Mullion as one option, disclose authorship. |
| Scoop Extras | no | Needs 100 stars or 50 forks. A bucket of our own is the alternative. |
| winget | no | microsoft/winget-pkgs (rule 1). |

## Assets

- Feature pages with light and dark screenshots: `https://mullion.aylith.com/feature.html?id=<id>`;
  the list is `docs/features.js`. Images are in `docs/media/<base>-{light,dark}.png`,
  so GitHub markdown can show the reader's theme with `#gh-light-mode-only` /
  `#gh-dark-mode-only` suffixes on two images side by side.
- README banner: `docs/media/readme-hero-{light,dark}.png` (1280x640, also usable as
  the GitHub social preview, which only the web UI can set).
- Repo: homepage, description and topics are set; issues are enabled.

## Watching for responses

- **stith watches** (WSL): `stith watch arm --command "<gh probe>" --repeat ...`. They
  can only raise a desktop toast here, not resume a session. One per open PR or
  thread, e.g. 2b25a099 on awesome-terminals#139, 27b67434 on any Microsoft GitHub
  notification.
- **Inbound issues and PRs on aylith-labs/mullion**: `tools/Watch-Contributions.ps1`
  (Scheduled Task "Mullion contribution watcher") reviews each adversarially with
  read-only tools, then raises a notification with buttons. Reviews are in
  `%LOCALAPPDATA%\MullionWatch\reviews`.
- **The Microsoft apology** (sent 2026-10-20 to opencode@microsoft.com from
  hello@aylith.com) is watched by a cloud routine that emails steven@aylith.com if a
  reply arrives. Do not follow up, chase, or contact anyone else about the block;
  that is what caused it.

## After every step

Append to `log.md` in this folder: date, step, link, what happened, and the lesson if
any. Update the rules above when a lesson generalises. The log is how the next
session knows what has already been tried.
