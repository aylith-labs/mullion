# The control pipe

A local, addressable, byte-exact control channel into a running Windows Terminal. It exists so that
another process on the same machine can do the things that every other major terminal already
allows, and that Windows Terminal had no way to do at all:

- **enumerate** the panes of every window in the process,
- **read** what is on a pane's screen,
- **write** text into a specific pane's connection,
- **focus** a specific pane: select its tab, focus it, and bring its window forward.

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
| `no-such-pane` | The window/tab/pane triple (or, for `focus-pane`, the session id) doesn't name a live terminal pane in this process. |
| `needle-gone` | `requireContains` was not on the pane's screen. **Nothing was written.** |
| `disconnected` | The pane's connection has already closed. **Nothing was written.** |

Unknown members in a request are ignored, so adding a field later doesn't break an existing client.

### ping

```json
{"op":"ping"}
{"ok":true,"version":1,"pid":24680,"windows":[1,2]}
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

## Guarantees

- **No key synthesis.** Nothing goes through `SendInput`, `keybd_event`, `SendKeys` or the message
  queue. Text goes to the connection.
- **No focus involvement, except in `focus-pane`.** `list-panes`, `capture-pane` and `send-input`
  never call `SetForegroundWindow`, activate a window, switch a tab or call `Focus()`. They work with
  the target window minimised, on another virtual desktop, and with the pane in a background tab,
  and the user's focus is exactly where it was before the call. `focus-pane` is the one op whose
  whole job is to move focus, and it moves it only when asked.
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
- The op set is exactly the five above. There is no "run this", no file access, no settings
  mutation, no way to spawn a pane or a process, no way to close one.
- `focus-pane` widens nothing an attacker could use. Focus is not input: it types nothing, and a
  caller who can reach the pipe can already write into any pane directly with `send-input`, which is
  strictly more powerful. What it can do is pull a window to the front, which is the same thing a
  notification toast's click already does, and it is behind the same current-user DACL.
- Connections are traced at measure level; **the text being written is never logged**, because
  session titles and command lines would end up in traces.
- `controlPipe: false` removes the endpoint entirely.

## How it fits together

| Piece | Where |
|---|---|
| Wire format: parsing, formatting, pane ids | `src/cascadia/WindowsTerminal/ControlPipeProtocol.h` |
| The pipe itself: threads, DACL, framing, I/O | `src/cascadia/WindowsTerminal/ControlPipeServer.cpp` |
| Marshalling onto the UI thread (`WM_CONTROL_PIPE_REQUEST`), window enumeration | `src/cascadia/WindowsTerminal/WindowEmperor.cpp` |
| Pane enumeration, capture, input and focus, per window | `src/cascadia/TerminalApp/TerminalPage.ControlPipe.cpp` |
| Viewport read and match | `ControlCore::ReadViewportText`, `ControlCore::ViewportContains` |
| Tests for the wire format | `src/cascadia/UnitTests_Control/ControlPipeProtocolTests.cpp` |

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
$w.WriteLine('{"op":"ping"}');       $r.ReadLine()
$w.WriteLine('{"op":"list-panes"}'); $r.ReadLine()
$w.WriteLine('{"op":"capture-pane","pane":"1.0.0","lines":20}'); $r.ReadLine()
$w.WriteLine('{"op":"send-input","pane":"1.0.0","text":"echo hello-from-the-pipe"}'); $r.ReadLine()
$w.WriteLine('{"op":"send-input","pane":"1.0.0","text":"`r"}'); $r.ReadLine()
```

The command runs in that pane, and the window stays minimised and unfocused throughout.

## Relationship to upstream

| | This pipe | upstream PR #20461 (`wtcli`) |
|---|---|---|
| Transport | named pipe, DACL'd to the user | classic COM, per-brand CLSID, `OpenConsoleProxy.dll` |
| Reachable from WSL / any language | yes, it's a pipe | needs a COM client |
| Pane addressing | `window.tab.pane` | connection `SessionId` GUID |
| Read | viewport-bounded | `ReadEntireBuffer()`, then split |
| Write into a pane | yes, with an atomic guard | **no, by design** |
| Mutating ops (new-tab, split, kill, focus) | focus only | yes |

The overlap is real but the shapes don't compose: upstream's surface omits the one operation this
exists for, and its transport can't be reached from where the client lives. If `wtcli` lands, the
`session` field above is the bridge — it is the same id upstream addresses panes by.
