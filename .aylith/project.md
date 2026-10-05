---
name: Mullion
tagline: Windows Terminal, with more in it
description: >-
  A Windows Terminal fork that evolves on its own terms: agent-aware links and
  cards, sessions that come back after a crash, a control pipe for outside
  automation, and a Settings UI grouped into sections.
category: developer-tools
websiteUrl: https://mullion.aylith.com/
status: building
features:
  - 'Brings back what was actually running: agents and multiplexers resume after a crash or close'
  - 'A local control pipe, so an outside process can type into or focus a specific pane'
  - 'Plain-click links everywhere, Slack-style bracketed links, and WSL file:// resolution'
  - 'Link cards for GitHub, Jira, Slack and stith that fit their content'
  - 'Per-window position and size, and separate light and dark color schemes'
  - 'A Settings UI grouped into collapsible sections, with its open state remembered'
  - 'Two side-by-side slots: a disposable test build and a promote-when-ready daily driver'
targetUser: >-
  Windows developers who live in the terminal with coding agents and
  multiplexers, and want more than stock Windows Terminal ships
featured: false
icon: 'M3.75 4.5h16.5v15H3.75z M12 4.5v15 M3.75 12h16.5'
gradientFrom: '#0f766e'
gradientTo: '#5eead4'
---

## Vision

Windows Terminal is a good terminal that moves at the pace of a large product. Mullion is the same
codebase, moving at the pace of one person who runs coding agents in it all day. Upstream fixes
still flow in. Features land here because they are useful here, whether or not upstream would ever
take them.

## The Problem

A modern terminal session is not just a shell. It holds agents, multiplexers and long-running
work, and it is full of references: pull requests, tickets, chat threads, file paths inside WSL.
Stock Windows Terminal forgets what was running when it closes, treats most of those references as
plain text, and offers no way for another process to drive a specific pane.

## Key Differentiators

- **Sessions that survive**: agents and multiplexers are resumed with the flags they started with.
- **References you can act on**: links resolve across WSL, and cards show what a link points at.
- **Drivable from outside**: a local control pipe lets tools target an exact pane.
- **Safe to develop in daily**: a disposable test slot beside the production slot, with promotion
  as a deliberate gesture rather than a side effect of a build.
