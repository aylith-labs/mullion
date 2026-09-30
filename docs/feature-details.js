// Long-form documentation, keyed by the ids in features.js.
//
// Kept separate from the catalogue on purpose: features.js is generated from
// git and stays machine-checkable, while everything here is written by hand and
// changes for editorial reasons rather than because a commit landed.
//
// Shape:
//   problem   why the feature exists — what goes wrong without it
//   how       what it actually does, in enough detail to be checkable
//   steps     a walkthrough someone can follow in their own build
//   settings  the settings.json keys involved, with real defaults
//   media     { base, caption } — resolves to media/<base>-light.png and
//             media/<base>-dark.png, or -.mp4 for kind:"video"
//   upstream  what Microsoft did about it, where that is known
window.FEATURE_DETAILS = {

  "session-resume": {
    problem:
      "Close the terminal and the work is gone. Upstream can put the windows, tabs and splits back, and can even repaint what was on screen, but every pane comes back as a fresh shell: the agent you had a two-hour conversation with, the multiplexer holding six sessions, the dev server — all of them are simply not running any more. Restoring the furniture is not the same as restoring the work.",
    how:
      "Shortly before the layout is written out, every pane is asked what it is running right now. Windows panes are read from the process tree, taking the first thing the shell launched — not the deepest, because an agent spawns a child per MCP server and the deepest descendant is one of those. WSL panes cannot be read that way at all, since none of their processes are Windows processes; instead one probe runs inside each distro and matches panes by the WT_SESSION identifier the terminal already puts into every pane's environment. That probe never runs twice at once, runs at most every 30 seconds, backs off up to five minutes while a distro is not answering, and has its whole process tree (console host included) killed after ten seconds, so a wedged WSL cannot pile up hung processes; panes it could not ask about keep their last answer. The answer is stored in the same persisted layout the panes are restored from, so it travels with the profile, the working directory and the split geometry. On the next start it is typed into the pane rather than launched as its command line — launching it would make the agent the pane's root process, so the pane would close the moment it exited, instead of leaving you the shell it was actually running in.",
    steps: [
      "Turn on <code>Restore window layout and content</code> under Settings → Session Restore. Resume rides on the persisted layout, so without it there is nothing to attach a command to.",
      "Leave <code>Resume coding agents</code> and <code>Resume session multiplexers</code> on. Anything else you want back — a dev server, a watcher — goes in <code>Also resume these programs</code> by name.",
      "Open a pane and start something: a coding agent, or <code>tmux new-session -s demo</code>.",
      "Close the terminal, or kill it outright. Contents are saved on a timer, so an ungraceful end costs you the process but not the history.",
      "Reopen. The pane returns on its own profile, in its own directory, running what it was running — and a notice names what came back.",
    ],
    settings: [
      { key: "resumeAgents", def: "true", note: "Reopen a known agent's conversation, keeping its original flags." },
      { key: "resumeMultiplexers", def: "true", note: "Reattach to shefrd, herdr, tmux, screen or zellij." },
      { key: "resumeExtraPrograms", def: "[]", note: "Extra program names to re-run exactly as they were found." },
      { key: "resumeExcludedPrograms", def: "[]", note: "Never resume these. Beats every setting above." },
      { key: "resumeSessionNotification", def: "\"toast\"", note: "silent, toast, or confirm — which lists what it would resume and waits." },
      { key: "persistBufferPeriodically", def: "true", note: "Save pane contents on a timer so a crash doesn't take the scrollback." },
      { key: "bufferPersistIntervalMinutes", def: "5", note: "How often that happens." },
    ],
    media: [
      { kind: "video", base: "resume-restore", caption: "A tmux session running, the terminal killed outright, and the pane coming back attached to it. Recorded in one take across the restart." },
      { base: "resume-toast", caption: "After the restart: the notice names what was resumed, and it is the attach form — not the command the session was first created with." },
      { base: "resume-settings", caption: "The settings, under Session Restore. Agents and multiplexers are separate switches, with lists for anything else." },
      { base: "resume-restored-buffer", caption: "A pane that resumed nothing keeps its scrollback instead, marked with when it was saved." },
    ],
    notes: [
      "A pane that reopens an agent conversation deliberately does <em>not</em> repaint its saved scrollback — the agent redraws its own history, and doing both would show the same transcript twice.",
      "Multiplexer-owned processes are skipped on purpose. shefrd, tmux, screen and zellij mark the processes they run, and those belong to the daemon, which restores them itself; resuming them here too would start a second copy of everything inside.",
      "Only agents whose resume syntax is actually known are in the table. Guessing a flag produces a command that fails at restore, which is worse than getting a shell — anything unlisted can still be named in <code>resumeExtraPrograms</code>.",
    ],
    upstream:
      "Restoring buffer <em>contents</em> is upstream's (microsoft/terminal#961, implemented in #16598). Restoring the running process is not: they closed that thread with \"restoring the actual state of the running executable might be impossible\" and deliberately stopped at the text. Saving contents periodically rather than only on a clean exit is an open upstream PR (#19805) that has sat since January.",
  },

  "control-pipe": {
    problem:
      "Automating a terminal from outside usually means faking keystrokes at the window that happens to have focus. That breaks the moment focus moves, races anything else typing, and cannot read back what a pane actually printed. Upstream's own automation surface additionally cannot see into a pane running inside WSL.",
    how:
      "Each window serves a named pipe, <code>\\\\.\\pipe\\wt-control-&lt;pid&gt;</code>, speaking newline-delimited JSON. Four operations: <code>ping</code>, <code>list-panes</code>, <code>capture-pane</code> and <code>send-input</code>. The pipe is created with a DACL granting the current user's SID and nothing else, and rejects remote clients. It cannot run commands or change settings — it can only address panes that already exist.",
    steps: [
      "Leave <code>controlPipe</code> on (it is by default), or set it to false to close the pipe entirely.",
      "Find the pipe: it is named after the Terminal process id, so a client can enumerate <code>\\\\.\\pipe\\wt-control-*</code> and talk to every Terminal running.",
      "Send <code>{\"op\":\"list-panes\"}</code> to see every window, tab and pane, with the process each is running.",
      "Send <code>{\"op\":\"capture-pane\",\"pane\":\"1.0.0\",\"lines\":20}</code> to read back what that pane printed.",
      "Send <code>{\"op\":\"send-input\",\"pane\":\"1.0.0\",\"text\":\"...\"}</code>, then <code>\"\\r\"</code> as a second call, to type into it.",
    ],
    settings: [
      { key: "controlPipe", def: "true", note: "Serve the pipe. Set false to turn it off completely." },
    ],
    media: [
      { base: "control-pipe-list", caption: "list-panes: every window, tab and pane, and what each one is running." },
    ],
    notes: [
      "The Enter goes as its own call, never combined with the text. A single write containing both is read by some TUIs as a bulk paste and never submits.",
      "<code>send-input</code> takes an optional <code>requireContains</code>, re-checked atomically with the write. If the pane's screen no longer contains that string, nothing is written — which is what makes a command typed into the wrong shell impossible when tabs get reordered mid-flight.",
    ],
    upstream:
      "Microsoft considered this capability twice — issue #9368 and PR #20106 — and passed on both.",
  },

  "link-card": {
    problem:
      "The hover card for a link could open past the edge of the window, so the thing you hovered to read was partly off screen — worst exactly when a pane was narrow, which is when you most need to know where a link goes.",
    how:
      "The card is measured against the actual viewport rather than only the configured maximum width, then flipped horizontally as well as vertically when there is no room on the preferred side, and finally clamped on both axes. It also resolves what the link will actually open, which for a WSL pane means turning a POSIX path into the <code>\\\\wsl.localhost\\&lt;distro&gt;</code> path Windows can follow.",
    steps: [
      "Hover any link in a pane. The card appears near the cursor.",
      "Narrow the pane, or hover a link near the right edge, and the card flips to the side with room rather than overflowing.",
      "In a WSL pane, hover a bare absolute path such as <code>/home/you/notes.md</code> — it resolves to the Windows path that opens it.",
    ],
    settings: [
      { key: "hyperlink.tooltipShowDelay", def: "0", note: "Milliseconds before the card appears." },
      { key: "hyperlink.tooltipHideDelay", def: "300", note: "Milliseconds before it goes away." },
      { key: "hyperlink.tooltipMaxWidth", def: "640", note: "Cap on the card's width, further limited by the pane." },
    ],
    media: [
      { base: "link-card", caption: "The card names the link, says what a click will do, and shows the path it will really open — with the actions that were previously buried in a right-click menu." },
    ],
  },

  "wsl-links": {
    problem:
      "A path printed inside WSL means nothing to Windows. Click <code>/home/you/notes.md</code> and Windows looks for a folder called <code>home</code> at the root of your system drive, finds nothing, and the click does nothing — even though the file is right there and openable.",
    how:
      "The distro is worked out from the profile's own launch command, so a Debian pane resolves as Debian rather than as whichever distro happens to be the default. A POSIX path is then rewritten to <code>\\\\wsl.localhost\\&lt;distro&gt;\\…</code>, which Windows can open directly. Paths that already make sense to Windows — a drive letter, a UNC path, <code>/mnt/c/…</code> — are left alone, because they resolve without needing to know the distro at all.",
    steps: [
      "In a WSL pane, print a path: <code>ls -d ~/Documents</code>, or anything that emits an absolute path.",
      "Hover it. The card shows the <code>\\\\wsl.localhost\\…</code> path it will actually open.",
      "Click it. Explorer opens the real file, inside the distro.",
    ],
    media: [
      { base: "link-card", caption: "Hovering a file inside WSL. The arrow shows what will actually open: the \\\\wsl.localhost path for this pane's distro, not the POSIX path as written." },
    ],
    notes: [
      "Resolution is deliberately gated on the pane really being WSL. Without that, a Windows program printing <code>file:///etc/hosts</code> into a PowerShell tab would silently resolve against whichever distro is default.",
    ],
  },

  "plain-click": {
    problem:
      "Opening a link meant Ctrl+clicking it, and inside anything using the mouse — tmux, an editor, a TUI — links stopped being clickable at all, because the application had taken the mouse.",
    how:
      "A plain click follows a link. When an application has requested mouse reporting, the click still reaches the application as it must, but a link under the cursor is recognised first, so both behaviours coexist instead of one shutting the other out.",
    steps: [
      "Click any URL in a pane — no modifier.",
      "Start something that captures the mouse, such as <code>tmux</code>, print a URL inside it, and click that too.",
    ],
  },

  "slack-links": {
    problem:
      "Tools that emit Slack-style links write them as <code>&lt;https://example.com|the label&gt;</code>. A terminal that doesn't understand that shows the whole thing as literal text, punctuation and all.",
    how:
      "The bracketed form is recognised and rendered as its label, linking to its target — the same text, minus the plumbing.",
    steps: [
      "Print one: <code>printf '&lt;https://example.com|the label&gt;\\n'</code>.",
      "It renders as <em>the label</em>, and clicking it opens the URL.",
    ],
  },

  "color-schemes": {
    problem:
      "A terminal that follows the system light/dark setting still had a single colour scheme, so switching the OS to light left you with a dark scheme on a light window — or meant editing settings twice a day.",
    how:
      "<code>colorScheme</code> accepts a pair rather than a single name. The terminal picks whichever matches the current appearance and re-picks when the OS changes, with no restart.",
    steps: [
      "Set a pair in your profile: <code>\"colorScheme\": { \"light\": \"One Half Light\", \"dark\": \"One Half Dark\" }</code>.",
      "Change the Windows appearance setting. The pane follows immediately.",
    ],
    settings: [
      { key: "profiles.defaults.colorScheme", def: "—", note: "A scheme name, or a <code>{ light, dark }</code> pair." },
      { key: "theme", def: "\"system\"", note: "This fork follows the OS by default; upstream ships \"dark\"." },
    ],
  },

  "defaults": {
    problem:
      "Three small things that go wrong often enough to matter: the mouse wheel changing font size or window opacity when a modifier was still held, being asked whether to close all windows when only one is open, and having no quick way to look up selected text.",
    how:
      "Selected text can be searched with your default engine. The wheel no longer rebinds itself to zoom or opacity. Quitting with a single window open just quits.",
    steps: [
      "Select some text and use the search action.",
      "Scroll with a modifier held — the font stays where you put it.",
    ],
  },

  "window-persist": {
    problem:
      "Every new window opened wherever Windows felt like putting it, so a window you had sized and placed came back somewhere else.",
    how:
      "Position and size are remembered per window and restored on the next open. This is tracked separately from tab persistence, so it works whether or not you restore layouts.",
    settings: [
      { key: "rememberWindowGeometry", def: "true", note: "Reopen a window where it was, at the size it was." },
    ],
    upstream: "Requested upstream as microsoft/terminal#12633.",
  },

  "icons": {
    problem:
      "A profile icon pointing at an http(s) URL made settings load wait on the network, and a profile using a glyph or emoji icon showed up in the jump list as a blank tile.",
    how:
      "Remote icons are fetched once and cached locally, so later loads never touch the network. Glyph and emoji icons are rasterised into real icon files for the jump list, and refreshed when a new build is deployed.",
  },

  "slot-system": {
    problem:
      "Running two builds of the same app side by side, one disposable and one you actually work in, means constantly answering \"which one is this?\" — and getting it wrong means testing in the window holding your live sessions.",
    how:
      "A badge in the tab row names the slot. Clicking it copies the build details. The About dialog carries the commit and build time, and when a newer build has been staged a banner says so, names it, and offers to promote it.",
    media: [
      { base: "slot-badge", caption: "The tab-row badge naming the slot." },
    ],
  },

  "distinct-identity": {
    problem:
      "Windows runs one instance per app identity. The disposable test build shared an identity with the live one, so launching it silently handed off to the live window — you thought you were testing, and you were running the other build's code in the window holding your real work.",
    how:
      "Each slot has its own package identity, so its window class and single-instance mutex differ. The disposable build opens as itself, beside the live one.",
    notes: [
      "Two accepted limits: the test slot declares fewer COM registrations, so no default-terminal handoff or Explorer verb; and its <code>wt.exe</code> path resolution points somewhere that doesn't exist, because that check matches the dev identity by name.",
    ],
  },

  "auto-promote": {
    problem:
      "Installing a staged build means closing the window it would replace. Doing that blind can discard live sessions; asking every single time, even when the window is empty, is friction for nothing.",
    how:
      "A read-only check inspects what is actually running under the production build. It allows an unattended promote in exactly two cases: nothing running at all, or a single window with a single pane whose foreground process is a session multiplexer — where the work lives in the multiplexer's own server and survives the window closing. Anything else, including anything ambiguous, fails closed and asks.",
    notes: [
      "An empty slot is treated as the case needing the <em>most</em> care, not the least: nothing fails, nothing warns, and the next launch silently runs whatever was written there.",
    ],
  },

  "ci-poll": {
    problem:
      "Finding out that a new build exists meant remembering to go and look.",
    how:
      "A scheduled task fetches the newest successful build and stages it, so the update offer appears on its own. Staging is automatic; installing stays a deliberate action.",
    settings: [
      { key: "ciPollIntervalMinutes", def: "15", note: "How often to check. 0 never checks." },
    ],
    notes: [
      "The check runs genuinely hidden. On a machine where Windows Terminal is the default terminal host, a hidden console process still gets a Terminal window created for it before the hide request applies — so these launches go through a wrapper that avoids that handoff entirely.",
    ],
  },

  "ci-tests": {
    problem:
      "Builds were made on the developer machine. A warm build tree runs to tens of gigabytes, every worktree pays for its own copy, and nothing verified a build except opening it.",
    how:
      "CI restores, builds, runs the unit suites that pass cleanly, packages both slots, and uploads them as one artifact. A fetch script stages both halves locally. The same deploy script produces the payload in CI as on a developer machine, so the two paths cannot drift.",
    notes: [
      "Green means it compiled, packaged and passed those two suites — not that any behaviour was confirmed. Anything involving the UI still needs a run in the disposable slot.",
    ],
  },
};
