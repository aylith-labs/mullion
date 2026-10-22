# Activity log

The activity log answers one question: **where did that come from?** A window you did not
open, a console that flashed and vanished, a process you found in Task Manager with the
Terminal as its parent — the log records every program this Terminal started and every
console program that was handed to it.

The **Activity** page is where you read it. There is also an activity pane, reachable from
the command palette, for the window that just surprised you and is still on screen; the page
is where you go looking when you remember the question later, because it is where the switch
that turns recording on already lives.

## What is recorded

For each event: the time, the program, its **full command line**, its working directory, and
what started it.

> [!WARNING]
> Command lines are recorded in full. An argument containing a password, a token or an API
> key is written to that file in plain text. The log lives beside your settings, readable by
> your Windows account. If you routinely pass secrets on a command line, leave this off.

## Reading the page

The filter box narrows the list by any of the recorded text — a program name, a folder, a
fragment of a command line. The refresh button re-reads the file, because the page takes a
snapshot when you open it rather than following the log live.

The page shows the most recent couple of hundred entries, which is more than fits on a
screen and enough for "what just happened". For a full trawl through a large log, open the
file itself.

## The file

`activity.jsonl`, beside your `settings.json`. One JSON object per line, which is what makes
it greppable and trivially machine-readable:

```
%LOCALAPPDATA%\Packages\<package family>\LocalState\activity.jsonl
```

The page prints the exact path under *Log file*. Each installed version of the Terminal
keeps its own, because each has its own settings folder.

**Maximum log size** bounds it: when `activity.jsonl` passes the limit it is renamed to
`activity.1.jsonl` and a new one is started. One previous file is kept, so the most you ever
hold is roughly twice the limit.

## Why it defaults to on

A terminal that launches things on your behalf — a default-terminal handoff, a shell
integration, a script someone sent you — is exactly where an unexplained process comes from,
and nothing else in Windows tells you which terminal started what. The log is cheap: a line
per launch, capped, and never sent anywhere.

Turning it off stops recording immediately and leaves the existing file alone. An empty page
with recording off says so, rather than looking like "nothing has happened".

## See also

- [Session restore](session-restore.md) — the programs the Terminal starts *for* you when it
  comes back.
