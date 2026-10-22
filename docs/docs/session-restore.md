# Session restore

Session restore decides what comes back when the Terminal starts: which windows and panes,
what was on screen in them, and which of the programs that were running are started again.

One setting governs all of it — **When Terminal starts** — and everything else on the page
follows from what you choose there.

## When Terminal starts

| Choice | Comes back |
|---|---|
| Open a tab with the default profile | Nothing. A fresh window, every time. |
| Open windows from a previous session | The windows and panes you had, empty. |
| Restore window layout | The layout, with no scrollback. |
| Restore window layout and content | The layout *and* what was on screen in each pane. |

Only the last one restores pane contents, so *Saving pane contents* below has no effect
under the others. *Resume programs* needs a layout to put programs back into, so it is off
entirely under "Open a tab with the default profile".

## Resume programs

Restoring a layout gives you the panes back; resuming programs puts something in them. Each
category is separate, because they fail differently.

**Resume coding agents** — a pane that was running Claude Code, Codex, Copilot or another
recognised agent reopens *the same conversation*, keeping the flags it was started with.
This is not "run the command again": the agent is asked to resume, so the history is intact.

**Resume session multiplexers** — a pane running shefrd, herdr, tmux, screen or zellij
reattaches to the session its server is still holding. The server outlived the Terminal, so
nothing was lost in the first place; this just reconnects to it.

**Also resume these programs** — comma-separated program names. Each is re-run exactly as it
was, with its original arguments. Use this for anything that is neither a known agent nor a
multiplexer: a watcher, a REPL, a log tail.

**Never resume these programs** — comma-separated, and it wins over everything above. This
is how you turn a whole category on and still leave one program out.

> [!WARNING]
> "Also resume these programs" re-runs a command line. That is safe for a watcher and
> unwise for anything that changes state when it starts — a migration, a deploy script, a
> one-shot command with side effects. The agent and multiplexer categories are different in
> kind: they reattach or resume rather than re-execute.

**When resuming sessions** chooses how much ceremony this gets: resume silently, resume
immediately and then say so, or ask first. It only applies when something is actually
eligible to be resumed, so with nothing to resume you are never prompted.

## Saving pane contents

Restoring what was on screen means writing each pane's scrollback to disk.

Without **Save pane contents periodically**, scrollback is only written when the Terminal
closes normally — so a crash, a power cut, or End Task loses it. With it on, the Terminal
writes every *N* seconds; shorter loses less in a crash, longer writes less to disk.

**What saving has been costing** shows the last seven days of saves, oldest on the left, so
the trade-off is a number rather than a guess. If that looks expensive, lengthen the
interval before turning the feature off.

> [!NOTE]
> Saved scrollback is a file on disk containing whatever was on your screen. If a command
> printed a token or a password, it is in that file. Turn *Restore window layout and content*
> down to *Restore window layout* if that is not a trade you want to make.

## See also

- [Activity log](activity-log.md) — the other thing the Terminal records about what ran.
- [Provenance marks](fork-marks.md) — most of this page is specific to this Terminal.
