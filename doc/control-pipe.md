# The control pipe

A local, addressable, byte-exact control channel into a running Windows Terminal. It exists so that
another process on the same machine can do the things that every other major terminal already
allows, and that Windows Terminal had no way to do at all:

- **enumerate** the panes of every window in the process,
- **read** what is on a pane's screen,
- **write** text into a specific pane's connection,
- **focus** a specific pane: select its tab, focus it, and bring its window forward,
- **run an action** in a specific pane, any action `settings.json` can bind to a key, without
  pressing that key.

This is a fork-only feature. Upstream has refused `wt send-input` twice (microsoft/terminal#9368,
PR #20106) over the injection risk, and the automation surface they *are* building
(PR #20461, `wtcli` over COM) deliberately leaves input out. See [Relationship to
upstream](#relationship-to-upstream).

## Why not the alternatives

The thing this replaces is UI Automation plus `System.Windows.Forms.SendKeys`, and it replaces it
because that approach is broken in two ways that cannot be fixed from outside the process:

- **It corrupts text.** Synthesised keystrokes go through the message queue, interleave with the
  user's own typing and with the shell's own reads. A real capture: `/rename my-session` arrived as
  `//rreenn/ramemnea mne`. `SendKeys` additionally assigns meaning to `+ ^ % ~ ( ) { } [ ]`, so
  anything containing them needs escaping that the sender has to get exactly right.
- **It can only reach the foreground window, and only a whole window.** There is no way to say
  *which pane*. With any split on screen the write lands somewhere unpredictable.

The pipe fixes both by construction: text goes to the connection, not the keyboard, and every
operation names a pane.

## Turning it on and off

The `controlPipe` global setting, **default `true` in this fork**:

```jsonc
{
    "controlPipe": false   // no pipe is created; an existing one is torn down
}
```

It is re-read on every settings reload, so flipping it takes effect without restarting the Terminal.
With it off, the pipe does not exist and a client sees nothing to connect to.

## Connecting

```
\\.\pipe\wt-control-<pid>
```

One pipe per Terminal process, `<pid>` being that process's id. A client enumerates
`\\.\pipe\wt-control-*` and talks to each one it finds, so several Terminals — stable, preview, a
dev build, an elevated one — coexist with no coordination between them.

The pipe is created with an explicit DACL granting **the current user's SID and nothing else**
(`D:P(A;;GA;;;<sid>)`), and with `PIPE_REJECT_REMOTE_CLIENTS`. The first instance is created with
`FILE_FLAG_FIRST_PIPE_INSTANCE`, so a name someone else already owns fails loudly rather than
quietly making us a second instance of somebody else's pipe. Up to 4 clients are served at once.

## Wire format

UTF-8, newline-delimited JSON. One request object per line, one response object per line, in order.
The server keeps reading until the client disconnects, so a client may send many requests on one
connection or open one per request. A line over 1 MiB is rejected.

Every response carries `"ok"`. On failure:

```json
{"ok":false,"error":"<code>"}
```

| Code | Meaning |
|---|---|
| `bad-request` | Not JSON, not an object, unknown `op`, a malformed pane id, a field of the wrong type, or an over-long line. |
| `no-such-pane` | The window/tab/pane triple (or, for `focus-pane` and `run-action`, the session id) doesn't name a live terminal pane in this process. |
| `needle-gone` | `requireContains` was not on the pane's screen. **Nothing was written**, and for `run-action` nothing was done. |
| `disconnected` | The pane's connection has already closed. **Nothing was written.** |
| `missing-reason` | The request had no `reason` or no `caller` (see below). **Nothing was done**; the response also carries a `message` saying what to add. |
| `bad-action` | `run-action` only: the `action` is a name or an object, but the settings model does not read it as an action (an unknown name, `"unbound"`, arguments its parser refuses). **Nothing was done**, not even the focus move. |
| `action-not-allowed` | `run-action` only: an action the pipe refuses to run - see [the list](#refused-actions). **Nothing was done**, not even the focus move. |
| `action-failed` | `run-action` only: the action threw while it ran. Unlike the codes above, **it may have done part of its work**; the pane was already focused. |

Unknown members in a request are ignored, so adding a field later doesn't break an existing client.

### Every request says why, and who is asking

Every request - `ping` included - must carry two strings, or it is refused with `missing-reason`
before anything is done for it (protocol version 2):

| Field | What to put in it |
|---|---|
| `reason` | Why this request is being sent, in words a person reading the Activity log would understand: `"find which terminal hosts session 6f1a... to label it on the dashboard"`, not `"lookup"`. |
| `caller` | The program and code path sending it, with its pid, specific enough to find the code: `"stith-server host-terminal.ts identifyTerminalProduct (pid 1939269, WSL Ubuntu)"`. |

This is required rather than inferred because the server can only see the process on its end of
the pipe, and for anything in WSL that is a throwaway `powershell.exe` whose parent is
`wslhost.exe` - the same two names for every WSL client on the machine. The Windows side records
the whole parent chain it can see (`ancestry`, under [Recording](#recording)); past `wslhost.exe`,
only the client can say who it is. Both are capped at 300 characters. The examples below leave them
out for brevity; a real request has them:

```json
{"op":"list-panes","reason":"find the pane showing session 6f1a... to rename it","caller":"stith-server live-rename.ts (pid 4242)"}
```

### ping

```json
{"op":"ping"}
{"ok":true,"version":2,"pid":24680,"windows":[1,2]}
```

### list-panes

```json
{"op":"list-panes"}
{"op":"list-panes","containing":"6f1a2b3c-...."}
```

```json
{"ok":true,"panes":[
  {"id":"1.0.3","window":1,"tab":0,"pane":3,"title":"stith - bash",
   "focused":true,"windowFocused":false,"alive":true,"pid":12345,"process":"wsl.exe",
   "session":"{6f1a2b3c-....}"}
]}
```

- `id` is `"<window>.<tabIndex>.<paneId>"` and is what the other ops take. `window` is the
  Terminal window id (the one `wt -w` uses), `tab` is a positional index, `pane` is the pane's id
  within its tab.
- `containing` filters to panes whose current screen contains that literal string. The match happens
  inside the app against the pane's own buffer, and **no pane text is returned**, because this is
  the call a polling client makes every few seconds.
- `focused` — this pane has focus within its tab *and* its tab is the active tab.
  `windowFocused` — that window is the OS foreground window. Neither is a precondition for anything;
  they are reporting only.
- `alive` — the pane's process is still running. A pane whose shell has exited stays listed, because
  its last screen is usually the thing you wanted; it just reports `false`, and `pid` drops to 0 with
  an empty `process`. It is the same test `send-input` applies, so a client that filters on this and
  one that writes blindly and reads back `disconnected` can never disagree about a pane.
- `session` is the connection's session id, the same GUID the shell sees in `WT_SESSION`. It is an
  addition to the original contract, and a client is free to ignore it. `focus-pane` accepts it as
  the pane's address.

### capture-pane

```json
{"op":"capture-pane","pane":"1.0.3","lines":80}
{"ok":true,"text":"line one\nline two\n..."}
```

Returns the last `lines` rows ending at **the last row that has anything on it**; omit `lines` for
the whole viewport. Only a `lines` larger than the screen reaches up into the scrollback to make up
the count. Rows come back the way they are laid out on screen — one output line per buffer row,
**wrapping included** — with trailing whitespace trimmed per row and rows joined with `\n`. No escape
sequences. A pane that genuinely shows nothing returns `""`.

"Viewport" here means the live screen, the way `tmux capture-pane` does: it deliberately does *not*
follow where the user has scrolled to, because otherwise scrolling the pane with the mouse would
silently change what a client reads.

**Anchoring on the last written row rather than on the bottom of the screen is load bearing**, and
getting it wrong is subtle enough to be worth recording. A shell that has printed four lines into a
thirty-row pane leaves twenty-six blank rows underneath. Counting `lines` rows *up from the bottom of
the viewport* lands entirely inside that blank region, and the per-row trimming then reduces the
whole capture to `""`. The result is a read path that looks correct whenever a pane is full — a
full-screen TUI captures perfectly — and returns nothing for an ordinary shell prompt. Since an empty
capture reads as "cannot tell", a client deciding whether it is safe to type would simply never type,
silently, and indistinguishably from "no pane matched".

### send-input

```json
{"op":"send-input","pane":"1.0.3","text":"/rename my session","requireContains":"6f1a2b3c-...."}
{"ok":true}
```

Writes `text` into that pane's connection, **verbatim**: exactly those characters, nothing added,
nothing escaped, no trailing newline, no bracketed-paste wrapper. This is the same
`TermControl::SendInput` the `sendInput` action uses.

`requireContains` is optional and should not be treated as such. It re-checks that the pane's screen
still contains that literal string, atomically with the write, on the same trip to the UI thread. If
it doesn't, **nothing is written** and the answer is `needle-gone`. That is the guard that makes a
wrong-pane injection impossible even if tabs are reordered or a pane is closed between the client's
`list-panes` and its `send-input`, and the failure it prevents — a command typed into somebody else's
shell — is the worst thing this feature could do.

To submit a command, send **two** calls: the text, then `"\r"` on its own. They are never combined,
buffered or coalesced. A single write containing both is read by some TUIs (Claude Code among them)
as a bulk paste, and never submits.

### focus-pane

```json
{"op":"focus-pane","session":"{6f1a2b3c-....}"}
{"op":"focus-pane","pane":"1.0.3"}
{"ok":true,"id":"1.0.3"}
```

Selects the pane's tab, focuses the pane within it, and summons its window to the front — restoring
it if minimised, on the monitor it is already on, the same summon a toast click performs. Name the
pane **exactly one** way: `session` (the `WT_SESSION` GUID, braced or bare, any case) or `pane`.
Both, or neither, is `bad-request`. A tab zoomed onto a different pane is unzoomed first; a tab zoomed
onto the target keeps its zoom.

The success response carries `id`, the address the pane resolved to, so a client that asked by
session learns it without a second `list-panes`.

`no-such-pane` means **this process** has no pane with that session. Every Terminal process has its
own pipe, so a client looking for a session asks each `wt-control-*` pipe in turn and stops at the
first `ok`.

### run-action

```json
{"op":"run-action","pane":"1.0.3","requireContains":"PS C:\\demo>","action":"clearBuffer"}
{"op":"run-action","pane":"1.0.3","requireContains":"PS C:\\demo>","action":{"action":"scrollUp","rowsToScroll":10}}
{"op":"run-action","session":"{6f1a2b3c-....}","requireContains":"$ ","action":{"action":"commandPalette","launchMode":"commandLine"}}
{"op":"run-action","pane":"1.0.3","requireContains":"$ ","action":{"action":"openSettings","target":"settingsUITab"}}
{"ok":true,"id":"1.0.3","handled":true}
```

Runs one action in that pane as if its key binding had been pressed. `action` is exactly what
`settings.json` puts under `"command"`: the name on its own, or an object with `"action"` and its
arguments. It is read by the settings model's own action parser (`ActionAndArgs::Deserialize`), so
the vocabulary is the settings file's, every action and every argument, and anything the settings
file would not accept is refused the same way. A name or an object that does not come out as an
action is `bad-action`; an `action` that is not a string or an object at all is `bad-request`.

Name the pane **exactly one** way, as for `focus-pane`: `pane` or `session`. The response's `id` is
the address it resolved to.

**`requireContains` is required here**, and must be non-empty; a request without it is
`bad-request`. It is the same guard as `send-input`'s, for the same reason, only more so: a
`window.tab.pane` address goes stale the moment a tab closes or moves, and `closePane`,
`closeTab`, `clearBuffer`, `restartConnection` or a `sendInput` action aimed at the wrong pane is
somebody else's shell closed, wiped or typed into. It is checked against the pane's screen first,
on the same trip to the UI thread as everything else; if the text is not there the answer is
`needle-gone` and **nothing is done**, not even the focus move. Unlike `send-input`, a pane whose
process has exited is still a valid target — `restartConnection` exists for exactly that pane.

A key binding acts on the focused tab and its active pane, so the target is made that next —
inside the window only. The pane's tab is selected and the pane made active within it (a tab zoomed
onto another pane is unzoomed), each step skipped when it is already so. **The window is not
raised, restored or activated**; there is no summon, unlike `focus-pane`. Then the action goes
through the window's normal dispatch, the same one a key binding uses.

`handled` is the dispatcher's answer: `false` is an action that parsed but that nothing took here
(`copy` with no selection, say), which is not an error. `globalSummon` and `quakeMode` always
answer `handled:false`: they are hotkeys the window manager registers, and their in-window handlers
decline on purpose, so over the pipe they do nothing. An action that opens UI — the command palette,
a dialog, the settings tab — returns as soon as the UI is up; it does not wait for it to close. An
action that throws while it runs is `action-failed`, never `handled:false`.

#### Refused actions

These are answered `action-not-allowed` with nothing done. The list lives in one place,
`IsRefusedOverPipe` in `TerminalPage.ControlPipe.cpp`, with the reason for each entry beside it.

| Action | Why |
|---|---|
| `openSystemMenu` | `TrackPopupMenu` runs a nested modal message loop inside the pipe's `SendMessage`, so the pipe would re-enter itself under the menu. |
| `newWindow`, `openWorkspace` | Open a new window. |
| `moveTab`, `movePane` with a `window` | Move to another or a new window, which opens or raises it. Moving within the window is allowed. |
| `openSettings` with a file target (`settingsFile`, `defaultsFile`, `allFiles`, `directory`) | Opens an external editor or Explorer. `settingsUITab` is always allowed; `settingsUI` is allowed unless `settingsUIHost` is `"window"`. |
| `searchWeb`, `openCWD` | Hand a URL or folder to the shell, which opens a browser or Explorer in front of the user. |
| `exportBuffer` without a `path` | Shows a file picker, a modal dialog. With a `path` it writes the file and returns. |
| `newTab` or `splitPane` that would elevate | An `elevate` argument, or a profile with `elevate` set: a UAC prompt and a new elevated window. A duplicating `splitPane` copies the focused pane and is allowed. |
| `saveSnippet` | Writes `settings.json`. The pipe does not change settings. |
| `breakIntoDebugger` | `DebugBreak` with no debugger attached takes the whole process down. |

`run-action` is the in-window counterpart of upstream's microsoft/terminal#9994 ("Allow for
ExecuteCommandline actions to be called straight from the commandline", tracked in #4632), which
asks for the same reach from `wt.exe`. Nothing upstream runs an arbitrary action against a running
window.

## Guarantees

- **No key synthesis.** Nothing goes through `SendInput`, `keybd_event`, `SendKeys` or the message
  queue. Text goes to the connection.
- **No focus involvement, except in `focus-pane` and `run-action`.** `list-panes`, `capture-pane`
  and `send-input` never call `SetForegroundWindow`, activate a window, switch a tab or call
  `Focus()`. They work with the target window minimised, on another virtual desktop, and with the
  pane in a background tab, and the user's focus is exactly where it was before the call.
  `focus-pane` is the one op whose whole job is to move focus, and it moves it only when asked.
  `run-action` moves it only inside the target window — tab and pane — and never summons or
  activates that window; what the action itself does with focus is the action's business.
- **No UI-thread blocking.** Accept, read, parse, serialise and process-name lookups all happen on
  the pipe threads. Only the pane touch is marshalled to the UI thread, and a stuck or dead client
  blocks nothing but its own thread.
- **Byte-exact.** UTF-8 in, the same characters out (UTF-16 inside the app). Emoji and non-ASCII
  survive.
- **Clean lifetime.** The pipe comes up with the process and goes away with it. Turning the setting
  off cancels the in-flight I/O and joins the threads; a client dying mid-request is ordinary.

## Security

The pipe can type into the user's shells, so:

- The DACL is the whole story: current user SID only, no `PIPE_ACCESS_*` defaults, not Everyone, not
  Authenticated Users, no remote clients.
- The op set is exactly the six above. Five of them have no file access, no settings mutation, no
  way to spawn a pane or a process and no way to close one.
- `run-action` is the exception, and knowingly so: it can do nearly anything a key binding can,
  which includes opening tabs and panes (and so starting processes) and closing them. It is a
  deny-list, not an allow-list, because the point is that the vocabulary is the settings file's;
  what it refuses is [listed above](#refused-actions): nothing that opens or raises a window, runs
  a modal loop, elevates, crashes the process, or writes `settings.json`. **The pipe still cannot
  change settings**: `saveSnippet`, the one action that writes the settings file, is refused, and
  opening the file in an editor is refused with it. `exportBuffer` with a `path` does write that
  file, as it would from a key. Every `run-action` must carry `requireContains`, so a stale address
  fails as `needle-gone` instead of acting on someone else's pane. It widens nothing an attacker on the pipe did not already have: `send-input`
  can type any command into a live shell, which is strictly more than any action can do. What it
  adds is the app's own commands, recorded like every other request.
- `focus-pane` widens nothing an attacker could use. Focus is not input: it types nothing, and a
  caller who can reach the pipe can already write into any pane directly with `send-input`, which is
  strictly more powerful. What it can do is pull a window to the front, which is the same thing a
  notification toast's click already does, and it is behind the same current-user DACL.
- Connections are traced at measure level; **the text being written never goes into a trace**,
  because session titles and command lines would end up in telemetry. It *is* recorded in the
  activity log - see [Recording](#recording) - which is a file in this user's own package folder,
  not a trace, and exists precisely so you can see what was typed into your shells and by whom.
- `controlPipe: false` removes the endpoint entirely.

## Recording

Every request is recorded, including the ones that fail and lines that never parse - a client
writing garbage into the pipe is exactly the one worth knowing about. Two places show it:

**The activity log** (`activity.jsonl` beside `settings.json`; Settings → Activity) gets one
`controlPipe` event per request:

| Field | For a pipe request |
|---|---|
| `exe`, `pid` | The **client**: the process on the other end of the pipe (`GetNamedPipeClientProcessId`), asked once per connection. |
| `parentExe`, `parentPid` | Its parent. Clients are often a short-lived `powershell.exe`; the parent is what started it. |
| `commandLine` | What was asked, on one line: `send-input 1.0.3 "echo hi\r"`. Control characters are spelled out, so whether the Enter was sent is visible. A `run-action` shows its action as the JSON it was sent as, then its guard: `run-action 1.0.3 {"action":"scrollUp","rowsToScroll":10} requireContains "$ "`, or `run-action 1.0.3 "clearBuffer" requireContains "$ "`. |
| `ancestry` | The parent and every ancestor Windows still knows, nearest first: `wslhost.exe 20440 < wsl.exe 1234`. The walk stops at a pid whose process is younger than its child (a reused pid) rather than name a stranger. |
| `reason`, `caller` | What the client said: why it asked, and who it is. Empty on a request refused for lacking them. |
| `outcome` | How it ended: `ok`, or the error code it was answered, plus what came back (`3 panes`, `812 chars`, the pane focus-pane landed on, and for run-action the pane it ran in and `handled` or `not handled`: `ok, 1.0.3 handled`). Records written before version 2 kept this in `reason`; the Activity page reads them that way. |
| `op`, `target`, `result` | The same, unpacked, for a script that would rather not parse `commandLine`. |

send-input text and a run-action's JSON are kept in full up to 4096 characters, then cut with a count of what was dropped;
filters and needles up to 256; an unparsed line's first 512 bytes. The log is governed by the
`activityLog` and `activityLogMaxKilobytes` settings like every other event - turning the activity
log off stops recording pipe requests too.

**The tab row** grows a small badge the first time a window sees a pipe request, and keeps it for
the rest of that window's life. The number beside it counts requests since its list was last
opened; the list holds the latest ten rows, newest first: the op, the pane and the result, the
client's reason, and its caller and process (the process chain is the row's tooltip). Repeats -
the same op, target and request from the same program, caller and reason, answered the same way -
are grouped into one row with a count and the time of the first, so a client polling every few
seconds is one line; the *Group identical* box above the list turns that off for the window. Its
last row - *Show all in Activity* - opens Settings → Activity filtered to control
pipe requests. Every window gets every request: the badge says the pipe is in use at all, not which
window was touched. A window opened later starts with no badge until the next request.

The pipe thread does none of the UI work: it records into the activity log (which formats on the
caller and appends on a threadpool timer) and **posts** `WM_CONTROL_PIPE_ACTIVITY` to the emperor,
so the client already has its answer before any of this happens.

## How it fits together

| Piece | Where |
|---|---|
| Wire format: parsing, formatting, pane ids | `src/cascadia/WindowsTerminal/ControlPipeProtocol.h` |
| The pipe itself: threads, DACL, framing, I/O | `src/cascadia/WindowsTerminal/ControlPipeServer.cpp` |
| Marshalling onto the UI thread (`WM_CONTROL_PIPE_REQUEST`), window enumeration | `src/cascadia/WindowsTerminal/WindowEmperor.cpp` |
| Pane enumeration, capture, input, focus and actions, per window | `src/cascadia/TerminalApp/TerminalPage.ControlPipe.cpp` |
| Reading an action (`run-action`) | `ActionAndArgs::Deserialize`, the settings model's own parser |
| Viewport read and match | `ControlCore::ReadViewportText`, `ControlCore::ViewportContains` |
| Tests for the wire format | `src/cascadia/UnitTests_Control/ControlPipeProtocolTests.cpp` |
| Describing a request for the record (`DescribeRequest`, `SummarizeCall`) | `src/cascadia/WindowsTerminal/ControlPipeProtocol.h` |
| Identifying the client, reporting each request | `ControlPipeServer::_identifyClient`, `_report` |
| Writing the `controlPipe` event, broadcasting to windows | `WindowEmperor::_setupControlPipe`'s observer, `_broadcastControlPipeCall` |
| The badge and its list | `src/cascadia/TerminalApp/TabRowControl.{xaml,cpp}` |
| The Activity page's filter and "show all" landing | `TerminalSettingsEditor/Activity.cpp` (`ShowOnlyKind`), `MainPage::NavigateToActivity` |

`WindowEmperor` hosts the server because it owns every `AppHost` in the process, which is what pane
enumeration across windows needs. The pipe threads never touch a pane directly; they `SendMessage`
to the emperor's message window, which runs the whole operation on the UI thread and hands back a
plain struct.

## Trying it

With the Terminal window **minimised**, from PowerShell:

```powershell
$name = [System.IO.Directory]::GetFiles('\\.\pipe\') |
        Where-Object { $_ -like '*wt-control-*' } | Select-Object -First 1
$name = Split-Path $name -Leaf
$c = New-Object System.IO.Pipes.NamedPipeClientStream '.', $name, 'InOut'
$c.Connect(2000)
$w = New-Object System.IO.StreamWriter $c; $w.AutoFlush = $true
$r = New-Object System.IO.StreamReader $c
$who = '"reason":"trying the pipe by hand","caller":"interactive PowerShell (pid ' + $PID + ')"'
$w.WriteLine('{"op":"ping",' + $who + '}');       $r.ReadLine()
$w.WriteLine('{"op":"list-panes",' + $who + '}'); $r.ReadLine()
$w.WriteLine('{"op":"capture-pane","pane":"1.0.0","lines":20,' + $who + '}'); $r.ReadLine()
$w.WriteLine('{"op":"send-input","pane":"1.0.0","text":"echo hello-from-the-pipe",' + $who + '}'); $r.ReadLine()
$w.WriteLine('{"op":"send-input","pane":"1.0.0","text":"`r",' + $who + '}'); $r.ReadLine()
$w.WriteLine('{"op":"run-action","pane":"1.0.0","action":"clearBuffer","requireContains":"hello-from-the-pipe",' + $who + '}'); $r.ReadLine()
```

The command runs in that pane, the buffer is then cleared, and the window stays minimised and
unfocused throughout.

## Relationship to upstream

| | This pipe | upstream PR #20461 (`wtcli`) |
|---|---|---|
| Transport | named pipe, DACL'd to the user | classic COM, per-brand CLSID, `OpenConsoleProxy.dll` |
| Reachable from WSL / any language | yes, it's a pipe | needs a COM client |
| Pane addressing | `window.tab.pane` | connection `SessionId` GUID |
| Read | viewport-bounded | `ReadEntireBuffer()`, then split |
| Write into a pane | yes, with an atomic guard | **no, by design** |
| Mutating ops (new-tab, split, kill, focus) | focus, and any bindable action through `run-action` | yes |

The overlap is real but the shapes don't compose: upstream's surface omits the one operation this
exists for, and its transport can't be reached from where the client lives. If `wtcli` lands, the
`session` field above is the bridge — it is the same id upstream addresses panes by.
