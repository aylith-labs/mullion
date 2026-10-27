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
//   media     a list of:
//             { base, caption } — a screenshot, media/<base>-light.png and
//               media/<base>-dark.png, or -.mp4 for kind: "video"
//             { kind: "compare", before, after, caption, aligned?, frame? } —
//               upstream beside Mullion (compare.js). aligned: true only when the
//               two shots line up pixel for pixel; frame: "square" drops the
//               rounded frame for a shot whose corners are the point
//             { kind: "evidence", caption, columns, source?, note?, before?, after } —
//               text evidence (a UIA tree, a CI run) as a real table, never a
//               picture of one (compare.js)
//   upstream  what Microsoft did about it, where that is known
window.FEATURE_DETAILS = {

  "dock-fit": {
    problem:
      "Windows 11 rounds the corners of every top-level window. A quake window, or one docked to a screen edge, sits flush against that edge, so the rounding leaves a wedge of desktop showing at each corner. And a docked window was sized against the work area once, when it docked: when the taskbar changed size or another app bar (Copilot, in the upstream report) took a strip of the screen, the window kept covering it, or left a gap.",
    how:
      "While a window is in quake mode or docked, it asks the Desktop Window Manager for square corners (<code>DWMWA_WINDOW_CORNER_PREFERENCE</code>), and hands them back to the system default when it stops being docked. A quake window is named before its window exists, so the corners are applied again once it does. Windows 10 does not know the attribute and has nothing to undo. For the work area, the window listens for <code>WM_SETTINGCHANGE</code> with <code>SPI_SETWORKAREA</code>, which the taskbar and every app bar broadcast, and re-fits from a message it posts to itself rather than inside the broadcast. It is never shown, raised or activated by this. A minimized window is marked and re-fitted when it is next restored. A fullscreen or maximized window is left alone. The dock keeps the thickness you dragged it to and takes only its edge and its length from the new work area, and a broadcast that leaves this monitor's work area unchanged does nothing.",
    steps: [
      "Summon a quake window (<code>wt -w _quake</code>, or the Quake mode action), or set <code>Dock the window</code> under Personalization.",
      "Look at its corners on Windows 11: square, flush with the screen edge.",
      "Change the taskbar's size, or turn on an app bar that reserves screen space. The docked window re-fits to the new edge.",
      "Undock it, or leave quake mode, and the corners round again.",
    ],
    settings: [
      { key: "dockWindow", def: "\"off\"", note: "Any value but off docks the window, and with it gets the square corners and the work-area re-fit. Quake mode always has both." },
    ],
    media: [
      { kind: "compare", aligned: true, frame: "square", before: "dock-fit-corners-before", after: "dock-fit-corners-after", caption: "The top corners of a quake window on Windows 11: rounded in upstream, square in Mullion." },
      { kind: "compare", aligned: true, before: "dock-fit-workarea-before", after: "dock-fit-workarea-after", caption: "A docked window after the taskbar grows: upstream keeps its old size and runs under the taskbar; Mullion re-fits." },
    ],
    notes: [
      "The re-fit never makes a dismissed quake window reappear. Dismissing minimizes and hides it, so it is only marked, and fitted when you summon it.",
    ],
    upstream:
      "Both are open upstream. #10534 (2021): a maintainer said quake windows should not be rounded, and nothing landed. #16067 (2023): the maintainer named the fix, listening for <code>SPI_SETWORKAREA</code>, and it was never written.",
  },

  "early-upstream-fixes": {
    problem:
      "Some upstream bugs have a fix that is written, tested and reviewed, and then waits in the queue for months. Others are small, labelled Help Wanted, and have sat open for years with nobody on them. Running a fork means neither has to wait.",
    how:
      "A fix from upstream is cherry-picked with <code>git cherry-pick -x</code>, which keeps the original author and records the upstream commit each one came from, including any changes a maintainer asked for in review; when upstream merges it, the next merge brings in the same change and git reconciles the two. A fix written here is kept small and names the issue in its commit and in the code, so the next upstream merge conflicts as little as possible.",
    notes: [
      "Only fixes whose review has settled are taken from upstream. A pull request still being argued over is left to finish.",
      "A fix written here goes no further: Mullion opens nothing against microsoft/terminal.",
    ],
  },

  "close-tab-tooltip": {
    problem:
      "Hovering a tab's close button says <em>Close Tab (Ctrl+F4)</em>. Ctrl+F4 is a TabView accelerator that only works while focus is in the tab strip, and nothing in the default keymap binds it, so the hint names a key that does nothing while you are typing in a pane.",
    how:
      "WinUI's TabView gives the close button a fixed tooltip when the template is applied. When each tab is loaded, and whenever the key bindings change, the tooltip is replaced with <em>Close tab</em>, followed by the key bound to <code>closeTab</code> when there is one. <code>closeTab</code> ships unbound and has no <code>Terminal.*</code> id, so the binding is found by action rather than by id, and one that names a tab index is not counted, since it closes some other tab.",
    steps: [
      "Hover a tab's close button: the tooltip says <em>Close tab</em>.",
      "Bind <code>closeTab</code> to a key (Settings, Actions) and hover it again. The tooltip names that key.",
    ],
    media: [
      { kind: "compare", before: "early-fixes-close-tooltip-before", after: "early-fixes-close-tooltip-after", caption: "Hovering the tab close button with <code>closeTab</code> bound to Ctrl+W: upstream still names Ctrl+F4; Mullion names the binding." },
    ],
    upstream:
      "Open since 2021 as #10109, with four reports closed as its duplicates, and no pull request. Upstream tied it to a WinUI change (microsoft-ui-xaml#4930); this replaces the tooltip from the Terminal side instead.",
  },

  "infobar-follows-pane": {
    problem:
      "When a shell exits with an error, a bar across the window suggests changing the profile's <code>closeOnExit</code>. It stayed after the pane it was about had been closed, over a pane it had nothing to do with, until it was dismissed by hand.",
    how:
      "The connection-state event carries only the control's core, so there was no way back from it to a pane. The subscription now hands the handler the control as well, and the page remembers which panes the bar is showing for. Each one's Unloaded triggers a check, and a pane only counts as closed once no tab in the window holds it, because switching tabs, zooming a pane and splitting one all unload a terminal that is still in use. When none is left, the bar closes. Closing it by hand, and <em>Don't show again</em>, are unchanged.",
    steps: [
      "In a split tab, run <code>exit 1</code> in one pane so the bar appears.",
      "Close that pane with Ctrl+Shift+W. The bar closes with it.",
    ],
    media: [
      { kind: "compare", before: "early-fixes-infobar-before", after: "early-fixes-infobar-after", caption: "After closing the pane whose shell exited with code 1: upstream keeps the termination-behavior bar over the pane that is left; Mullion closes it." },
    ],
    upstream:
      "Open since 2021 as #11607, with no pull request. A maintainer sketched the approach (tag the bar with its pane) without anyone building it.",
  },

  "split-pane-duplicate-command": {
    problem:
      "<code>wt split-pane -D pwsh -c foo</code> duplicated the focused pane and dropped <code>pwsh -c foo</code> without a word. Moving <code>-D</code> after the command only seemed to work because it then became an argument to pwsh.",
    how:
      "A duplicate copies the focused pane's profile and working directory, and the duplicate path never looked at the command line it was given. A command line (respecting <code>appendCommandLine</code>) and a <code>-d</code> directory given with <code>-D</code> now win, as they do on a manual split. Elevation still comes from the copied profile.",
    steps: [
      "In a Mullion pane, run <code>wt -w 0 split-pane -D cmd /k echo duplicated</code>.",
      "The new pane uses the focused pane's profile and directory and runs the echo.",
    ],
    media: [
      { kind: "compare", before: "early-fixes-dup-before", after: "early-fixes-dup-after", caption: "<code>split-pane -V -D -d C:\\Temp\\demo cmd /k echo …</code>: look at the right-hand pane, a plain copy of Command Prompt upstream and the typed command in Mullion." },
    ],
    upstream: "Open since 2024 as #17481, with no pull request.",
  },

  "scroll-marks-after-clear": {
    problem:
      "With scroll marks on, the scrollbar marks each prompt. Clear Buffer, <code>clear</code> and <code>cls</code> erased the text but left its marks, so the scrollbar kept pointing at prompts that were gone.",
    how:
      "Upstream's own fix, <a href='https://github.com/microsoft/terminal/pull/20105'>PR #20105</a>, cherry-picked with its authorship. Clear Buffer removes the marks in the rows it erases, and so do <code>clear</code> and <code>cls</code>, which erase the screen with <code>ESC[2J</code> rather than going through Clear Buffer.",
    steps: [
      "Turn on <code>showMarksOnScrollbar</code> (<code>autoMarkPrompts</code> is on by default) and run a few commands.",
      "Run <code>clear</code>. The marks go with the text.",
    ],
    settings: [
      { key: "autoMarkPrompts", def: "true", note: "Marks each prompt, which is where the scroll marks come from." },
      { key: "showMarksOnScrollbar", def: "false", note: "Shows those marks on the scrollbar." },
    ],
    media: [
      { kind: "compare", aligned: true, before: "early-fixes-marks-before", after: "early-fixes-marks-after", caption: "The scrollbar after clear: upstream still shows marks for the erased prompts; Mullion clears them." },
    ],
    upstream:
      "#20086 is open; its fix, PR #20105, is approved in substance and not yet merged. A maintainer asked for the <code>clear</code>/<code>cls</code> path to be covered too, and the author added it; both commits are here.",
  },

  "settings-accessibility": {
    problem:
      "A screen reader user moves around a page by its landmarks. The Settings page had the navigation pane as one and nothing for the page content, so there was no direct way to jump from the menu to the settings themselves.",
    how:
      "The scroll region that holds whichever settings page is open is marked <code>AutomationProperties.LandmarkType=\"Main\"</code>. UI Automation then reports a main landmark beside the navigation one, and Narrator's landmark navigation (and any screen reader's) can reach it.",
    steps: [
      "Open Settings and start Narrator (Win+Ctrl+Enter).",
      "Press D (Narrator's landmark key) to move between landmarks: the navigation pane, then the main content.",
    ],
    media: [
      {
        kind: "evidence",
        caption: "The landmarks UI Automation reports on the Settings page: only navigation upstream; navigation and main in Mullion.",
        source: "every element of the Settings tab, read for UIA_LandmarkTypePropertyId (30157)",
        columns: ["Landmark", "Control type", "AutomationId"],
        before: {
          label: "Windows Terminal Canary 1.27.2931.0 (upstream)",
          rows: [["Navigation", "Button", "TogglePaneButton"], ["Navigation", "LandmarkTarget", "NavigationBreadcrumbBar"], ["Main", null, null]],
          mark: [2],
        },
        after: {
          label: "Mullion (Test slot)",
          rows: [["Navigation", "Button", "TogglePaneButton"], ["Navigation", "LandmarkTarget", "NavigationBreadcrumbBar"], ["Main", "ScrollViewer", "SettingsMainPage_ScrollViewer"]],
          mark: [2],
        },
        note: "Read with UI Automation from outside the process. No input was sent.",
      },
    ],
    upstream: "Open since 2021 as #12004, one of a batch of accessibility reports against Settings that are still open upstream.",
  },

  "promote-from-tab-row": {
    problem:
      "A new build of Mullion is only useful if it gets installed, and installing it means closing the window your work is in. Without something telling you a build is waiting, and what is in it, the update either gets forgotten or gets taken blind.",
    how:
      "The CI poller stages each successful build beside the live one and writes a marker describing it. The Dev slot reads every marker and, when one is newer than what it is running (compared by build time first, so an older build is never offered), shows it on the tab row. The promote dialog lists every commit between the running build and the staged one, read from the release notes the build was compiled with, with a link to the full comparison on GitHub. Promoting swaps the staged payload into place rather than unpacking over the live one, a named mutex lets only one promotion run at a time, and while the swap waits for windows to close the dialog says which processes it is waiting for and counts down.",
    steps: [
      "Push a commit and let CI build it; the poller stages it within its interval.",
      "The tab row offers the staged build. Open the dialog to read what it adds.",
      "Promote when nothing you mind losing is running. The Terminal restarts on the new build.",
    ],
    media: [
      { kind: "compare", aligned: true, before: "promote-tab-row-before", after: "promote-tab-row-after", caption: "The tab row with a newer build staged: look right of the new-tab dropdown, where upstream shows nothing and Mullion shows the Update button." },
      { base: "promote-dialog-after", caption: "Update pressed: the staged commit and its age, the latest commits each linked to its feature page and upstream issues, and the choice between applying now and on next launch." },
    ],
    notes: [
      "Only the Dev slot offers promotion. The Test slot is refreshed by a script, on purpose, so it never changes under a session that is using it.",
    ],
    upstream: "Not applicable upstream: the Store and winget handle updates there, and nothing in the app says what an update contains.",
  },

  "remote-file-previews": {
    problem:
      "A pane can be working on another machine (a shefrd pane connected to a saved machine), and the paths it prints are paths on that machine. Hovering one looked it up locally and found nothing, or worse, found a different file at the same path. And a <code>file://</code> link naming another host resolved to a network share: hovering it opened an SMB connection to whatever machine a program chose to print, with no click.",
    how:
      "The pane's OSC 7 working-directory report names the machine it is on. With <code>hyperlink.remoteFilePreviews</code> on, a path in such a pane is read from that machine through <code>shefrd machine read</code>, with a five-second limit and the same 1 MiB text cap as local previews, and the card says <em>on &lt;machine&gt;</em>. Separately and always: a <code>file://</code> URI whose host is not this machine or WSL, a bare <code>\\\\server\\share</code> path, a device path, and an image inside a previewed Markdown file that points at another host are all refused before any filesystem call.",
    steps: [
      "Set <code>hyperlink.remoteFilePreviews</code> to true.",
      "In a shefrd pane connected to another machine, list some files and hover one.",
      "The card shows the file's contents from that machine and names it.",
    ],
    settings: [
      { key: "hyperlink.remoteFilePreviews", def: "false", note: "Read paths in a pane that is on another machine from that machine. Text files only." },
    ],
    media: [
      { kind: "compare", before: "remote-previews-card-before", after: "remote-previews-card-after", caption: "Hovering a path printed on another machine: upstream has nothing to show; Mullion previews it from that machine." },
    ],
    notes: [
      "The share refusal applies whether or not remote previews are on. Clicking such a link keeps its existing \"not supported\" message.",
    ],
    upstream: "No upstream equivalent. Upstream's file:// handling (#8981, closed) never reached remote hosts, and its hover card does not read files at all.",
  },

  "session-restore-page": {
    problem:
      "Session restore's settings were scattered across the bottom of the Startup page, and the save interval was in whole minutes: five minutes was the least you could lose to a crash.",
    how:
      "The nine settings that make up session restore now have their own page, second in the navigation. <em>When Terminal starts</em> appears on both pages, since it decides whether anything is restored at all, and everything under it greys out together when restoring is off, not just the first level. Pane contents are saved on a seconds clock, <code>bufferPersistIntervalSeconds</code>, from 5 seconds to 4 hours, and the page draws what each interval costs. A value saved under the old minutes key is carried over as minutes × 60.",
    steps: [
      "Open Settings → Session Restore.",
      "Set <em>When Terminal starts</em> to restore the previous session.",
      "Pick a save interval and read its cost in the chart beside it.",
    ],
    settings: [
      { key: "persistBufferPeriodically", def: "true", note: "Save pane contents on a timer, not only on a clean exit." },
      { key: "bufferPersistIntervalSeconds", def: "300", note: "How often, in seconds: 5 to 14,400." },
    ],
    media: [
      { kind: "compare", before: "session-restore-page-before", after: "session-restore-page-after", caption: "Where session restore lives: one dropdown at the top of Startup upstream; a page of its own in Mullion." },
    ],
    upstream: "Upstream restores buffer contents (#961) and has an open PR to save them periodically (#19805), in minutes. Neither gives restore a page.",
  },

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
      { key: "bufferPersistIntervalSeconds", def: "300", note: "How often that happens, from 5 seconds to 4 hours. An older <code>bufferPersistIntervalMinutes</code> is read as minutes and carried over." },
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
      "Each window serves a named pipe, <code>\\\\.\\pipe\\wt-control-&lt;pid&gt;</code>, speaking newline-delimited JSON. The core operations are <code>ping</code>, <code>list-panes</code>, <code>capture-pane</code> and <code>send-input</code>; <code>focus-pane</code> and <code>run-action</code> came later. <code>run-action</code> takes an action exactly as <code>settings.json</code> spells it, read by the settings model's own parser, and runs it in the named pane as if its key had been pressed, without raising the window. It must name text the pane is showing, so a stale address does nothing, and it refuses the few actions that would open a window, run a modal loop, elevate or write the settings file. The pipe is created with a DACL granting the current user's SID and nothing else, and rejects remote clients. It cannot change settings, and every request is recorded.",
    steps: [
      "Leave <code>controlPipe</code> on (it is by default), or set it to false to close the pipe entirely.",
      "Find the pipe: it is named after the Terminal process id, so a client can enumerate <code>\\\\.\\pipe\\wt-control-*</code> and talk to every Terminal running.",
      "Send <code>{\"op\":\"list-panes\"}</code> to see every window, tab and pane, with the process each is running.",
      "Send <code>{\"op\":\"capture-pane\",\"pane\":\"1.0.0\",\"lines\":20}</code> to read back what that pane printed.",
      "Send <code>{\"op\":\"send-input\",\"pane\":\"1.0.0\",\"text\":\"...\"}</code>, then <code>\"\\r\"</code> as a second call, to type into it.",
      "Send <code>{\"op\":\"run-action\",\"pane\":\"1.0.0\",\"requireContains\":\"...\",\"action\":\"clearBuffer\"}</code> to run an action in that pane, as long as it still shows that text; nearly any action <code>settings.json</code> can bind works, with its arguments as an object.",
    ],
    settings: [
      { key: "controlPipe", def: "true", note: "Serve the pipe. Set false to turn it off completely." },
    ],
    media: [
      { kind: "compare", before: "control-pipe-list-upstream", after: "control-pipe-list", caption: "list-panes: every window, tab and pane, and what each one is running." },
    ],
    notes: [
      "The Enter goes as its own call, never combined with the text. A single write containing both is read by some TUIs as a bulk paste and never submits.",
      "<code>send-input</code> takes an optional <code>requireContains</code>, re-checked atomically with the write. If the pane's screen no longer contains that string, nothing is written — which is what makes a command typed into the wrong shell impossible when tabs get reordered mid-flight.",
    ],
    upstream:
      "Microsoft considered this capability twice — issue #9368 and PR #20106 — and passed on both. For <code>run-action</code> the nearest upstream ask is #9994, running actions from <code>wt.exe</code>, which is still open; nothing upstream runs an action against a window that is already open.",
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
      { key: "hyperlink.tooltipShowDelay", def: "250", note: "Milliseconds before the card appears." },
      { key: "hyperlink.tooltipHideDelay", def: "400", note: "Milliseconds before it goes away." },
      { key: "hyperlink.tooltipMaxWidth", def: "640", note: "Cap on the card's width, further limited by the pane." },
    ],
    media: [
      { kind: "compare", aligned: true, before: "link-card-hover-upstream", after: "link-card-hover", caption: "Hovering an OSC 8 link to a file in WSL: upstream's tooltip shows the raw file:/// URI and asks for Ctrl+Click; Mullion's card shows the path it will really open, previews the file, and carries the actions that were previously buried in a right-click menu." },
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
    settings: [
      { key: "hyperlink.remoteFilePreviews", def: "false", note: "In a pane whose OSC 7 says it is on another machine (a shefrd pane showing a saved machine), read its paths from that machine through <code>shefrd machine read</code> instead of looking them up in WSL. The card says <em>on &lt;machine&gt;</em>; text files only, and a quiet <em>Preview unavailable</em> when the machine is not connected." },
    ],
    media: [
      { kind: "compare", aligned: true, before: "link-card-hover-upstream", after: "link-card-hover", caption: "Hovering a file inside WSL: upstream shows only the file:/// URI as written; in Mullion the arrow shows what will actually open, the \\\\wsl.localhost path for this pane's distro." },
    ],
    notes: [
      "Resolution is deliberately gated on the pane really being WSL. Without that, a Windows program printing <code>file:///etc/hosts</code> into a PowerShell tab would silently resolve against whichever distro is default.",
    ],
  },

  "plain-click": {
    media: [
      { kind: "compare", aligned: true, before: "plain-click-hover-upstream", after: "plain-click-hover", caption: "Hovering a URL: upstream's tooltip says Ctrl+Click to follow link; Mullion's card asks for no modifier, because a plain click follows it, and offers Open, Copy link and Show in pane." },
    ],
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
      "Tools that emit Slack-style links write them as <code>&lt;https://example.com|the label&gt;</code>. Plain URL detection allows a pipe inside a URL, so the link it found was <code>https://example.com|the</code>: the address with the first word of the label stuck to it.",
    how:
      "The bracketed form is recognised as one link, from the opening bracket to the closing one, label included. Hovering or clicking anywhere on it uses the URL before the pipe; the text on screen stays exactly as the program printed it.",
    steps: [
      "Print one: <code>printf '&lt;https://example.com|the label&gt;\\n'</code>.",
      "Hover any part of it, label included: the card shows <code>https://example.com</code>, and clicking opens it.",
    ],
    media: [
      { kind: "compare", aligned: true, before: "slack-links-upstream", after: "slack-links", caption: "A Slack-style link, a URL and a label split by a pipe: upstream's match runs through the pipe into the label (…/4182|deploy); in Mullion it is one link from bracket to bracket, and the card shows the URL before the pipe." },
    ],
  },

  "color-schemes": {
    problem:
      "The terminal can already use one scheme in light mode and another in dark, but only if you write the pair into settings.json by hand. The Settings UI offers a single scheme, so choosing one there replaces the pair.",
    how:
      "The colour scheme row under Profiles → Appearance offers a light and a dark scheme side by side and writes them as a <code>{ light, dark }</code> pair. The terminal picks whichever matches the app theme and re-picks when it changes, with no restart; that part is upstream's.",
    steps: [
      "Open Settings → a profile (or Defaults) → Appearance, and tick <strong>Separate light and dark mode</strong> in the colour scheme row.",
      "Choose a scheme for each, then Save. The file now holds <code>\"colorScheme\": { \"light\": \"…\", \"dark\": \"…\" }</code>.",
      "Change the Windows appearance setting. The pane follows immediately.",
    ],
    settings: [
      { key: "profiles.defaults.colorScheme", def: "—", note: "A scheme name, or a <code>{ light, dark }</code> pair." },
      { key: "theme", def: "\"system\"", note: "This fork follows the OS by default; upstream ships \"dark\"." },
    ],
    media: [
      { kind: "compare", before: "color-schemes-settings-upstream", after: "color-schemes-settings", caption: "Profiles → Defaults → Appearance: upstream has one Color scheme picker, so in dark mode the preview stays One Half Light; Mullion has Dark and Light pickers under Separate light and dark mode, and the preview follows the theme." },
    ],
  },

  "defaults": {
    media: [
      { kind: "compare", before: "defaults-first-run-upstream", after: "defaults-first-run", caption: "A fresh settings file: upstream defaults beside Mullion's quieter ones." },
    ],
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
    media: [
      { kind: "compare", before: "window-persist-reopen-upstream", after: "window-persist-reopen", caption: "Close a window and open a new one: upstream opens at the default position; Mullion opens where the last one was." },
    ],
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
    media: [
      { kind: "compare", before: "icons-jumplist-upstream", after: "icons-jumplist", caption: "The taskbar jump list for profiles with web icons: blank in upstream; drawn from a local copy in Mullion." },
    ],
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
      { kind: "compare", before: "slot-badge-upstream", after: "slot-badge", caption: "The tab-row badge naming the slot." },
    ],
  },

  "distinct-identity": {
    media: [
      { kind: "compare", before: "distinct-identity-launch-upstream", after: "distinct-identity-launch", caption: "Launching the test build while the live one is open: upstream branding hands it off to the live window; Mullion opens a window of its own." },
    ],
    problem:
      "Windows runs one instance per app identity. The disposable test build shared an identity with the live one, so launching it silently handed off to the live window — you thought you were testing, and you were running the other build's code in the window holding your real work.",
    how:
      "Each slot has its own package identity, so its window class and single-instance mutex differ. The disposable build opens as itself, beside the live one.",
    notes: [
      "Two accepted limits: the test slot declares fewer COM registrations, so no default-terminal handoff or Explorer verb; and its <code>wt.exe</code> path resolution points somewhere that doesn't exist, because that check matches the dev identity by name.",
    ],
  },

  "auto-promote": {
    media: [
      { kind: "compare", before: "auto-promote-verdict-upstream", after: "auto-promote-verdict", caption: "Promoting with a window open: without the idle check the promotion waits on you; with it, an idle window is closed and the build goes in." },
    ],
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
    media: [
      { kind: "compare", before: "ci-poll-setting-upstream", after: "ci-poll-setting", caption: "The top of Settings → Compatibility: upstream's first group ends at Width of ambiguous characters; Mullion adds CI build update poll interval (minutes) below it, marked ◆ as a fork setting." },
    ],
    notes: [
      "The check runs genuinely hidden. On a machine where Windows Terminal is the default terminal host, a hidden console process still gets a Terminal window created for it before the hide request applies — so these launches go through a wrapper that avoids that handoff entirely.",
    ],
  },

  "ci-tests": {
    media: [
      {
        kind: "evidence",
        caption: "One push to main, as CI ran it: the highlighted rows are the build, the two unit suites that gate it, and the one artifact that carries both slots.",
        source: "gh run view 39949539634 --repo aylith-labs/mullion --json jobs",
        columns: ["Step", "Result", "Time"],
        after: {
          label: '<a href="https://github.com/aylith-labs/mullion/actions/runs/39949539634">aylith-labs/mullion · build.yml</a> · push of <a href="https://github.com/aylith-labs/mullion/commit/9b415976249fe53729c35f77d0c2b2d79723b870"><code>9b41597</code></a> · success in 44m 31s',
          rows: [
            ["Promotion helper regression tests", "success", "12s"],
            ["Settings model consistency", "success", "1s"],
            ["XAML comments · XAML resource keys", "success", "2s"],
            ["Native file preview preflight", "success", "44s"],
            ["Release notes", "success", "35s"],
            ["Restore and build", "success", "39m 29s"],
            ["Unit tests (Types, Control)", "success", "8s"],
            ["Unit tests (SettingsModel, diagnostic only)", "success", "2m 28s"],
            ["Upload artifact: dev-payload (both slots, 17.8 MB)", "success", "5s"],
            ["Collect and upload symbols", "success", "18s"],
          ],
          mark: [5, 6, 8],
        },
        note: "Fetch-CIBuild.ps1 then stages dev-payload into dev-staged-ci (muld) and test-staged-ci (mult).",
      },
    ],
    problem:
      "Builds were made on the developer machine. A warm build tree runs to tens of gigabytes, every worktree pays for its own copy, and nothing verified a build except opening it.",
    how:
      "CI restores, builds, runs the unit suites that pass cleanly, packages both slots, and uploads them as one artifact. A fetch script stages both halves locally. The same deploy script produces the payload in CI as on a developer machine, so the two paths cannot drift.",
    notes: [
      "Green means it compiled, packaged and passed those two suites — not that any behaviour was confirmed. Anything involving the UI still needs a run in the disposable slot.",
    ],
  },

  "link-rules": {
    problem:
      "Every link got the same card. There was no way to say that a Jira key in a build log should be hoverable at all, that image links should show without waiting, or that one kind of link needs a button only your setup knows how to fill in. Upstream detects its own fixed set of URL patterns, plus whatever a program marks with OSC 8, and nothing else.",
    how:
      "Rules live in <code>hyperlink.tooltipRules</code> and are edited on Settings → Link Tooltip. A rule's match criteria — URI schemes, a pattern, a file-type group, extra extensions — are ANDed, and an unset criterion is skipped. A <em>Link</em> rule tests detected and OSC 8 links, its pattern searched anywhere in the link, case-insensitively; a <em>Text</em> rule's pattern is scanned over the output itself, case-sensitively, and whatever it finds becomes hoverable even though nothing marked it as a link (up to 16 text patterns are active at once). Both are ICU regular expressions with named groups — the engine the buffer scanner and the preview pipeline already used — because link rules compiled with <code>std::wregex</code> threw on every <code>(?&lt;name&gt;…)</code> and silently never matched. The first enabled rule that matches wins and nothing merges. With <code>hyperlink.manualRuleOrder</code> off the list is kept grouped by service and sorted by name, so what you see is the order that runs; rules the page cannot place go last, so a rule of your own never outranks a built-in one. Everything a rule changes is an override — show and hide delay, maximum width, the button list, pane versus tooltip, primary and alternative click action — and unticked means inherit. A custom button names an entry under <code>actions</code>; for <code>sendInput</code> and <code>wt</code> actions, <code>%u</code> in its argument is replaced with the hovered link. The editor opens with a Rule Preview card that takes a line of sample text and shows what the rule actually finds in it: the matched run highlighted where it sits, the named captures (<code>key = ACME-8209</code>), and whether the pattern compiles at all.",
    steps: [
      "Open Settings → Link Tooltip. Under <code>Rules</code>, open the <code>Add rule</code> menu and pick <em>Jira: Issue keys in output, like ACME-8209</em>. Presets already in your list are greyed out.",
      "Open the new rule. The Rule Preview card starts with the preset's own example, <code>Deployed ACME-8209 to staging</code>, with <code>ACME-8209</code> highlighted and <code>key = ACME-8209</code> under it. Edit the line and watch the match follow.",
      "Turn on <code>Show which rule matched</code> in the Defaults half of the page.",
      "In a pane, run <code>echo 'Deployed ACME-8209 to staging'</code> and hover the key. It is hoverable, and the card's far edge says <em>Matched by</em> and the rule's name. Click the name and Settings opens on that rule.",
      "Hover an ordinary URL. The same line says <em>No rule matched</em> — the answer to \"why is this link not previewed the way I set it up\".",
      "Back in the rule, open <code>Overrides</code>, tick <code>Show delay</code> and set it to 0. Only links this rule matches now show their card at once.",
      "Add a <em>Link</em> rule with <code>md</code> under extensions. Under <code>Built-in buttons</code>, tick <code>Override the default buttons for this rule</code> and leave only <code>Copy link</code> checked. Hover the path of a Markdown file: the card shows Copy link and a <code>⋯</code> button, and the ⋯ menu holds Open, Show in Explorer and Show in pane.",
    ],
    settings: [
      { key: "hyperlink.tooltipRules", def: "[]", note: "The rules, in precedence order. Per rule: <code>match</code> (<code>link</code> or <code>text</code>), <code>schemes</code>, <code>pattern</code>, <code>fileTypeGroup</code>, <code>extensions</code>, <code>showDelay</code>, <code>hideDelay</code>, <code>maxWidth</code>, <code>buttons</code>, <code>showInPane</code>, <code>primaryAction</code>, <code>alternativeAction</code>, <code>actions</code>, <code>integration</code>, <code>preview</code>." },
      { key: "hyperlink.manualRuleOrder", def: "false", note: "Off, rules are grouped by service and sorted by name. On, they become one flat list you drag into order." },
      { key: "hyperlink.tooltipShowRule", def: "false", note: "Name the rule the card came from, or say that none matched. The name opens that rule in Settings." },
    ],
    media: [
      { base: "link-rules-editor", caption: "A rule open in the editor. The Rule Preview card shows what the pattern finds in a line of output, and the named capture it reads back." },
      { base: "link-rules-matched", caption: "With Show which rule matched on, the card names the rule that produced it." },
    ],
    notes: [
      "The automatic group order is GitHub, Unblocked Code, Jira, Slack, Stith, shefrd, git, files, then anything else. Unblocked sits above Jira on purpose: a specific <code>UNB-123</code> task id has to be tried before Jira's broad <code>PROJECT-number</code> pattern would claim it.",
      "A preset is a starting point written into your rule, not a live link. A preset counts as already added if a rule carries its name, even after its pattern has been edited by hand — the dedup asks \"is this preset in my list\", not \"is this regex\".",
      "Opening a rule used to write its displayed default delays and width back into it, ticking the overrides on by themselves. The setters now refuse a write while the override is off; rules created before that may carry values that arrived that way.",
      "Unticking a built-in button tidies the card without taking the action away: whatever is off and could act on the hovered link goes in a <code>⋯</code> menu at the end of the row, and the row is just the ⋯ when every button is off. A button that cannot act on the link is left out of both, such as Copy path on a URL. With <code>hyperlink.tooltipActions</code> off there are no buttons and no menu.",
      "For a path, <em>Copy link</em> copies it as it is written in the terminal, a POSIX path included, and <em>Copy path</em> copies the Windows path it opens, such as <code>\\\\wsl.localhost\\Ubuntu\\home\\…</code>. Neither copies a <code>file://</code> URI. When the two would copy the same text, as for <code>C:\\src</code>, the card shows one button for both: Copy link when Copy path is not in the button list, Copy path otherwise.",
    ],
    upstream:
      "Configurable patterns for turning text into links, with custom handlers, is microsoft/terminal#8849, open since January 2021. A maintainer's spec draft that covered it (#15700, \"Triggers and Custom Clickable Links\") was closed without landing.",
  },

  "click-chords": {
    problem:
      "Upstream has one gesture: Ctrl+click opens. You cannot make a middle click copy a link, keep clicks from launching anything while still getting the card, or make one class of link unfollowable. And turning off <code>experimental.detectURLs</code> to stop the URL guessing also emptied the pattern set that text rules and integrations are scanned from, so a switch meant to silence false positives disabled rules written on purpose.",
    how:
      "<code>experimental.detectURLs</code> now governs only the two built-in URL patterns; rule and integration patterns keep matching with it off. <code>hyperlink.clickable</code> decides whether a click acts at all — off, links are still detected, highlighted and previewed, and the card's buttons are how you act on one — and <code>hyperlink.clickableKinds</code> chooses which of automatically detected URLs, rule matches and OSC 8 links a click reaches. Each of the two chords is a modifier, matched exactly so a Ctrl chord does not fire while Ctrl+Shift is held, plus a gesture. A left-click chord acts on release and only if the click did not become a selection, so dragging across a URL still selects it; middle and double act on press. Alt and Shift also start block and extended selections: on a link the chord wins, everywhere else selection is untouched. A chord runs an action id — one of the card's button ids, <code>none</code>, or an entry under <code>actions</code> — through the same function the card's buttons go through, so a chord bound to <code>copyLink</code> does exactly what the Copy link button does. Before this, a click reached the shell down a different path from the Open button, and a rule's actions were never consulted on click at all. A rule can swap either action for its own; the chords themselves stay global.",
    steps: [
      "Open Settings → Link Tooltip and expand <code>Make links clickable</code>.",
      "Set <code>Alternative action</code> to <code>No modifier</code>, <code>Middle click</code>, <code>Copy link</code>.",
      "In a pane, run <code>echo https://github.com/microsoft/terminal</code>. Middle-click the URL: it is on the clipboard and nothing opens. A plain left click still opens it, and dragging across it still selects.",
      "Under <code>Which links respond to a click</code>, clear <code>Matches from your rules</code>. A rule's text match is still highlighted and still gets its card, but clicking it does nothing.",
      "To make one kind of link unfollowable without turning clicking off everywhere, open that rule and set its <code>Primary action</code> to <code>Do nothing</code>.",
    ],
    settings: [
      { key: "experimental.detectURLs", def: "true", note: "The built-in URL patterns only. Rules and integration patterns match either way." },
      { key: "hyperlink.clickable", def: "true", note: "Off, links are still highlighted and still get the card; you act through its buttons." },
      { key: "hyperlink.clickableKinds", def: "[\"detected\", \"rules\", \"osc8\"]", note: "Which kinds of link a click reaches." },
      { key: "hyperlink.primaryClickModifier", def: "\"none\"", note: "<code>none</code>, <code>ctrl</code>, <code>alt</code>, <code>shift</code>, <code>win</code>, <code>ctrlAlt</code>, <code>ctrlShift</code>, <code>altShift</code> or <code>ctrlAltShift</code>." },
      { key: "hyperlink.primaryClickGesture", def: "\"left\"", note: "<code>left</code>, <code>middle</code> or <code>double</code>. Right click is not offered: it belongs to the context menu and paste." },
      { key: "hyperlink.primaryAction", def: "\"open\"", note: "<code>open</code>, <code>copyLink</code>, <code>copyPath</code>, <code>reveal</code>, <code>showInPane</code>, <code>none</code>, or an id under <code>actions</code>." },
      { key: "hyperlink.alternativeClickModifier", def: "\"ctrl\"", note: "So Ctrl+click still opens, as it does upstream." },
      { key: "hyperlink.alternativeClickGesture", def: "\"left\"", note: "As above." },
      { key: "hyperlink.alternativeAction", def: "\"open\"", note: "As above." },
      { key: "safeUriSchemes", def: "[\"stith\"]", note: "Schemes that open without the unsafe-URI confirmation. Upstream's default is empty. Setting your own list replaces this one; an empty list confirms them all." },
    ],
    media: [
      { kind: "compare", before: "click-chords-settings-upstream", after: "click-chords-settings", caption: "Make links clickable, expanded: which links a click reaches, and the two chords, each a modifier, a gesture and an action." },
    ],
    notes: [
      "The pickers in Settings offer <em>Do nothing</em> and the built-in buttons. An id under <code>actions</code> is written in settings.json; once it is there the picker shows it rather than appearing blank.",
      "This replaces the earlier fork-only single-click switch, so plain click and Ctrl+click both open out of the box.",
    ],
    upstream:
      "Configuring what a link click does is microsoft/terminal#13598, still open, and folded by the maintainers into the broader mouse-bindings request #1553, also open. Neither has a PR.",
  },

  "integration-previews": {
    problem:
      "A link in terminal output is a URL and nothing more. To find out whether the pull request a build printed has merged, or what <code>ACME-8209</code> is about, you open a browser, wait, read and come back — for every link, every time. A bare identifier is worse: an issue key in a commit message is not a link at all.",
    how:
      "Each integration is an <code>integration.json</code> manifest: matchers that claim a link or a run of text (ICU patterns whose named groups become template variables), the settings and credentials it needs, fetch steps — HTTP, or a local command whose output is JSON — chained through <code>{{step:/json/pointer}}</code> templates, and the fields, field groups, tabs and actions the card draws. GitHub, Jira, Slack, Stith, shefrd and Unblocked Code ship built in, and a manifest under <code>%LOCALAPPDATA%\\Microsoft\\Windows Terminal\\Integrations</code> with the same id replaces a built-in. The card shows a loading state, then the fields; an integration missing a required setting says <em>not configured</em> and contacts nothing, and a failed fetch shows its error inline without taking the card down. A result is cached per match for the manifest's <code>cacheSeconds</code> — 300 for GitHub, Jira and Slack, 30 for Stith — and no tab is built until it is opened; Jira's comments are not even requested until then. Markdown and Jira's ADF render as formatted text, tables and code blocks included. Credentials live in the Windows credential vault under <code>WindowsTerminal/Integrations/&lt;id&gt;</code>, and a link matcher can require the hovered host to equal a configured setting, so a look-alike link never starts a fetch and never sees a token. GitHub runs <code>gh auth token</code> first and only falls back to a stored token, so a signed-in CLI needs no setup. A card can also act: Jira's offers the issue's transitions, Apply changes the status badge in place, and Undo appears only when the workflow has a way back.",
    steps: [
      "Open Settings → Integrations and open <code>GitHub</code>. With the GitHub CLI signed in there is nothing to fill in; the page shows which account it connected as, with a <code>Refresh</code> button.",
      "In a pane, run <code>echo https://github.com/microsoft/terminal/issues/8849</code> and hover the link. The card shows a loading state, then the issue's title, state and details, with its description behind a tab. A Comments tab is one tick away under <code>Show as tabs</code>.",
      "Open <code>Jira</code> on the Integrations page and fill in <code>Site host</code>, <code>Account email</code> and <code>API token</code> (id.atlassian.com → Security → API tokens). Until all three are set it says <em>Not configured</em> and is never contacted.",
      "Under <code>Detect in output</code>, press <code>Add as rule</code> beside the issue-key pattern. Print a key from your site and hover it: the ticket's summary, status, type and assignee, with its parent in a breadcrumb above the title.",
      "Pick a transition in the card's <code>Change status</code> box and press <code>Apply</code>. The badge changes without a second hover.",
      "Back on the integration's page, clear boxes under <code>Show in tooltip</code> to trim the card. Only what you changed is stored.",
    ],
    settings: [
      { key: "integrations", def: "{}", note: "Per integration id: <code>enabled</code>, <code>settings</code>, and <code>fields</code> / <code>tabs</code> as overrides — a key not mentioned takes the manifest's default. Never credentials." },
      { key: "hyperlink.integrationDisplayMode", def: "\"above\"", note: "Where the integration's logo sits on the card: <code>none</code>, <code>above</code> or <code>left</code>." },
    ],
    media: [
      { kind: "compare", aligned: true, before: "integration-previews-github-upstream", after: "integration-previews-github", caption: "Hovering a GitHub issue link: upstream's tooltip is the bare URL; Mullion's card shows the issue’s title, state, author and comment count, with its description and comments behind tabs." },
      { base: "integration-previews-settings", caption: "Part of an integration’s page: credentials kept in Windows Credential Manager, never in settings.json, and which fields the card shows." },
    ],
    notes: [
      "Field choices are stored as overrides, not as the list of fields to show. A list could not tell \"I turned that off\" from \"that did not exist when I chose\", so every field a manifest gained later stayed hidden from anyone who had ever ticked a box. The older list form is still read.",
      "The credential vault is per Windows user, so every Terminal built from this repo sees the same stored credentials. That is deliberate.",
      "A bare <code>repo#number</code> names no owner. GitHub tries owners you picked before, then pinned organizations, then every owner your account can reach, then repository search; with <code>Assume the most-starred repository</code> on (the default) the card says which owner it assumed and lets you change it.",
    ],
  },

  "integration-pages": {
    media: [
      { kind: "compare", before: "integration-pages-card-upstream", after: "integration-pages-card", caption: "Hovering a Stith session link: a bare URL tooltip upstream; Stith's own session page inside the card in Mullion, after Show Stith page." },
    ],
    problem:
      "Some services already have a better view of a thing than any card could rebuild. Stith's session view has tabs, grouped sections and live state; the card had a dozen flat fields, and the real page meant leaving the terminal for a browser.",
    how:
      "A manifest's <code>embed</code> key is a URL template, expanded with the same values as a matcher's link; Stith's is <code>{{settings.baseUrl}}/embed/s/{{id}}?host=mullion</code>, its chromeless session view. When it is set, the card and the link preview pane show <em>Show Stith page</em>, and the page is loaded with <code>theme=dark</code> or <code>theme=light</code> appended to match the app. It is hosted by a WebView2 composition controller whose visual is a child of a XAML element, so it clips, scrolls and layers with the card; the first version parented a WebView2 window to the Terminal's top-level window, where the page loaded and was never drawn. Navigation within the page's origin is allowed, so its own tabs work; any other origin, and anything that asks for a new window, is cancelled and opened through the terminal's normal link handling. A certificate error is accepted only for the page's own origin. In the card the page grows the card to the pane and keeps it up while you read; <em>Esc</em>, a click on the terminal, <em>Hide Stith page</em>, a pane resize or switching tabs puts it away. In the pane the button swaps the summary and tab strip for the page and stays on, so each link shown there opens on its page.",
    steps: [
      "Open Settings → Integrations → <code>Stith</code> and check <code>Server</code> points at a running stith dashboard (it defaults to <code>https://stith.lvh.me</code>).",
      "In a pane, print a session link — <code>echo stith://session/&lt;id&gt;</code> with a real session id — and hover it. A bare <code>stith://</code> link is hoverable without OSC 8.",
      "Press <code>Show Stith page</code>. The card grows and shows the session's own page, tabs and all, and stays open while the pointer is inside it.",
      "Press <em>Esc</em> to put it away. Then press <code>Show in pane</code>, and in the pane press <code>Show Stith page</code>: every session link shown there now opens straight onto its page.",
    ],
    notes: [
      "Of the built-in integrations only Stith declares a page. A user manifest adds one with a single <code>embed</code> key.",
      "It needs the WebView2 runtime, which Windows 11 includes. The older <code>html</code> manifest key, an inline document rendered in place of the fields, still uses the window-hosted approach and so still does not show.",
    ],
  },

  "file-previews": {
    problem:
      "A path in output was something to click and hand to whatever app Windows associates with the extension. Checking what a log, a config file, a screenshot or a generated report actually contains meant leaving the terminal to open it, even when one glance would have answered the question.",
    how:
      "A file link previews by itself, independent of the rules, and leaves the card's <em>No rule matched</em> line alone. The footer names the file type with its icon and a readable size. Text is decoded as UTF-8 or BOM-marked UTF-16; source files get syntax colours, and Markdown has <em>Formatted</em> and <em>Raw</em> buttons, with YAML frontmatter shown as metadata. PNG, JPEG, GIF, BMP and TIFF show their first frame, PDFs render through Windows' own PDF API, and DOCX, XLSX and PPTX are unpacked and read in-process — paragraphs and tables, cells with their cached formula values, slide text — with no Office, no macros, no recalculation and no external relationships followed. The card shows up to 200 lines; <em>Show in pane</em> adds selectable text with line numbers, zoom for images and PDF pages with <em>Fit</em>, 50%, 100% and 200%, a PDF page picker, and a sheet or slide selector. Limits are labelled when they are hit: 50 MiB of input, 1 MiB of extracted text, 32 MiB of Office XML, 16 megapixels per image, 200 rows by 50 columns per sheet, 10,000 PDF pages. Legacy Office formats and other binaries show the type and size with an explanation instead. Each hover and each pane Refresh rereads the file; nothing is cached on disk.",
    steps: [
      "In a PowerShell pane, run <code>echo C:\\Windows\\System32\\drivers\\etc\\hosts</code> and hover the path. The card shows the file's first lines, its type and its size.",
      "Print the path of a Markdown file and hover it. Switch between <code>Formatted</code> and <code>Raw</code>.",
      "Hover the path of a PDF or an image and press <code>Show in pane</code>. Use <code>Fit</code> and the zoom buttons, and the page picker for a PDF.",
      "In a WSL pane, run <code>realpath ~/.bashrc</code> and hover the result. The file inside the distro previews the same way.",
      "Hover a folder's path, such as <code>C:\\Windows\\Web</code>. The card counts its folders and files and lists them, folders first.",
    ],
    media: [
      { kind: "compare", aligned: true, before: "file-previews-markdown-upstream", after: "file-previews-markdown", caption: "A Markdown file's path under the pointer: upstream does not detect the path, so nothing appears; Mullion renders the file, with its frontmatter as metadata and the Formatted / Raw switch." },
      { base: "file-previews-pane", caption: "A PDF in the pane: page picker, Fit and fixed zoom levels." },
    ],
    notes: [
      "A rule's own preview switches still apply: a rule with <code>Show preview</code> off, or naming an integration, decides for the files it matches.",
      "A bare POSIX path printed in a non-WSL pane is looked for in each registered distribution, off the UI thread, and accepted only when exactly one has it. Otherwise the card says it was not found, or that it exists in several and the path should name the distribution.",
      "The presets menu has Text files, PDF files and Office documents rules for when a file type should behave differently from the defaults.",
    ],
  },

  "preview-pane": {
    problem:
      "A hover card is the wrong place for a long Jira description or a comment thread: it is small, it goes when the pointer leaves, and reading three links in a row means three cards opening and closing over your output. Once previews contained links of their own, three cards could also pile up on screen at once, overlapping, with nothing able to close them until the pointer left the terminal.",
    how:
      "<em>Show in pane</em> on the card opens a pane holding the same preview — header, tabs, field groups, actions — and pressing it on a second link retargets that pane rather than splitting again. <em>Pane only — follow hovered links here</em> makes the pane follow: each link the pointer crosses replaces what it shows, and hover cards are suppressed in that tab until the pane closes. <em>Pin link</em> is separate: with a link pinned, hovering another shows it for as long as you are on it, and moving off restores the pinned one with the tab it was on; without a pin the last hovered link stays. Choosing <em>Show in pane</em> explicitly clears the pin. Links inside a preview's text are hoverable through the same rules and integrations as links in the buffer — always in the pane, and inside a hover card only with <code>hyperlink.tooltipNested</code> on — to at most four levels. Open previews are tracked in one registry rather than a counter, so opening one at a given depth closes every other at that depth or deeper, and closing one closes everything below it. In <em>Fit</em> mode an image or PDF page sizes to the pane's visible height, not just its width.",
    steps: [
      "Hover a link that has a preview — <code>echo https://github.com/microsoft/terminal/issues/8849</code> — and press <code>Show in pane</code>.",
      "Turn on <code>Pane only — follow hovered links here</code> at the bottom of the pane. Move the pointer across other links: the pane follows, and no cards open.",
      "Press <code>Pin link</code>, hover a different link, then move away. The pinned one comes back.",
      "Hover a link inside the description in the pane: it gets a card of its own. To get the same inside hover cards, turn on <code>Link tooltips inside popovers</code> on the Link Tooltip page.",
    ],
    settings: [
      { key: "hyperlink.previewInPane", def: "false", note: "Send a hovered link's details to a pane instead of a card. A rule can override it with <code>showInPane</code>." },
      { key: "hyperlink.tooltipNested", def: "false", note: "Preview links found inside a hover card's own content. In the pane they always are." },
    ],
    media: [
      { base: "preview-pane-follow", caption: "A link’s details in a pane beside the output it came from, with Pin link and the follow switch at the bottom." },
    ],
    notes: [
      "The pane has a <code>Refresh</code> button, which refetches and, for a local file, rereads it.",
    ],
  },

  "path-smarts": {
    problem:
      "Four ways a correct path came out wrong. A bare POSIX path — how most tools print one — went through a URI parser that throws for anything without a scheme, so the card said <em>Invalid URI</em> for a real, openable file. <code>~/.claude/plans/x.md</code> was detected from its first slash, as <code>/.claude/plans/x.md</code>, which does not exist. A path ending a sentence, <code>…/x.md.</code>, was classified by the extension <code>.md.</code> and previewed as a generic file. And dragging <code>Z:\\home\\me\\a.png</code> from a drive mapped to <code>\\\\wsl.localhost\\Ubuntu</code> onto a WSL pane pasted <code>/mnt/z/home/me/a.png</code>.",
    how:
      "A bare absolute POSIX path skips URI parsing and goes through the same resolution as a <code>file://</code> link, so its <code>\\\\wsl.localhost\\&lt;distro&gt;\\…</code> form comes from one place. The detection pattern keeps a leading <code>~</code>, and <code>~/</code> resolves to the source distribution's home, or to the Windows profile directory in a pane that is not WSL; with no known home there is no candidate, never a guess. A WSL home is the distribution's default user looked up in its <code>/etc/passwd</code> over <code>\\\\wsl.localhost</code>, with no shell started, cached per distribution and warmed off the UI thread when a WSL pane's settings apply. A detected path never ends on <code>.</code>, <code>:</code>, <code>!</code> or <code>?</code>; inside a path they still match. For a file dropped onto a profile whose <code>pathTranslationStyle</code> is <code>wsl</code>, the drive letter is looked up with <code>WNetGetConnectionW</code> first, and one mapped to <code>\\\\wsl.localhost\\&lt;distro&gt;</code> or <code>\\\\wsl$\\&lt;distro&gt;</code> becomes the distro's own path. Every other drive, other network drives included, still becomes <code>/mnt/&lt;letter&gt;</code>.",
    steps: [
      "In a WSL pane, run <code>echo /etc/hosts</code> and hover it. The card shows the <code>\\\\wsl.localhost\\&lt;distro&gt;\\etc\\hosts</code> path it will open, and a click opens it.",
      "Run <code>echo 'Read ~/.bashrc.'</code> and hover the path. It resolves to <code>.bashrc</code> in your home in that distro, and the full stop is not part of it.",
      "Map a drive to the distro (Explorer → This PC → Map network drive, folder <code>\\\\wsl.localhost\\Ubuntu</code>, say as <code>Z:</code>).",
      "Drag a file from <code>Z:\\etc</code> onto the WSL pane. It pastes as <code>/etc/…</code>, not <code>/mnt/z/etc/…</code>.",
    ],
    settings: [
      { key: "profiles.list[].pathTranslationStyle", def: "\"none\"", note: "How dropped paths are rewritten: <code>none</code>, <code>wsl</code>, <code>cygwin</code>, <code>msys2</code> or <code>mingw</code>. Generated WSL profiles get <code>wsl</code>." },
    ],
    media: [
      { kind: "compare", aligned: true, before: "path-smarts-tilde-upstream", after: "path-smarts-tilde", caption: "A <code>~/</code> path at the end of a sentence in a WSL pane: upstream does not detect it; Mullion resolves it to the distro user’s home, leaves the full stop out, and previews the file." },
    ],
    notes: [
      "A <code>~/</code> path hovered before that distribution's home is cached shows nothing yet rather than a guessed path. Opening the link, and the card's buttons, look the home up on a background thread, so they work even then.",
    ],
    upstream:
      "Translating dropped paths for WSL is upstream's (microsoft/terminal#11625); the mapped-drive case is not in any upstream issue. Clickable file paths in general are requested in #2671, still open.",
  },

  "tab-edges": {
    problem:
      "In a horizontal strip every tab gets a slice of the window's width. Once there are more than a handful, a tab named after what it is running — a branch, a host, a dev server — is down to an icon and three letters. There was nowhere else to put the tabs.",
    how:
      "<code>tabPosition</code> is a window setting with four values. Top is the shipped layout and the only one that draws the tabs into the title bar. The other three leave the window frame alone and put the strip in the page body; the title bar then shows the active tab's title instead, and for left and right it also takes the elevation shield and workspace button, which would otherwise stack at the top of a 200 px column (both only while <code>showTabsInTitlebar</code> is on). A left or right strip is a column of its own, 200 px wide to start, and the splitter on its inner edge drags it between 100 and 400 px. MUX's TabView keeps its stock template. An earlier version swapped in a rotated copy, and that emptied the strip every time the orientation changed: the tabs are live elements, and they never arrive in the new list a fresh template builds. So now the items panel the list has already built is turned vertical, and the strip's footer moves into a sidebar grid so it doesn't take the column's width. Drag-to-reorder, tear-out, keyboard cycling and the automation tree all keep working. The layout is reset and re-applied on every settings reload, which is how the dropdown and the <code>toggleVerticalTabs</code> action move the strip in a running window. <code>tabWidthMode</code> can't follow into a column, because Equal and Compact divide the strip's width inside compiled MUX code, so a vertical strip is pinned to size-to-content and that dropdown is greyed out.",
    steps: [
      "Open Settings → Personalization, expand <code>Tab strip</code> under Tabs and layout, and set <code>Tab position</code> to Left. The window reflows as soon as you save.",
      "Open a few tabs. They stack down the left edge, and the title bar shows the active tab's title.",
      "Drag the column's right-hand edge to widen or narrow it.",
      "Set <code>New tab button position</code> to <code>Under the last tab</code> to keep the + right under the tabs instead of at the foot of the column. It is greyed out for Top and Bottom, where the button always sits beside the tabs.",
      "To flip between Top and Left without opening Settings, run <code>Toggle vertical tabs</code> from the command palette, or bind it to a key under Actions.",
    ],
    settings: [
      { key: "tabPosition", def: "\"top\"", note: "top, bottom, left or right. Only top draws the tabs into the title bar." },
      { key: "newTabButtonPosition", def: "\"bottom\"", note: "bottom or afterTabs. Read only for a left or right strip." },
      { key: "tabWidthMode", def: "\"equal\"", note: "Upstream's. Ignored, and greyed out in Settings, while the strip is vertical." },
    ],
    media: [
      { kind: "compare", before: "tab-edges-left-upstream", after: "tab-edges-left", caption: "The strip on the left: six tabs named after what they run, every title readable in full, and the active tab’s title up in the title bar." },
      { base: "tab-edges-settings", caption: "The Tab strip group under Personalization. With the tabs on the left, Tab width mode is greyed out, because a column has no width to divide." },
    ],
    notes: [
      "The sidebar width is not saved. Every new window starts at 200 px.",
      "<code>toggleVerticalTabs</code> has no default key and only changes the in-memory setting, like <code>toggleAlwaysOnTop</code>. The next settings reload puts back whatever settings.json says, so use the Settings UI to make a position stick.",
      "This is a window setting, not a theme one, even though it sits beside <code>useMica</code> and <code>frame</code> in spirit. Built-in themes are never written back to settings.json, so a value the Settings UI stored on one would work for the session and then disappear on the next reload.",
    ],
    upstream:
      "Requested as microsoft/terminal#835, open since May 2019. Upstream has a hack-week prototype, and #20240 is an open spec-only PR that puts the setting on <code>theme.window.tabPosition</code> and re-templates the TabView for left and right. Mullion follows the prototype's grid layout but differs on both of those points: the setting is a window setting, for the reason above, and the template is never replaced, which is what lets a running window change position without losing its tabs.",
  },

  "window-dock": {
    problem:
      "A terminal you keep against one side of the screen has to be put back by hand every time something moves it. Windows Terminal already had exactly this behaviour, but only as quake mode: always the top edge, always half the height, and only for the summoned window.",
    how:
      "Quake mode was already a dock: the top edge at 50% of the monitor's work area, moving refused, resizing allowed only from the bottom, all done by a few checks in the window layer. <code>dockWindow</code> and <code>dockSize</code> generalise those checks. The arithmetic is the same, with the edge and percentage taken from settings. Moving is refused (Alt+Space → Move included), and only the edge facing away from the dock can be dragged. A docked window that lands on another monitor (Win+Shift+Arrow) re-snaps to the same edge there. Left, right and bottom docks are sized against the work area plus the invisible resize border only, so they stop at the taskbar instead of standing a title bar's height on top of it. Quake mode still wins: a summoned quake window goes to the top at half height whatever these settings say, because summoning is a different gesture from docking. <code>dockSize</code> is clamped to 10–90%. Below that the window is barely there, and above it docking is an awkward way to say maximized. Changes apply to the open window on save.",
    steps: [
      "Open Settings → Personalization and, under Window behaviour, set <code>Dock the window</code> to Left.",
      "Expand it and set <code>Docked size</code>, a percentage of the screen between 10 and 90. Save, and the window snaps to the edge.",
      "Try dragging it by the title bar: it stays where it is. Drag its right edge: that one resizes.",
      "Set it back to Off for an ordinary window.",
    ],
    settings: [
      { key: "dockWindow", def: "\"off\"", note: "off, top, bottom, left or right." },
      { key: "dockSize", def: "50", note: "Percent of the monitor's work area the docked window covers, clamped to 10–90." },
    ],
    media: [
      { base: "window-dock-left", caption: "Docked to the left edge at 35% of the screen. The window runs full height and stops at the taskbar." },
      { base: "window-dock-settings", caption: "Dock the window, under Personalization, with the size slider inside it." },
    ],
    notes: [
      "Dragging the inner edge resizes the window, but it doesn't change <code>dockSize</code>. The setting is still the size the window docks at.",
    ],
    upstream:
      "No upstream issue asks for docking to any edge. The closest is microsoft/terminal#11162 (open), which asks for a quake window taller than half the screen. This doesn't give you one: quake mode deliberately keeps its own top-edge, half-height placement.",
  },

  "small-knobs": {
    problem:
      "Several small behaviours were fixed in code or reachable only by hand-editing a theme. The resize-pane keys always moved a divider 5%. Closing the last tab always closed the window. Nothing showed a tab's number, so Ctrl+Alt+7 meant counting. The tab icon and close button could only be hidden through <code>theme.tab</code>, which the Settings UI cannot write to a built-in theme. And the automatic close confirmation asked as soon as a second tab was open, even when the second tab was Settings.",
    how:
      "<code>paneResizeStep</code> replaces the hardcoded 5%, clamped to 1–50%, and a <code>resizePane</code> binding can carry its own <code>amount</code> that overrides it. <code>showTabIndex</code> draws each tab's position, counting from 1, ahead of its title and its status icons, so the numbers line up in a column down a vertical strip instead of shifting as bells and progress rings come and go. With <code>closeWindowOnLastTab</code> off, closing the last tab opens a new default-profile tab in its place. Tearing the only tab out into another window still closes the window it left, and closing the window itself still closes it. <code>tabIconStyle</code> and <code>tabCloseButton</code> are window settings that override the active theme's <code>tab.iconStyle</code> and <code>tab.showCloseButton</code> when set and defer to it when not. They are deliberately left out of defaults.json, so only you can set them. <code>warning.confirmOnClose</code> gains <code>moreThanTabs</code>, which asks only once more than <code>warning.confirmOnCloseTabThreshold</code> tabs are open. The Settings tab never counts, in that mode or in automatic, because closing it loses nothing. Closing one tab that holds several panes still asks, as it does in automatic.",
    steps: [
      "Settings → Personalization → Tab strip: turn on <code>Show tab numbers</code>. Every tab shows its position.",
      "Settings → Interaction → Window and layout behavior: set <code>Pane resize step</code> to 10. Split a pane and press Alt+Shift+Arrow: each press moves the divider 10%.",
      "Settings → Personalization → Window behaviour: turn off <code>Close the window after closing the last tab</code>, then close the last tab. A new one takes its place.",
      "Back on Interaction, set <code>Show a confirmation dialog before closing tabs or windows</code> to <code>More tabs than a threshold</code>, and choose a number in <code>Warn above this many tabs</code>.",
      "Under Personalization → Tab strip, <code>Profile icon on tabs</code> and <code>Close button on tabs</code> override what the theme says.",
    ],
    settings: [
      { key: "paneResizeStep", def: "5", note: "Percent of the pane per key press, 1–50. A resizePane binding's own amount wins." },
      { key: "showTabIndex", def: "false", note: "Number each tab by its position, from 1." },
      { key: "closeWindowOnLastTab", def: "true", note: "Off, the last tab is replaced by a new one instead of the window closing." },
      { key: "tabIconStyle", def: "—", note: "default, hidden or monochrome. Unset, the theme decides." },
      { key: "tabCloseButton", def: "—", note: "always, hover, activeOnly or never. Unset, the theme decides." },
      { key: "warning.confirmOnClose", def: "\"automatic\"", note: "never, automatic, always, or Mullion's moreThanTabs." },
      { key: "warning.confirmOnCloseTabThreshold", def: "3", note: "How many tabs moreThanTabs allows before it asks. The Settings tab is not counted." },
    ],
    media: [
      { kind: "compare", before: "small-knobs-tab-numbers-upstream", after: "small-knobs-tab-numbers", caption: "Tab numbers before each title, and before the bell a tab shows when its program rang." },
      { base: "small-knobs-settings", caption: "Interaction: the pane resize step, and the close confirmation set to warn only above a tab count." },
    ],
    notes: [
      "The icon and close-button dropdowns show the override, not the value in effect. On a built-in theme the two agree. A custom theme that sets them shows its own value in the window and the default in the dropdown until you change the control.",
      "Tab numbers keep counting past 9, but only the first nine positions have default key bindings.",
    ],
    upstream:
      "microsoft/terminal#17843 (open) asks for a configurable keyboard resize step, measured in rows and columns rather than a percentage. #6276 (open) asks for the window to stay open when its last tab closes. The tab icon and close button were implemented upstream (#8157, #3335) as theme properties with no Settings UI. <code>moreThanTabs</code> extends upstream's own <code>confirmOnClose</code> (#20055).",
  },

  "motion": {
    problem:
      "<code>disableAnimations</code> could only force animations off. There was no way to say \"whatever Windows says\": turning off animation effects in Windows, the switch a browser reports as <code>prefers-reduced-motion</code>, left the Terminal animating anyway. And there was no way to keep the Terminal's animations while Windows had them off.",
    how:
      "<code>motion</code> is <code>system</code>, <code>full</code> or <code>reduced</code>. Under <code>system</code> it reads the Windows animation effects setting, and an old <code>\"disableAnimations\": true</code> still forces animations off, so an existing settings.json behaves exactly as before. The other two values ignore both. The answer goes into XAML's own process-wide switch, <code>Timeline.AllowDependentAnimations</code>, rather than a variable of Mullion's, so the Settings UI (a different DLL) reads the same decision instead of keeping its own copy. Animations that switch doesn't reach are handled by hand: the tab list's content and reorder transitions are removed, the Settings page drill-in transition is dropped, and settings search skips the 333 ms it otherwise waits for an expander to finish opening before it scrolls to a result. The preference is applied when the window starts and on every settings reload.",
    steps: [
      "Open Settings → Personalization and find <code>Motion</code> under Visual style.",
      "Choose <code>Never animate</code> and save. Open, close and reorder tabs: they move without transitions, and pages in Settings switch without sliding in.",
      "Choose <code>Follow Windows</code>, then turn off Windows Settings → Accessibility → Visual effects → Animation effects. Save any setting or open a new window, and the Terminal stops animating too.",
      "Choose <code>Always animate</code> to keep the Terminal's tab and settings animations while the rest of Windows has them off.",
    ],
    settings: [
      { key: "motion", def: "\"system\"", note: "system, full or reduced." },
      { key: "disableAnimations", def: "false", note: "The older switch, now JSON-only. Under system, true still forces animations off; full and reduced ignore it." },
    ],
    media: [
      { kind: "compare", before: "motion-settings-upstream", after: "motion-settings", caption: "Settings → Personalization: upstream has only the Pane animations switch, which does not follow Windows' animation effects; Mullion's Motion offers Follow Windows, Always animate or Never animate." },
    ],
    notes: [
      "Pane split and close animations still check the Windows switch themselves. With animation effects off in Windows, <code>Always animate</code> does not bring those back.",
      "The old Pane animations toggle is gone from the Settings UI. <code>disableAnimations</code> is still read from settings.json.",
    ],
    upstream:
      "microsoft/terminal#8405 (open) reports that <code>disableAnimations</code> leaves other animations playing, which is the same gap from the other side.",
  },

  "scheme-contrast": {
    problem:
      "\"Automatically adjust lightness of indistinguishable text\" is a profile setting, but whether text needs it is mostly down to the palette. A tuned light scheme is made worse by it, and colours that were legible come out unreadable, while a low-contrast scheme needs it. A profile that uses one scheme in light mode and another in dark could only give one answer for both.",
    how:
      "A scheme may carry <code>adjustIndistinguishableColors</code>, with the same values as the profile setting. Precedence: a value set on the profile's own appearance wins, then the active scheme's, then <code>profiles.defaults</code> and the built-in default, <code>automatic</code>. So a scheme beats a default you set once for everything, but never a choice you made on one profile. The scheme that counts is the one in use right now, so with a light/dark pair the answer changes when Windows does. A scheme with no value writes no key, so existing schemes save byte for byte as before. In the Settings UI the choice is on the scheme's own page, below Set color scheme as default, and <code>Use the profile's setting</code> means the scheme has no opinion. It is greyed out on built-in schemes, which can't be edited.",
    steps: [
      "Open Settings → Profiles → Color schemes and pick a scheme you added. Built-in schemes are read-only, so duplicate one first if you need to.",
      "Set <code>Automatically adjust lightness of indistinguishable text</code> to <code>Never</code> and save.",
      "In a profile that uses the scheme, print text close to its background, e.g. <code>printf '\\e[34;44m blue on blue \\e[0m\\n'</code>. It stays exactly as the palette draws it.",
      "Set the scheme to <code>Always</code> instead. The same text is lightened or darkened until it can be read.",
      "Set the option on the profile itself, under Profiles → your profile → Appearance, and the profile's value wins over the scheme's.",
    ],
    settings: [
      { key: "schemes[].adjustIndistinguishableColors", def: "—", note: "never, indexed, always or automatic. Unset, the profile decides." },
      { key: "profiles.defaults.adjustIndistinguishableColors", def: "\"automatic\"", note: "Upstream's profile setting. A scheme's value now beats it; a value on the profile itself still beats the scheme." },
    ],
    media: [
      { base: "scheme-contrast-compare", caption: "The same foreground-on-background grid under one scheme, set to Never on the left and Always on the right." },
      { base: "scheme-contrast-setting", caption: "The setting on a colour scheme’s page, with Use the profile’s setting as the no-opinion choice." },
    ],
    notes: [
      "An unfocused appearance that sets nothing takes the scheme's value, even when the focused appearance set its own.",
    ],
    upstream:
      "microsoft/terminal#14940 and #15452 (both open) are about the adjustment misfiring, #15452 on a light scheme. Neither proposes letting the scheme decide.",
  },

  "profile-order": {
    problem:
      "The Settings UI had no way to reorder profiles. The standing advice was to reorder <code>profiles.list</code> by hand in settings.json. Since the dropdown got a layout of its own there have also been two orders: the dropdown shows profiles one way, while Ctrl+Shift+1–9 and <code>newTab</code>'s <code>index</code> still count <code>profiles.list</code>. So the shortcut labels down the menu read 4, 7, 3, 1, and which profile Ctrl+Shift+2 opens depends on the order the profiles happened to be created in. Moving a profile to the top of the Dropdown Menu page moves it in the menu and nowhere else.",
    how:
      "The dropdown is the authority, and <code>profiles.list</code> follows it. The dropdown is walked depth-first: a profile inside a folder is counted where the folder sits, a matcher contributes the profiles it matches in list order, and \"remaining profiles\" expands, in list order, to whatever no other entry claimed. A profile that appears twice counts where it first appears. The profiles the dropdown shows are then put back into the slots they already occupy in the list, so hidden and orphaned profiles never move. This runs when settings load, so the shortcuts number profiles the way the dropdown shows them even before anything is saved, and after every edit in the Settings UI, so moving an entry on the Dropdown Menu page reorders the Profiles page. It also works the other way. Dragging a row on the Profiles page, by the gripper in its left padding, becomes a dropdown edit: the dropdown's own profile entries are sorted to match, a profile dropped beside an explicit entry gets an entry of its own there, and an explicit one dropped among the remaining profiles loses its entry. A drop the dropdown can't express, such as a profile dropped into a matcher's group it doesn't match, snaps back to where the dropdown puts it, and the list shows that.",
    steps: [
      "Open Settings → Profiles. Under Terminal profiles, every row has a gripper at its left edge (tooltip: Drag to reorder).",
      "Drag a profile by its gripper to a new place and save.",
      "Open the new-tab dropdown. The profiles are in the order you set, and the Ctrl+Shift+N labels count straight down it.",
      "Open Settings → Dropdown Menu, move an entry or put a profile in a folder, then go back to Profiles: the list has reordered to match.",
      "Press Ctrl+Shift+2. It opens the second profile the dropdown shows.",
    ],
    media: [
      { kind: "compare", before: "profile-order-drag-upstream", after: "profile-order-drag", caption: "Terminal profiles on the Profiles page: upstream's rows have no handle to reorder them by; Mullion's rows carry a gripper, and here PowerShell is lifted mid-drag on its way down the list." },
      { base: "profile-order-dropdown", caption: "The new-tab dropdown. The Ctrl+Shift+N labels count 1, 2, 3 down the menu, in the order it shows." },
    ],
    notes: [
      "With the default dropdown, which is nothing but remaining profiles, the dropdown's order is the list's order, so a drag simply reorders <code>profiles.list</code>.",
      "Profiles inside folders are numbered in the order the menu walks them, folder contents included, so Ctrl+Shift+3 can open a profile you only see after opening a folder.",
      "Hidden profiles stay where they are and get no number. On the Profiles page they carry a badge: Hidden from the dropdown menu.",
      "Reordering is mouse-only for now. There is no keyboard way to move a row.",
    ],
    upstream:
      "This is microsoft/terminal#8914, \"Profile reordering for Settings UI\": open since January 2021, labelled Help Wanted and Area-SettingsUI, with 51 comments. The maintainers' workaround has been to reorder <code>profiles.list</code> in settings.json by hand. The issue's design notes floated dragging the profiles' NavigationView items, with a gripper icon so the dragging is discoverable, or an index box on each profile's page, and suggested deprecating <code>hidden</code> once reordering existed. Commenters asked for a keyboard way to move a profile, such as Alt+Up/Down. In December 2025 a maintainer called the Ctrl+Shift+# order not following the New Tab Menu \"probably the last thing we should fix before closing this issue out\", and asked how numbering should work across folders, matchers and remaining profiles. Against that list, Mullion has drag-to-reorder with a gripper, on the Profiles page's rows rather than in the navigation pane, where profiles no longer sit. It has no keyboard reordering. The order follows the new-tab dropdown in both directions, numbered depth-first through folders, with matchers and remaining profiles expanded in list order. And <code>hidden</code> stays: hidden profiles keep their slot and get no number. No upstream PR implements any of it.",
  },

  "focused-notifications": {
    problem:
      "A desktop notification (OSC 777, or a bell set to notify) is thrown away when the window has focus and the pane that sent it is the active one. For a shell that's right. For tmux, zellij or shefrd in that pane it isn't: they send notifications about their own panes, which the Terminal can't see, so the notifications are lost exactly while you are working in the multiplexer. A multiplexer's own \"send a test notification\" button looks broken every time, because the test is sent from the pane you're looking at.",
    how:
      "<code>compatibility.notifyWhenFocused</code> is a profile setting that sits next to <code>compatibility.allowOSC777</code>. When it is on, the focused-and-active check is skipped for that profile's panes, so a notification shows whether or not you're looking at the pane. It covers everything that goes through the Terminal's desktop-notification path, OSC 777 and the bell notification alike. It is off by default, which is upstream's behaviour.",
    steps: [
      "Open Settings → Profiles, pick the profile you run a multiplexer in, and go to Terminal Emulation.",
      "Turn on <code>Allow OSC 777 (Desktop Notification) to show toast notifications</code>, then <code>Show notifications from the pane you are looking at</code>, and save.",
      "With the window focused and that pane active, run <code>printf '\\e]777;notify;Build;Finished\\a'</code>. A notification appears, even though you're looking at the pane.",
      "Turn the switch off and run it again. Nothing appears, which is what upstream does.",
    ],
    settings: [
      { key: "compatibility.notifyWhenFocused", def: "false", note: "Show this profile's notifications even from the active pane of a focused window." },
      { key: "compatibility.allowOSC777", def: "false", note: "Upstream's: whether applications may send OSC 777 notifications at all." },
    ],
    media: [
      { kind: "compare", before: "focused-notifications-setting-upstream", after: "focused-notifications-setting", caption: "Profiles → Terminal Emulation: upstream's OSC 777 switch is followed by the ENQ response; Mullion adds Show notifications from the pane you are looking at right under it." },
    ],
    upstream:
      "No upstream issue or PR asks for this. Upstream's notification sources are OSC 777 (microsoft/terminal#7718, closed) and the bell notification (#20011, merged). Activity notifications are an open PR, #20014.",
  },

  "about-dialog": {
    problem:
      "The inherited dialog linked to Microsoft's source, docs, release notes and privacy policy. It ran a Store update check that can't apply to a slot (a dev build only ever reported a simulated update), and its Send feedback button went to Microsoft. None of it described the build you were actually running, and there was nowhere to see what had changed since the last one.",
    how:
      "Two tabs. About shows the version, the commit and branch, the build time and the executable path, with Copy build info and links to github.com/aylith-labs/mullion and mullion.aylith.com. Release notes lists the CI builds of main, newest first and the newest expanded. Each is titled by how long ago it was built, with the exact UTC time on hover, and shows its commit, run number and the commits it brought. The one you are running is marked <em>this build</em>. Mullion has no versions of its own, so a version here is a successful build of main, which is the only thing that ever reaches a slot. <code>tools/Write-ReleaseNotes.ps1</code> runs in CI and walks the workflow's earlier successful runs, up to 30. For each build it takes the first-parent commits since the previous one, so an upstream merge is one line rather than every commit it carried, and runs that rebuilt a commit already covered are folded in. The result is compiled into the binary, so a build made anywhere but CI has no notes and says so. The main button is now Report an issue, which opens a new issue on Mullion's tracker. In portable mode, which has no package version, the version line reads <code>source &lt;commit&gt;</code>.",
    steps: [
      "Open the new-tab dropdown and choose About, or run <code>Open about dialog</code> from the command palette.",
      "On the About tab, check the commit and build time, and use Copy build info to put the details on the clipboard.",
      "Switch to Release notes. The newest build is open; expand older ones to see what each brought. Hover a title for the exact time.",
      "Report an issue opens a new issue on github.com/aylith-labs/mullion.",
    ],
    media: [
      { base: "about-dialog-about", caption: "The About tab: the version, commit, branch, build time and executable path, with links to Mullion’s source and docs." },
      { base: "about-dialog-release-notes", caption: "Release notes: one entry per CI build of main, the running one marked this build, each listing the commits it brought." },
    ],
  },

  "settings-window": {
    problem:
      "Upstream opens Settings in exactly one place: a tab, beside your terminals. To change a setting you lose sight of the pane you were changing it for, the Settings tab sits in the tab row as if it were a shell, and getting rid of it again means finding and closing that tab. There is no way to put Settings over the window or beside it.",
    how:
      "<code>settingsUIHost</code> picks the host; the Settings page itself is the same in all three, and only the container differs. <code>\"dialog\"</code> centres it over the current window and dims the terminal behind it. The first time it opens at 85% of the window; grips on every edge and corner resize it about its centre, and the size you leave it at is kept in state.json (<code>settingsDialogWidth</code>/<code>settingsDialogHeight</code>), so it opens at that size next time instead of changing shape with each page. The page scrolls inside the box, and the Close button (which Esc presses) closes it. <code>\"window\"</code> opens an ordinary Terminal window whose only startup action is the Settings UI, so it comes up showing Settings and no shell, and resizes, minimises and appears in the taskbar like any other window. That window asks for a Settings <em>tab</em> rather than \"the Settings UI\" — the new <code>settingsUITab</code> target of <code>openSettings</code> — because otherwise it would read the same setting and ask for another window, and that one for another.",
    steps: [
      "Open Settings → Interaction and find <code>Open settings in</code>.",
      "Choose <code>A dialog over the window</code>. Close Settings, then press <code>Ctrl+,</code> in a pane: it comes up over the terminal instead of as a tab.",
      "Drag any edge or corner of the dialog to resize it, press Esc, and open it again — it comes back at the size you left it.",
      "Choose <code>Its own window</code> and press <code>Ctrl+,</code>: Settings opens as a separate window with its own taskbar button and no shell tab.",
    ],
    settings: [
      { key: "settingsUIHost", def: "\"tab\"", note: "tab, dialog, or window. Honoured by <code>openSettings</code> with target <code>settingsUI</code>, which is what <code>Ctrl+,</code> runs." },
    ],
    media: [
      { kind: "compare", before: "settings-window-dialog-upstream", after: "settings-window-dialog", caption: "Settings as a dialog over the window, with the terminal still there behind it." },
      { base: "settings-window-option", caption: "Open settings in, on the Interaction page, with its three choices." },
    ],
    notes: [
      "<code>{ \"action\": \"openSettings\", \"target\": \"settingsUITab\" }</code> always opens a tab in the current window whatever <code>settingsUIHost</code> says, for a binding that should keep the old behaviour.",
      "The dialog's size is view state, so it lives in state.json, not settings.json, and resizing it is never an unsaved change. Only the size is kept: the dialog is always centred on the window it opens over.",
    ],
    upstream:
      "Asked for upstream as microsoft/terminal#9863. The team kept Settings as a tab, citing VS Code, and pointed to tab tear-off as the way to get it into a window of its own.",
  },

  "settings-save": {
    problem:
      "In upstream's Settings UI, Save and Discard changes are always enabled, so they say nothing about whether anything is unsaved, and a dropdown changed three pages ago leaves no trace you can see. Close Settings with edits pending and they are gone without a word — an evening of rebinding keys included.",
    how:
      "Whether there is anything to save is decided by one thing: a fingerprint of exactly the bytes a save would write, compared with the fingerprint taken when the editor's copy of the settings was made. Change notifications only decide <em>when</em> to look (within 120 ms of a change), and a once-a-second sweep while the page is loaded catches edits that raise no notification. Hovering Save or Discard changes lists the differences one per line, such as <code>profiles › PowerShell › fontSize: 10 → 12</code>, up to 20 lines; the list comes from diffing the serialized settings against the clean copy, with profiles, schemes and actions matched by name, id or keys so an entry is named rather than numbered. The <code>Auto-save</code> switch left of Save writes every change as it is made and disables both buttons while it is on; the reload that write sets off is recognised as its own and does not rebuild the page under your cursor. With auto-save off, closing Settings with unsaved changes is refused once, with a line in the footer saying so; closing again discards them. That applies to the dialog's Close button and Esc, and to closing a Settings tab or pane.",
    steps: [
      "Open Settings. Save and Discard changes are greyed out.",
      "Change a dropdown, say Startup → Default profile. Both buttons come alive.",
      "Hover Save: the tooltip names the setting and its old and new value.",
      "Try to close the Settings tab. It stays, and the footer reads <em>You have unsaved changes. Save them, or close again to discard them.</em> Close again to discard.",
      "Turn on <code>Auto-save</code> beside Save. Both buttons go grey, and every change from now on is written to settings.json straight away.",
    ],
    media: [
      { kind: "compare", before: "settings-save-tooltip-upstream", after: "settings-save-tooltip", caption: "Hovering Save with a change pending: upstream's tooltip only says Save all unsaved settings; Mullion's lists exactly what a save would change, old value to new." },
      { base: "settings-save-close", caption: "Closing with unsaved changes is refused once, and the footer says why." },
    ],
    notes: [
      "The auto-save switch is editor chrome, kept in state.json as <code>settingsAutoSave</code> (off by default) rather than in settings.json — otherwise turning it on would itself count as an unsaved change.",
      "Switching pages does not mark pending edits as clean; the baseline is taken only when the editor's copy is genuinely new — on open, after Save, after Discard and after a reload.",
      "Changes are listed in the tooltip but not yet marked on the pages themselves.",
      "Every settings reload used to lay the window's tab strip out again, which takes the tab content out of the window and puts it back. A Settings tab came back scrolled to the top with nothing focused, so with auto-save on, ticking a box halfway down a page threw you back to its first row. The layout is now redone only when the tab position or <code>showTabsInTitlebar</code> changes.",
    ],
    upstream:
      "Disabling Save and Discard until something changes is microsoft/terminal#12424, still open. Warning before closing Settings with unsaved changes was asked for in #12750, closed as a duplicate of #10627, which is still open.",
  },

  "fork-marks": {
    problem:
      "Every row in the Settings UI is drawn the same way whether Microsoft shipped it or this fork added it. That hides the divergence at exactly the moment it matters — when you are deciding whether to change something, or looking it up in upstream's documentation and finding nothing there.",
    how:
      "Each card or group that exists only in Mullion is marked in the page markup; the list came from diffing the setting keys against upstream rather than from memory. <code>aylith.imprint</code> draws a small diamond in the accent colour beside the setting's name on every such row, and flipping it redraws the cards already on screen rather than waiting for the next page. A group carries the mark on its header, and the pages that are Mullion's from top to bottom — Session Restore, Link Tooltip and Integrations — get it once, on their navigation entry, instead of on every row. <code>aylith.imprintJsonOnly</code> draws the same diamond hollow, in the secondary text colour, on a setting upstream has but exposes nowhere in its own UI; on Personalization, <code>Profile icon on tabs</code> and <code>Close button on tabs</code> carry it. A row has at most one of the two, since a setting either exists upstream or it does not.",
    steps: [
      "Open Settings → Compatibility. <code>Mark features added by this fork</code> shows a ◆ beside its own name when it is on, because it is one of them.",
      "Turn it on. The diamond appears on every Mullion setting, and after Session Restore, Link Tooltip and Integrations in the navigation pane.",
      "Turn on <code>Mark settings upstream hides in JSON</code> too, then open Personalization and find the hollow ◇ beside <code>Profile icon on tabs</code>.",
      "Open Documentation → Provenance marks for what each mark means.",
    ],
    settings: [
      { key: "aylith.imprint", def: "false", note: "Draw ◆ beside settings that exist here but not in Windows Terminal." },
      { key: "aylith.imprintJsonOnly", def: "false", note: "Draw ◇ beside settings Windows Terminal has but only exposes in settings.json." },
    ],
    media: [
      { kind: "compare", before: "fork-marks-compat-upstream", after: "fork-marks-compat", caption: "The two switches on Compatibility, each carrying the fork’s own mark, with the same mark on the pages it added." },
      { base: "fork-marks-page", caption: "Personalization with both marks on: hollow ◇ on settings upstream hides in JSON, ◆ on the fork’s own." },
    ],
    notes: [
      "The marks are hidden from UI Automation, so a screen reader does not read a stray diamond after every name — which also means an automated check cannot count them; they have to be checked on screen.",
      "The same marks are what the Settings filter uses to decide what is Mullion's.",
    ],
  },

  "settings-accordions": {
    problem:
      "The pages this fork added were long runs of loose cards: Link Tooltip was thirteen of them, four of which were a master switch and the three settings it gates. And where upstream does group settings, pressing Save or Discard snapped every group shut, as did going to another page and back — the group you had opened to change something closed the instant you committed the change.",
    how:
      "A group puts its master control in the collapsed header and the settings it gates as rows inside; each row repeats the gating condition itself, so the header stays clickable when the master switch is off. Link Tooltip's own settings are now three groups — <code>Make links clickable</code>, <code>Timing and size</code> and <code>Show buttons on the link tooltip</code> — and a rule's editor folds its four overrides into one. Whether a group is open is recorded per group in state.json (<code>expandedSettingsGroups</code>), keyed on the group's element name and, where a page hosts the same settings twice, the name of what surrounds it: Profiles → Appearance holds two copies of the appearance settings, and by bare name opening Color scheme under one also opened it under the other. A group you never touched is not recorded at all, so it still starts open or closed as its page defines.",
    steps: [
      "Open Settings → Link Tooltip and expand <code>Timing and size</code>.",
      "Change a value inside it and press Save. The page rebuilds and the group is still open.",
      "Go to another page and come back: still open.",
      "Close the Terminal and start it again. Settings → Link Tooltip opens with that group expanded.",
    ],
    media: [
      { base: "settings-accordions-linktooltip", caption: "Link Tooltip as groups, one of them open, each master switch in its header." },
    ],
    notes: [
      "Expansion is view state, so it lives in state.json and is never an unsaved change.",
      "A group that carries the ◆ mark carries it on its header, so moving Mullion's settings into a group does not lose the mark.",
    ],
    upstream:
      "The expander control and the first grouped pages are upstream's (microsoft/terminal#20200, #20232). Remembering which groups are open is not.",
  },

  "settings-nav": {
    problem:
      "Upstream's Settings sidebar is one ungrouped list, and its search box lives in a slot that turns into an icon you have to click whenever the pane is collapsed. Most pages also print a description under nearly every row, and on some pages those run to four lines, burying the settings they describe.",
    how:
      "Pages sit under headings — STARTUP, BEHAVIOR, APPEARANCE, COMMANDS, PROFILES, ADVANCED and HELP — still as one list; with the pane collapsed the headings disappear but the gap between runs of icons stays, so the grouping still shows. The search box is a plain box: above the first heading while the pane is open, beside the breadcrumb while it is closed. Focusing or clicking it while it is empty offers the last ten settings you reached through it, newest first, and choosing one goes there exactly as a live result does. Descriptions are off by default and come back with the <code>Show descriptions</code> switch at the foot of the navigation pane. With them off, every row that has one gets a small circled help badge beside its name that shows the description on hover; the badge disappears while descriptions are drawn, so the same text never appears twice. With the pane collapsed, clicking that footer icon flips the switch, since the switch itself is not drawn there.",
    steps: [
      "Open Settings. The navigation is grouped under STARTUP, BEHAVIOR and the rest, with Documentation alone under HELP.",
      "Hover the small circle beside a setting's name to read its description.",
      "Turn on <code>Show descriptions</code> at the bottom of the navigation pane: the descriptions print under each row and the circles go away.",
      "Search for a setting, choose a result, then click into the empty search box again — that setting is first in the list it offers.",
      "Collapse the pane with the menu button. The search box moves beside the breadcrumb and stays typeable.",
    ],
    media: [
      { kind: "compare", before: "settings-nav-pane-upstream", after: "settings-nav-pane", caption: "The grouped navigation pane, with the search box above the first heading and Show descriptions at the foot." },
      { base: "settings-nav-help", caption: "Descriptions off: the help badge beside a setting shows its description on hover." },
    ],
    notes: [
      "Both the descriptions switch and the search history are kept in state.json (<code>settingsShowDescriptions</code>, <code>settingsSearchHistory</code>), so neither counts as an unsaved change.",
      "Results that lead to a particular profile, colour scheme or Dropdown Menu folder are not kept in the history, because no stored string can rebuild them.",
    ],
  },

  "settings-filter": {
    problem:
      "Mullion's settings are spread through upstream's pages, so answering \"what does this fork actually add?\" — or the reverse, \"what here would Windows Terminal's documentation cover?\" — meant reading every page and every row.",
    how:
      "The filter button offers <code>All settings</code>, <code>Windows Terminal</code>, <code>Mullion</code> and <code>Experimental</code>, and turns accent with the filter's name beside it while one is on. Rows are judged by the marks they already carry — the fork mark and the experimental tag, on the row or on the group it sits in — plus the five pages Mullion added outright: Activity, Documentation, Integrations, Link Tooltip and Session Restore. Under Mullion or Experimental, an upstream group holding one matching row stays, showing only that row; a section heading goes once everything under it is hidden; navigation entries with nothing left go too, along with any heading left empty. Under Windows Terminal, an experimental upstream setting still counts as upstream's. Profiles and Color schemes are lists of pages rather than settings and are left whole, and if the page on screen drops out of the list the window moves to the first page still in it. Search results and recent searches honour the filter. Rows are hidden before the page is laid out, so a hidden row leaves no gap.",
    steps: [
      "Open Settings. With the navigation pane open, the filter button sits beside the menu button at the top of the pane.",
      "Choose <code>Mullion</code>. The navigation shrinks to the pages that hold Mullion settings, and each page shows only those rows.",
      "Open Interaction: only Mullion's rows are left, inside whichever groups hold them.",
      "Type in the search box: only Mullion settings come up.",
      "Choose <code>Windows Terminal</code> for the reverse, or <code>All settings</code> to put everything back.",
    ],
    media: [
      { base: "settings-filter-menu", caption: "The filter’s four choices, beside the menu button." },
      { base: "settings-filter-mullion", caption: "Filtered to Mullion: the navigation and the page keep only what this fork added." },
    ],
    notes: [
      "The choice is kept in state.json (<code>settingsViewFilter</code>), not settings.json, so changing it is never an unsaved edit, and it survives a restart.",
      "With the pane collapsed to its icons, the filter button moves back beside the search box in the page header.",
    ],
  },

  "shortcut-search": {
    problem:
      "The Shortcuts page lists every action, in a list long enough that finding one means scrolling and reading. There was no way to ask what a key does either — which action is on <code>Ctrl+Shift+W</code>, or everything that uses Alt — short of reading every chord.",
    how:
      "The box above the list filters as you type, with the fuzzy matcher already in the project: space-separated terms must all match, each as an ordered, case-insensitive subsequence, and the best matches rank first. Matched characters are drawn bold. A leading <code>@</code> matches key chords instead of names, each chord on its own so a match cannot run across two bindings. Modifiers can be typed in any order and abbreviated: <code>@shaltctrl</code> is read as <code>sh alt ctrl</code>, so it finds <code>ctrl+alt+shift</code> bindings, and the key caps that matched are drawn bold. The list's height follows the window, so a tall window shows a full list rather than stopping at a fixed height.",
    steps: [
      "Open Settings → Shortcuts.",
      "Type <code>split</code> in the search box. Only the split-pane actions are left, with the matched letters in bold.",
      "Clear it and type <code>@ctrl shift</code>: every action bound to a chord with both modifiers, those caps in bold.",
      "Type <code>@shaltctrl</code> to find <code>ctrl+alt+shift</code> chords without spelling them in order.",
    ],
    media: [
      { kind: "compare", before: "shortcut-search-name-upstream", after: "shortcut-search-name", caption: "Filtering by name: the matched letters in bold." },
      { base: "shortcut-search-chord", caption: "An @ search by key chord, with the matching key caps in bold." },
    ],
    notes: [
      "The Windows key cap is drawn as an icon rather than text, so <code>@win</code> filters correctly but that one cap is not bolded.",
      "When nothing matches, the page says <em>No shortcuts match your search.</em> rather than showing an empty list.",
    ],
    upstream:
      "Searching the actions list by name is microsoft/terminal#11630, still open.",
  },

  "in-app-docs": {
    problem:
      "Every Learn more link in upstream's Settings UI leaves the app for a website. For this fork that is worse than inconvenient: two of them promised explanations of features upstream does not have, so the page they opened could not describe them, and the regular-expression link pointed at .NET's reference for patterns that are actually run through ICU. Most of what Mullion adds had no documentation anywhere.",
    how:
      "The same topics are on the web at <a href='docs/'>mullion.aylith.com/docs</a>, rendered from the very files the app packages, so the two never disagree. Documentation is the last page in the navigation, alone under HELP. It lists ten topics — Keyboard shortcuts, Regular expressions, Extensions, Tabs and the tab strip, Window behaviour, Link tooltips, Integrations, Session restore, Activity log and Provenance marks — each a markdown file packaged with the terminal under <code>Docs\\</code> and drawn by the same renderer as the markdown pane and the link tooltip. Opening one adds it to the breadcrumb, which is how you get back. Links from one topic to another stay in the app. The renderer got proper spacing on the way: gaps after paragraphs and around headings, numbered ordered lists, ruled-off top headings, striped tables, and a <code>---</code> that actually draws. If a topic's file is missing from the package, the page says so instead of coming up blank. The topics are in settings search too, so searching for one opens it.",
    steps: [
      "Open Settings and choose Documentation, under HELP at the bottom of the navigation.",
      "Open Link tooltips, then click the link to <em>regular expression</em> in its table — that topic opens inside Settings.",
      "Use the breadcrumb at the top to go back to the topic list.",
      "Go to Shortcuts and click <code>Learn more about shortcuts</code>: it opens the Keyboard shortcuts topic rather than a website. Extensions (<code>Learn more about extensions</code>) and the Dropdown Menu's profile matcher (<code>Learn more about regular expressions</code>) do the same.",
    ],
    media: [
      { base: "in-app-docs-list", caption: "The Documentation page and its topics." },
      { base: "in-app-docs-topic", caption: "A topic open, with the breadcrumb back to the list." },
    ],
    notes: [
      "The regular-expressions topic corrects what the old link implied: the profile matcher's patterns are ICU, not .NET, and they must match the whole value — <code>Ubuntu</code> does not match <code>Ubuntu 22.04</code>, <code>Ubuntu.*</code> does.",
    ],
  },

  "activity-log": {
    problem:
      "A console window appears and nothing can say where it came from. With the terminal set as the default console host, any console program anything starts opens as a tab, and once it has closed there is no record of what it was or who started it. Upstream's answer to mysterious default-terminal launches is ETW logging (#11537), which only helps if a trace session was already running before the window appeared — exactly the situation you are not in when something surprises you.",
    how:
      "Each spawn site appends one JSON line to <code>activity.jsonl</code> beside <code>settings.json</code>, so the Dev and Test slots keep separate logs. Recorded: profile launches, with the working directory actually handed to <code>CreateProcessW</code>; console programs handed over by default-terminal delegation, whose command line is read back from the client process and whose parent is named; the terminal's own hidden background helpers (the promotion helper and the CI-poll task reconcile, logged as going through <code>wscript.exe</code>, so a hidden launch that turns visible is a bug rather than a mystery); and every control pipe request. Nothing on a launch path touches disk: the line is formatted on the caller's thread, queued, and appended from a threadpool timer with one <code>WriteFile</code> per batch through <code>FILE_APPEND_DATA</code>, which the OS places at end-of-file atomically — several Terminal processes share one file with no lock. Past <code>activityLogMaxKilobytes</code> the file is renamed to <code>activity.1.jsonl</code> and a new one started; one previous file is kept. The reader tolerates fields an older line lacks and skips a torn last line. Settings → Activity has two tabs. <em>Events</em> has a filter box over program, command line, folder, reason and time at once; Event type, Program and Day lists built from what the log actually holds, each entry with its count; <em>Group repeated events</em> by identical commands, program, event type or day; and switches for command lines and absolute time. <em>Settings</em> has the recording switch, the size cap, and the file's path with <em>Show in Explorer</em>. The <em>Open activity pane</em> command shows the same records, newest first, in a pane beside whatever just surprised you.",
    steps: [
      "Leave <code>Record an activity log</code> on under Settings → Activity → Settings (it is on by default). The <em>Log file</em> row shows where it is written; <em>Show in Explorer</em> opens the folder with the file selected.",
      "Open a few tabs and a split pane, so there is something to read.",
      "Switch to the <em>Events</em> tab. Each launch is a row: time, event, context, command. Hover a row for the exact time, the image path, the folder and what started it.",
      "Set <em>Group repeated events</em> to <em>Identical commands</em>. Repeats collapse into one row with a count and when the first was seen.",
      "Type part of a program name into the filter box, or pick a program or a day from its list, to narrow it further.",
      "From the command palette, run <em>Open activity pane</em> for the same list in a pane.",
    ],
    settings: [
      { key: "activityLog", def: "true", note: "Record every process started and every console program handed over. Command lines are written whole, so an argument holding a token lands in the file." },
      { key: "activityLogMaxKilobytes", def: "4096", note: "Size at which <code>activity.jsonl</code> rotates to <code>activity.1.jsonl</code>. Values below 64 are treated as 64." },
    ],
    media: [
      { base: "activity-log-events", caption: "Settings → Activity, grouped by identical commands: repeats are one row with a count and when the first was seen." },
      { base: "activity-log-pane", caption: "The activity pane beside a shell." },
    ],
    notes: [
      "Nothing is written until settings have loaded, so a crash before then cannot leave a record either way.",
      "The on/off state is a marker file beside the log, not a variable in memory. The library that records is linked separately into each module, and the two records that matter most are raised in a different module from the one that reads settings — with a variable, it compiled, linked and recorded nothing.",
      "Not recorded, on purpose: the working directory of a handed-off console program (Windows does not expose it reliably), the WSL distro probes (started with <code>CREATE_NO_WINDOW</code>, so they cannot produce a window), and a scheduled task firing later, which no in-process hook can see.",
      "The pane reads the newest 4000 entries; the Settings page searches the newest 2000 and lists at most 200 rows, while a group keeps counting past that so its number stays true.",
    ],
    upstream:
      "Upstream added ETW logging for default-terminal handoffs (microsoft/terminal#11537), which is right for reproducing a known problem and keeps nothing unless a trace was already running. Its open \"logging\" issues (#9700, #14018) are about saving terminal output, a different feature.",
  },

  "pipe-activity": {
    problem:
      "The control pipe lets any program running as you type into a pane. Without a record, the only evidence that one did is text appearing in a shell, with nothing saying which program sent it or why. The pipe can't work it out on its own either: it only sees the process on its end, and for every client inside WSL that is a throwaway <code>powershell.exe</code> under <code>wslhost.exe</code>, which names nothing useful.",
    how:
      "Each request becomes a <code>controlPipe</code> event in the activity log: the client's image and pid, its parent, and its whole ancestry as Windows sees it (<code>wslhost.exe 20440 &lt; wsl.exe 1234</code>), stopping at a pid younger than its child rather than naming a stranger who reused it; the request on one line with control characters spelled out, so whether the Enter was sent is visible; and the outcome (<code>ok, 3 panes</code>, or the error it was answered). send-input text is kept in full up to 4096 characters. Since protocol version 2 every request, <code>ping</code> included, must carry non-empty <code>reason</code> and <code>caller</code> strings; one without is refused with <code>missing-reason</code> before anything is done, and the refusal is recorded too. The pipe thread only records and posts a message, so the client has its answer before any UI work happens. On the tab row, a badge appears in every window the first time any request arrives and stays for that window's life; its number counts requests since the list was last opened. The list shows the latest ten rows, newest first — op, pane, result, reason and caller, with the process chain as the row's tooltip — and folds identical requests (same op, pane and text, from the same program, caller and reason, answered the same way) into one row with a count and when the first came, so a client polling every few seconds is one line. It is as wide as its rows, up to the width of the monitor under the cursor. Its last row, <em>Show all in Activity</em>, opens Settings → Activity filtered to control pipe requests. The same work added <code>focus-pane</code>, the one op that does move focus: it selects the pane's tab, unzooms the tab if another pane is zoomed, focuses the pane and brings its window up.",
    steps: [
      "Leave <code>controlPipe</code> and <code>activityLog</code> on (both are by default).",
      "From PowerShell, connect to <code>\\\\.\\pipe\\wt-control-&lt;pid&gt;</code> and send <code>{\"op\":\"list-panes\",\"reason\":\"trying the pipe\",\"caller\":\"interactive PowerShell\"}</code> — the snippet under <em>Trying it</em> in <code>doc/control-pipe.md</code> does this.",
      "A badge with a count appears on the tab row. Click it: the request is listed with its reason, caller and client process. Hover the row for the process chain.",
      "Send the same request a few more times. They fold into one row with a count. <em>Group identical</em> at the bottom of the list turns that off; the button beside it switches between time ago and date and time.",
      "Send <code>{\"op\":\"ping\"}</code> with no reason or caller. It is answered <code>missing-reason</code> and listed as refused.",
      "Click <em>Show all in Activity</em> for the full record, already filtered to control pipe requests.",
    ],
    settings: [
      { key: "controlPipe", def: "true", note: "Serve the pipe at all. Off removes the endpoint, and with it everything here." },
      { key: "activityLog", def: "true", note: "Off stops pipe requests being written to the log. The tab-row badge still counts them." },
    ],
    media: [
      { kind: "compare", before: "pipe-activity-badge-upstream", after: "pipe-activity-badge", caption: "The tab-row badge after seven pipe requests nobody has looked at yet." },
      { base: "pipe-activity-flyout", caption: "The list: repeated requests folded into one counted row, a refused request with no reason, and the way through to the full record." },
    ],
    notes: [
      "Every window gets every request: the badge says the pipe is in use, not which window was touched. A window opened later has no badge until the next request.",
      "The text typed through <code>send-input</code> is never put into a trace, because session titles and command lines would end up in telemetry. It is written to the activity log, which is a file in your own package folder and exists so you can see what was typed into your shells and by whom.",
      "Grouping and the time format are per window, and reset when it closes.",
    ],
    upstream:
      "Upstream has no control pipe to watch: it considered the capability in #9368 and #20106 and passed on both. An open PR (#20461, <code>wtcli</code>) would let a program query a running Terminal.",
  },

  "no-windowless": {
    problem:
      "One process owns the app's single-instance identity, and every later launch hands its command line to that process and exits. If creating a window failed, the error was swallowed and the process carried on with no window at all — alive, idle, and healthy-looking in Task Manager. From then on nothing opened: no window, no error, no crash, nothing in the event log. On 2026-09-05 this kept both slots from opening for nearly seven hours.",
    how:
      "The app already quit when its last window closed, or when startup deliberately created none. It did not cover a window creation that <em>failed</em>, because that exception is swallowed further up the stack, and it did not cover a launch handed over from a second process that produced nothing. A failed creation is now logged rather than lost, and a window only counts once it exists. Both cases arm a watchdog modelled on upstream's 5-second ConPTY handoff timeout: if there is still no window five seconds later, the process quits, and the next launch gets a fresh one. The watchdog returns at once whenever any window exists, so a failure while other windows are open can never put a live session at risk. For processes still running a build without the fix, <code>tools\\Repair-TerminalSlots.ps1</code> finds them from outside. It counts every top-level window a process owns, hidden ones included, rather than trusting <code>MainWindowHandle</code>, which only sees visible windows and would call a Terminal minimised to the notification area a zombie; it gives a process 90 seconds to produce a window before judging it, and lists what each zombie is still holding so the decision to clear it is an informed one. The deploy script's boot check used to skip itself whenever any process was running under the payload — so once a zombie existed, every later deploy quietly stopped verifying. It now clears windowless processes first.",
    steps: [
      "If a slot stops opening entirely, run <code>tools\\Repair-TerminalSlots.ps1</code> before suspecting the build or the settings. It reports every process per slot — healthy, still starting, or <code>NO WINDOW</code> — and changes nothing.",
      "To clear a zombie in the Test slot: <code>tools\\Repair-TerminalSlots.ps1 -Slot Test -Force</code>.",
      "The Dev slot needs <code>-IncludeDev</code> on top of <code>-Force</code>, because a windowless Dev process can still hold a live shell. Check the <code>holds:</code> lines first.",
      "Launch the slot again. A build carrying the watchdog does this by itself: a process left with no window is gone five seconds later.",
    ],
    media: [
      { base: "no-windowless-repair", caption: "Repair-TerminalSlots.ps1 reporting the Test slot: windows counted per process, hidden ones included." },
    ],
    notes: [
      "The script re-reads each process's path immediately before stopping it, and refuses if the pid now belongs to something outside the slot's payload.",
      "Exit code 1 means zombies are still present, so it can gate a script.",
    ],
    upstream:
      "The watchdog is modelled on upstream's 5-second timeout for ConPTY handoff embedding (#20051); the case it covers here, a process left with no window after a failed creation, has no upstream counterpart.",
  },

  "rollback": {
    problem:
      "Promotion kept the outgoing payload only until the new registration was confirmed, then deleted it. That covers a promotion that fails halfway and nothing else — not the one that actually happens, a promotion that succeeds onto a build which then won't start. On 2026-09-06 that left the Dev slot unable to launch with no previous payload on disk, and the fetcher could only ever stage the newest green run, which was the build that had just broken.",
    how:
      "<code>Promote-DevSlot.ps1</code> now rotates the outgoing payload into <code>dev.rollback.1</code>, moving older ones up to <code>dev.rollback.2</code> and beyond, and keeps <code>-KeepRollbacks</code> of them — 2 by default, at most 20, and 0 deletes it as before. A payload is about 200 MB. A kept payload with no <code>build-info.json</code> gets one recording when it was promoted away. Rotation only runs once the new registration has been verified, and if it fails that is reported but never stops the relaunch. <code>Fetch-CIBuild.ps1</code> takes <code>-RunId</code> or <code>-Commit</code> and stages that successful run instead of the newest. <code>-Commit</code> takes any sha prefix, matched against the last 60 successful runs, and refuses an ambiguous one by listing the matches rather than guessing; an unknown sha or run id fails with a message naming it. Naming a run skips the \"already staged something newer\" check, because going back to an older build is the point.",
    steps: [
      "Find the last commit that worked: <code>gh run list --repo aylith-labs/mullion --workflow build.yml --status success</code>.",
      "Stage it: <code>pwsh tools\\Fetch-CIBuild.ps1 -Commit 5c5c73f</code>, or <code>-RunId &lt;id&gt;</code>. It lands in <code>C:\\TerminalSlots\\dev-staged-ci</code>, over anything newer already there.",
      "Close the Dev windows and run <code>C:\\TerminalSlots\\Promote-DevSlot.ps1 -Staged C:\\TerminalSlots\\dev-staged-ci -Marker C:\\TerminalSlots\\dev-pending-ci.json</code>. It waits until every Dev process has exited, and never closes one itself.",
      "The build you went back from is now <code>C:\\TerminalSlots\\dev.rollback.1</code>, and the one before it <code>dev.rollback.2</code>.",
    ],
    media: [
      { base: "rollback-slots", caption: "C:\\TerminalSlots after a few promotions: the live payload, and the builds before it kept beside it." },
      { base: "rollback-fetch", caption: "Fetch-CIBuild.ps1 -Commit with an unknown sha refuses, naming it, before anything is staged." },
    ],
    notes: [
      "The promote button in the Dev window will not offer a build pinned this way: it only offers a staged build newer than the one running, a rule that exists because a stale marker once installed an older build over a newer one. Going back is done with the helper by hand — which is also the only option when the Dev slot won't start at all.",
      "<code>Fetch-CIBuild.ps1</code> needs PowerShell 7: it declares <code>#requires -Version 7.0</code>, so <code>powershell.exe</code> (5.1) refuses to run it.",
      "Run the fetcher again without arguments afterwards, or let the poller do it, to stage the newest build again.",
    ],
  },

  "portable": {
    problem:
      "Running a build of this fork meant registering an unpacked package from a slot folder, which needs Developer Mode — fine on the machine it is developed on, a wall for anyone who just wants to try it. And a ZIP made by hand, outside CI, would be a second build of uncertain source that nothing had ever launched.",
    how:
      "The <em>Portable customer acquisition</em> workflow is started by hand with the id of a <em>Build</em> run. It refuses anything but a completed, successful run on <code>main</code> of this repository whose <em>Unit tests</em> step passed, checks out that exact commit, and downloads that run's own <code>dev-payload</code> artifact instead of building again; the artifact's marker must name the same commit and must not be dirty. From the Test-branded layout in it, upstream's <code>New-UnpackagedTerminalDistribution.ps1 -PortableMode</code> produces an unpackaged folder carrying the pinned Microsoft.UI.Xaml 2.8.4 runtime and a <code>.portable</code> marker, which turns on upstream's portable mode: settings load from a <code>settings</code> folder beside the exe rather than from your profile. Before anything is uploaded, the ZIP is extracted into a fresh folder on the runner and <code>mullion.exe</code> started with a command that writes a proof file; within 45 seconds it must have a window, a <code>settings\\settings.json</code>, and that file. The output is the ZIP, <code>SHA256SUMS</code>, a <code>PAYLOAD.json</code> with every file's size and hash, and a <code>PROVENANCE.json</code> naming the source commit, the run, the XAML package's hash and the three checks that passed. In the app, a portable copy is named <em>Mullion (Portable)</em>, About gives its version as <code>source &lt;commit&gt;</code> because there is no package version to show, and the bar at the bottom of Settings reads <em>Mullion is running in portable mode.</em>",
    steps: [
      "Download <code>mullion-windows-x64-portable.zip</code> from the newest <code>portable-…</code> release, and check it against that release's <code>SHA256SUMS</code>.",
      "Extract the whole archive into a folder you can write to. Keep the <code>.portable</code> file: it is what keeps settings in that folder.",
      "Run <code>mullion.exe</code>. Its settings are created under <code>settings\\</code> beside it.",
      "Open Settings: the bottom bar says it is running in portable mode. Open About: the version is the source commit it was built from.",
      "To cut a new one, run the <em>Portable customer acquisition</em> workflow with a green Build run's id. The verified ZIP and its proofs come back as the <code>portable-customer-verified</code> artifact.",
    ],
    media: [
      { base: "portable-settings", caption: "A portable copy’s Settings: the bottom bar says it is running in portable mode." },
      { base: "portable-about", caption: "About in a portable copy: no package version, so the version is the source commit." },
    ],
    notes: [
      "Windows 10 version 2004 or later. It is a portable ZIP, not a signed MSIX installer.",
      "The workflow's artifact is kept for 14 days. Publishing it as a release is a separate step.",
      "LICENSE, NOTICE.md and the Microsoft.UI.Xaml licence travel inside the ZIP.",
    ],
    upstream:
      "Portable mode is upstream's (microsoft/terminal#15051): a <code>.portable</code> file beside the exe moves settings into the install folder. Upstream also publishes unpackaged x64, arm64 and x86 ZIPs with each release. What is added here is the Mullion naming, the commit as the version, and building the ZIP only from a CI run that has already passed, then launching it before it ships.",
  },
};
