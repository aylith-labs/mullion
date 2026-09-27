# Mullion — aylith-labs/mullion

**Mullion** is this fork’s name (the upright bar between window panes). It lives at
`aylith-labs/mullion`, a standalone repo, not a GitHub fork, so nothing defaults a PR
towards Microsoft. The process is `mullion.exe` and the slot aliases are `muld` (Dev) and `mult`
(Test). Builds from before the rename ran as `WindowsTerminal.exe` behind `wtd`/`wtt`, so the
slot scripts match both process names. Package identities `WindowsTerminalDev` /
`WindowsTerminalTest`, the window class, and the `WindowsTerminal/Integrations/` credential keys keep
their old names on purpose: renaming them would orphan both slots’ `settings.json` folders, the
promote machinery keyed on them, and every saved integration token.
The old `steven-pribilinskiy/terminal` repo is archived.

This checkout is **my fork, and it evolves on its own terms.** It is not a staging area for
upstream contributions. Features land here because I want them in *my* Terminal, whether or not
microsoft/terminal would ever take them.

## ALWAYS tell me when a change requires restarting the Terminal window

**Whenever a change you make or propose needs the running Windows Terminal instance restarted to
take effect, say so explicitly, and let ME decide when to restart it.** Never restart, close, or
kill a running Terminal window to "make the change take effect" or to verify your own work.

This matters more here than in most repos, because **the app I'm developing is also the app I run
my sessions in.** A restart can kill live shells, running builds, and Claude Code sessions. Same
hard rule as Tabby and WSL in my global CLAUDE.md: build and install the change, then *tell me* a
restart is needed.

Report such work as **"built and installed, not yet verified — needs a Terminal restart"**, and
say which behaviour the restart would let us confirm. If verification genuinely requires a running
UI, use **the test build (`mult`) in its own window** — never the dev build, and never my session's
window. See "The two slots" below; that distinction is the single most important rule in this file.

### What actually needs a restart

| Change | Restart? | Why |
|---|---|---|
| `settings.json` | **No** | `AppLogic::_RegisterSettingsChange` (`src/cascadia/TerminalApp/AppLogic.cpp:303`) watches the settings *folder* for `FileName \| LastWriteTime` and calls `ReloadSettingsThrottled()` when `settings.json` changes; debounced 100 ms (`AppLogic.cpp:137`). It watches the folder, not the file, because editors write a temp file then rename it. |
| OS light/dark theme flip | **No** | `WindowEmperor.cpp:1261` reloads settings, guarded so it only fires on a real theme change (GH#15732). |
| Atlas `.hlsl` shaders | **No**, Debug only | `ATLAS_DEBUG_SHADER_HOT_RELOAD` (`src/renderer/atlas/Backend.h:31`). |
| **Any C++ change** | **Yes** | Compiled into the binaries, loaded at process start. |
| **Any XAML change** | **Yes** | XAML compiles to XBF and links into the binaries. There is no XAML Hot Reload here — Terminal is a Win32 app hosting WinUI 2 through XAML Islands. |

## HARD REQUIREMENT: never edit a tracked file in place with `sed -i`

`.gitattributes` here is `* -text`, so git stores bytes exactly as they are and normalises
nothing. Most sources in this repo are **CRLF**. GNU tools write **LF**. So a `sed -i` — or `tr`,
`awk > file`, any POSIX in-place rewrite — silently converts the entire file: every line shows as
changed, a small edit becomes an unreviewable diff, and it conflicts with everything upstream
later does to that file.

- **Use the Edit tool.** It preserves the file's existing endings.
- If an edit genuinely has to be scripted, round-trip through `latin1` so bytes survive:
  ```bash
  node -e "const f=process.argv[1],fs=require('fs');let s=fs.readFileSync(f,'latin1');fs.writeFileSync(f,s.replace(/\r\n/g,'\n').replace(/\n/g,'\r\n'),'latin1')" <path>
  ```
- **`grep` cannot be trusted to see CR under git-bash.** Count bytes instead, which is unambiguous:
  ```bash
  cr=$(tr -cd '\r' < "$f" | wc -c); lf=$(tr -cd '\n' < "$f" | wc -c)   # cr == lf means CRLF
  ```
- **Look at `git show --stat` after every commit.** A file-sized diff for a small edit is this bug.
  Not every file is CRLF (`TerminalPage.cpp` is LF upstream), so compare against what the file was,
  never against an assumption.

A `pre-commit` hook enforces this — it refuses a commit that flips a file's endings wholesale, and
prints the repair. It lives in the repo so it survives a reclone, but `core.hooksPath` is local
config, so **each fresh clone needs it enabled once**:

```powershell
git config core.hooksPath tools/githooks
```

`--no-verify` bypasses it, for the rare case where a flip is genuinely intended.

## HARD REQUIREMENT: read the upstream issues and PRs before implementing anything

**When I ask for a feature or a fix, search microsoft/terminal's open issues and pull requests for
it BEFORE writing any code.** Not after, not "if it looks non-trivial" — first, every time.

```bash
gh search issues --repo microsoft/terminal --state open "<keywords>" --limit 20
gh search prs    --repo microsoft/terminal --state open "<keywords>" --limit 20
gh pr list   --repo microsoft/terminal --search "<keywords>" --state all --limit 20   # incl. merged/closed
git log --oneline upstream/main -20 -- <the files you would touch>
```

Then tell me what you found before you start: whether an issue already describes it (quote the
number), whether someone has an open PR for it, whether it was tried and rejected, and how the
maintainers' comments should shape the design. If there's an existing issue, the work references
it; if there's an open PR, say so and let me decide whether to build on it, wait, or go our own way.

This is not bureaucracy — it is the cheapest way to avoid building something that already exists or
was already refused, and it still pays now that nothing goes back: what upstream has already done
arrives here on the next merge, so building it again means writing a conflict for myself.

Also check `git log upstream/main` for the area you're about to touch: upstream may have reworked
it since our last merge (per-window settings, #20328, is a live example in the settings editor).

## HARD REQUIREMENT: never open a pull request against microsoft/terminal

**This fork sends nothing upstream. Do not open, reopen, or prepare a PR against
microsoft/terminal — not for a bug fix, not for something they would obviously want, not "ready if
you want it later".** Do not cut a branch off `upstream/main` for that purpose, and do not ask me
whether this one is worth sending. It never is.

This is one-directional, not a break: upstream is still **merged in** for their bug and security
fixes (see "Fork policy"), and their issues and PRs are still **read** before implementing anything
(see above) — knowing a thing was already tried, already exists, or was rejected is worth as much
when you are the only consumer. What stops is anything that pushes work outward.

## HARD REQUIREMENT: no Claude attribution, anywhere

**Nothing produced here credits Claude, Claude Code, or any AI — in any artifact, ever.** Not in
commit messages or trailers, not in PR titles or bodies, not in issue or review comments, not in
code comments, not in documentation, not in branch names.

I take ownership of everything that goes out under my name. My global settings suppress it
(`attribution: { commit: "", pr: "" }` in `~/.claude/settings.json`); never reintroduce it by hand,
and never work around it.

## Fork policy

- **`main` is mine.** My own work commits straight to `main` and pushes to `origin`. No PRs, no
  feature-branch ceremony, per my global "personal projects" rule.
- **Upstream is merged in periodically, on demand** — I want their bug and security fixes, I just
  don't want to be governed by their roadmap or review queue. Merge with
  `git fetch upstream && git merge upstream/main`. Never rebase `main` onto upstream: my commits are
  already pushed, and rebasing rewrites published history.
- Conflicts are resolved **in favour of my behaviour**, not upstream's. If upstream reworks
  something I've customised, keep mine and note it in the merge commit.
- Don't "clean up" divergence from upstream. Divergence is the point.
- Nothing goes back — see "never open a pull request" above. Upstream is a source, not a destination.

### Remotes

```
origin    https://github.com/aylith-labs/mullion.git       (fetch + push)
upstream  https://github.com/microsoft/terminal.git             (fetch only)
```

`upstream`'s push URL is deliberately set to `DISABLED_no_pushes_to_microsoft` so a stray
`git push upstream` fails loudly instead of attempting to write to Microsoft's repo.

### No open upstream PRs, and no branches cut for one

There are none, and there will be none. So there is no reason to branch off `upstream/main` for
that purpose — a branch here is for my own work in progress and is cut from `main` like any other.

The no-Claude-attribution rule above applies to these branches too — and to the fork's own history.

## HARD REQUIREMENT: builds happen in CI, never on this machine

**Do not build this repo locally. No `msbuild`, no `Invoke-OpenConsoleBuild`, no
`Deploy-TerminalSlots.ps1` run by hand, no Visual Studio build.** Push the commit and let
`.github/workflows/build.yml` produce the payload.

The reason is disk, and it is not marginal: a warm local build tree here runs to **~40 GB** —
the bulk of it in the `obj\` directories under each project, the rest in `bin\`, `packages\` and
`AppPackages\`. Every full build re-earns that, and `NuGet.Config` pins `globalPackagesFolder` and
`repositoryPath` to `.\packages` relative to the config file, so **every worktree pays it again**;
nothing is shared with the main checkout. CI has a clean machine and a package cache keyed on the
lockfiles, and it costs this disk nothing.

If a local tree ever reappears, deleting these reclaims all of it and loses nothing tracked:

```powershell
Get-ChildItem . -Recurse -Directory -Filter obj | Remove-Item -Recurse -Force
Remove-Item .\bin, .\packages, .\src\cascadia\CascadiaPackage\AppPackages -Recurse -Force
```

### Getting a build

Push to `main`. `.github\workflows\build.yml` restores, builds, runs the unit test suites that
pass cleanly (`Types.Unit.Tests`, `Control.Unit.Tests` — `SettingsModel.Unit.Tests` is excluded;
its test host crashes on `main` regardless of what changed, and gating on an already-red suite
would just teach everyone to stop reading the colour), packages both slots, and uploads the
`dev-payload` artifact: `dev-staged`, `test-staged`, and `dev-pending.json`.

`tools\Fetch-CIBuild.ps1` downloads the newest successful run and stages both halves —
`dev-staged-ci` for `muld`, `test-staged-ci` for `mult` — plus `dev-pending-ci.json` /
`test-pending-ci.json` describing each. `tools\Install-CIBuildPoller.ps1` registers a Scheduled
Task (`Terminal CI build poller`) that runs it on a timer — default 15 minutes, configurable from
Settings → Compatibility → CI build poll interval — so staging happens without remembering to
fetch anything.

Staging is automatic; registering either slot is a deliberate step, never a side effect of
fetching. `muld` reads `dev-pending-ci.json` on its own and offers to promote (see "The two slots"
for how, and the narrow exception to it). `mult` is refreshed by running
`tools\Refresh-TestSlot.ps1` — on demand, not on the poller's timer, since silently swapping an
actively open test window's binaries out from under it would defeat the point of it staying put
while it's being used.

CI green means the build compiles, packages, and passes its two test suites — report exactly
that, and name the UI behaviour a `mult` run would still need to confirm, rather than implying more
than CI itself proved.

**Two agents pushing to `main` at once will starve each other of CI results.** `build.yml` sets
`concurrency: cancel-in-progress`, which is right for one person pushing repeatedly and actively
harmful for two sharing a branch when a build takes ~40 minutes: each push cancels the run in
flight, so the newest commit is always building and nothing older ever reports. One afternoon of
this produced six runs and a single verdict, and that verdict was on the oldest commit in the
stack — both of us had individually done the reasonable thing by pushing a small fix promptly.

If another session is working in this tree, agree who pushes and when. Batching is strictly
better here: one run over a stack of commits clears everyone's backlog at once, while a prompt
push of a one-line fix costs the whole queue its answer. Check for a live run before pushing:

```powershell
gh run list --repo aylith-labs/mullion --limit 3
```

Note that `gh` resolves to `microsoft/terminal` in this checkout unless `--repo` is given, since
`upstream` is a remote here — a bare `gh run list` reports on Microsoft's CI, not ours.

### The build tooling stays in the tree — CI needs it

`tools\Deploy-TerminalSlots.ps1` is not local-only scaffolding: `build.yml` calls it directly
(`-StageOnly`), which is exactly why the two paths cannot drift. `OpenConsole.slnx`, the `.vcxproj`
tree, `NuGet.Config`, `dep\nuget\packages.config` and `tools\OpenConsole.psm1` are all on CI's
critical path too, and `tools\Fetch-CIBuild.ps1` / `Install-CIBuildPoller.ps1` are how a CI build
reaches this machine at all. **Deleting any of them breaks CI, saves kilobytes, and — for the files
inherited from upstream — writes a permanent merge conflict.** Leave them alone.

One trap the CI move doesn't remove, because it is about the repo and not the machine:
`dep/nuget/nuget.exe` is 4.1.0 (2017) and predates `.slnx` (*"Invalid input 'OpenConsole.slnx'.
The file type was not recognized."*). That is why `build.yml` installs a current nuget with
`NuGet/setup-nuget` instead of using the bundled copy.

The local-build traps that no longer apply here — `MAX_PATH` on long worktree paths, the COMMIT
exhaustion that reports itself as `C3859: ... the paging file is too small`, and
`Invoke-OpenConsoleBuild` silently eating `-p:` switches — are recorded in
[`doc/troubleshooting.md`](doc/troubleshooting.md) rather than here, so they cost nothing to read
every session.

**Never run `git worktree prune` from WSL here, and do not believe `worktree list` when it says
`prunable`.** A worktree added on the Windows side records a Windows-style path in
`.git/worktrees/<name>/gitdir` (`C:/wt-tm/.git`), which does not resolve inside WSL — `/mnt/c/...`
does. WSL git therefore reports *"gitdir file points to non-existent location"* for a worktree that
is entirely live, and a prune deregisters it. The directory survives, so nothing looks broken until
the next time you want that worktree. Check with Windows git (`/mnt/c/Program Files/Git/cmd/git.exe
-C 'C:\<path>' status`) before acting on any prunable verdict.

### When something you built will not run

For a crash or a silent UI failure — a `0xc000027b` fail-fast, controls that vanish without a
crash, a template that will not render — use the **`terminal-fork-diagnostics`** skill
(`.claude/skills/terminal-fork-diagnostics/`) rather than reasoning about it. It carries the cdb
capture procedure, the HRESULT decoder, the UI Automation verification scripts, and the MUX/UWP
traps this repo has already paid for. Its first rule is the one worth repeating here: **a XAML
failure in this app almost never fails where you can see it**, so capture a first-chance stack
before forming a theory. Builds cost forty minutes; a capture costs four.

Read [`doc/troubleshooting.md`](doc/troubleshooting.md) **before** you start bisecting or reading
code. It covers the slot-specific failures and, more usefully, the tests that produce confident
wrong answers: a payload exe launched directly has no package identity and aborts with
`REGDB_E_CLASSNOTREG` however healthy the build is; an "incremental rebuild" that never relinked
the exe has tested nothing; a pipeline's `$LASTEXITCODE` belongs to the last command, not to
msbuild.

The headline rule: **after changing a `.idl` or a `.xaml`, do not trust an incremental build.**
Stale generated interfaces across DLLs msbuild thinks are up to date abort inside
`AppHost::Initialize` before any window appears. Build the whole solution, or verify it starts.

**"A slot stopped opening entirely" is usually a windowless process, not a bad build.** A
`mullion.exe` with no window still owns its package's single-instance identity, so every
later launch hands off to it and exits silently — nothing opens, and nothing says why. Run
`tools\Repair-TerminalSlots.ps1` *first*, before suspecting the payload or the settings. Never
judge this by `MainWindowHandle`: it misses hidden windows and calls a tray-minimised Terminal a
zombie. Any script that launches a Terminal must confirm it reached a window and clean up if it
did not — a launch left unverified is how one of these is created.

## The two slots: `mult` is yours to break, `muld` is production

I run two Terminals built from this repo side by side. **They are not two equivalent scratch
installs.**

| Slot | Alias | Payload | What it is |
|---|---|---|---|
| **Test** | `mult.exe` | `C:\TerminalSlots\test` | **The only slot you may register, launch, restart or replace.** Disposable, no confirmation needed. `tools\Refresh-TestSlot.ps1` registers it from whatever `Fetch-CIBuild.ps1` last staged in `test-staged-ci` — run on demand, not on a timer, so it never swaps out from under an active verification session. |
| **Dev** | `muld.exe` | `C:\TerminalSlots\dev` (staged) | **Production. It hosts my live agent sessions, including Claude Code sessions.** You never build into it, never register it, never launch it, never restart it, and never write to any directory it is registered from. |

The Dev slot changes exactly one way: **I press promote**, in `muld` itself. Promotion is my gesture.
It is never a side effect of your build, and there is no situation in which you perform it for me.

**Exception:** you may run the promote helper (`tools\Promote-DevSlot.ps1`, deployed alongside the
payload at `C:\TerminalSlots\Promote-DevSlot.ps1`) yourself, without asking first, when you've
verified via process-tree inspection — not by assuming an empty slot is a safe slot — that either
**(a)** no `mullion.exe` is currently running under the Dev payload at all, or **(b)**
exactly one is running, with exactly one tab/pane, whose foreground process resolves to a session
multiplexer (`shefrd`, `herdr`, `tmux`, `screen`, `zellij`) rather than a raw shell — because in that
case the real work lives in the multiplexer's own persistent server, not the window, and closing it
loses nothing. Multiple windows, multiple tabs/panes, or a bare shell/REPL/build still require asking
first, unchanged; an ambiguous process tree counts as "still require asking," not as case (b). State
what you found and why you judged it safe every time you use this — a past verified case is not a
standing green light, and I might be sitting right in front of the window in question, in which case
just staging the build and letting me promote it myself is simpler than any of this.
`tools\Test-DevSlotIdle.ps1` implements this check — read-only, fails closed on anything ambiguous.

### Single-instance identity

`mult` and `muld` have distinct single-instance identities (window class and mutex mix in the package
family name — `WindowEmperor::HandleCommandlineArgs`), so `mult` can be started and verified while
`muld` is running. A boot check only has to decline when a process is already running under *its
own* payload, not "any `WindowsTerminal` process" — guarding on that would skip verification
whenever a Dev window is open, which is nearly always. The two packaging passes produce
byte-identical binaries; re-check with the one-liner in `Deploy-TerminalSlots.ps1`'s header
whenever you touch anything branding-conditional.

Two accepted limits, not bugs: `Package-Test.appxmanifest` declares fewer COM registrations than
`Package-Dev.appxmanifest`, so no defterm handoff or Explorer verb in the Test slot — byte-identical
binaries can't have distinct ones. And `WinRTUtils/inc/WtExeUtils.h`'s `IsDevBuild()` matches only
`WindowsTerminalDev`, so in Test `GetWtExePath()` builds a path to a `wt.exe` that doesn't exist.

Diagnosing a launch that silently ran the wrong build: never trust "it succeeded" — check
`(Get-Process WindowsTerminal).Path`; a window under the *Dev* executable path is the tell.
`Microsoft-Windows-AppModel-Runtime/Admin` shows the container being created and destroyed in the
same second.

### "There's no dev build running" is NOT permission

The Dev slot is production whether or not a window is open at that instant. An empty slot is not a
free slot — it is the case where overwriting it does the *most* damage: nothing fails, no DLL is
locked, nothing warns you, and the next `muld.exe` I launch silently runs whatever was written
there, unverified.

If you catch yourself reasoning "nothing is running there, so it's safe" — that is precisely the
inverted conclusion this section exists to prevent.

### Never

- Unpack, copy or `robocopy` into **any** directory `WindowsTerminalDev` is registered from —
  today that is `…\src\cascadia\CascadiaPackage\AppPackages\loose`.
- `Add-AppxPackage -Register` anything whose identity is `WindowsTerminalDev`.
- `Start-Process muld.exe` / `muld.exe <args>` — launching it is my gesture too, and with the handoff
  above a launch can land inside a process I am working in.
- Kill a `mullion.exe` whose path is under a Dev payload — except under the narrow, verified
  exception above.
- Run `tools\Register-DevSlot.ps1`.

### Check before every deploy

`-Register` is a silent no-op when the identity is already registered elsewhere, so *assume nothing*
about where a slot points. One command, every time:

```powershell
Get-AppxPackage -Name 'WindowsTerminal*' | Select-Object Name, InstallLocation
```

If `WindowsTerminalDev`'s `InstallLocation` is the directory you were about to write to — **stop and
tell me.** Do not "just this once" it.

The two-name form (`Get-AppxPackage Dev, Test`) reads better and **throws under Windows PowerShell
5.1** — `-Name` is `[string]` there, not `[string[]]`, so it dies with a parameter-binding error
before checking anything. A mandated check that errors is a check that gets skipped, and 5.1 is what
`powershell.exe` still resolves to. The wildcard works in both shells.

Each slot has its own package identity, so each has its own settings:
`%LOCALAPPDATA%\Packages\WindowsTerminal{Dev,Test}_8wekyb3d8bbwe\LocalState\settings.json`.

**"Run the test build" means launch `mult.exe`. It does not mean build anything.** If I want a
rebuild first I will say so.

## How a build reaches the Dev slot

One way, on every machine:

| Build | Stages into | Marker |
|---|---|---|
| `.github/workflows/build.yml` → `tools\Fetch-CIBuild.ps1` | `dev-staged-ci` | `dev-pending-ci.json` |

`tools\Install-CIBuildPoller.ps1` registers the Scheduled Task that keeps the newest successful
build staged, so the update offer appears without remembering to fetch anything. It never installs
— promotion is still the gesture.

CI runs `Deploy-TerminalSlots.ps1 -StageOnly`, so the payload is produced by the same script that
handles a local deploy — the staging, the byte-identity between the Test and Dev packages, and the
`dev-pending` contract are all shared. It cancels superseded runs
(`concurrency: cancel-in-progress`), because a run for a commit you have already pushed past is
building something nobody will install.

The Terminal reads **every** `dev-pending*.json` and offers the newest, and `SlotPromotion.h` is the
only place that decides which wins — so a local deploy's marker and a CI fetch's marker can both be
staged at once, and the older one is simply superseded. The promotion offer also suppresses "same
commit, both clean" rather than comparing timestamps alone — without that guard, a CI build of the
commit you are already running is always newer than your copy of it, so the button would never go
away no matter how often you pressed it.

### Two inherited workflows are disabled, deliberately

`Publish to WinGet` (on `release: published`, submits to `microsoft/winget-pkgs`) and `Spell
checking` (every push, against an expect-list that suits upstream's content). Both are **disabled in
repo settings rather than deleted**, so an upstream merge never conflicts over them and re-enabling
is one command. The winget one matters most: publishing a release here would otherwise open a PR
against Microsoft from a fork whose whole policy is that nothing goes upstream.

## How a build reaches the Test slot

`tools\Fetch-CIBuild.ps1` stages the Test half of the same CI artifact into `C:\TerminalSlots\test-staged-ci`
plus `test-pending-ci.json`, on the same run that stages the Dev half — no separate fetch needed.
Run `tools\Refresh-TestSlot.ps1` to actually pick it up: it closes any running `mult` (no confirmation
needed — see "The two slots"), backs up and restores `settings.json`/`state.json` across the
re-registration, registers from `test-staged-ci`, and launches `mult` to confirm it reaches a window.

`tools\Deploy-TerminalSlots.ps1` remains the only thing that ever *produces* a payload; CI simply
runs it now (`-StageOnly`, which skips the local script's own registration/launch/boot-check steps
— a GitHub Actions runner has no Developer Mode to register a package with anyway). Its local
doctrine is worth knowing because CI inherits all of it: build the solution once → package the Test
branding → package the Dev branding → unpack Test into `C:\TerminalSlots\test` and Dev into
`C:\TerminalSlots\dev-staged` → register only Test, preserving its `settings.json` across the
re-registration and asserting the resulting `InstallLocation` → start `mult` and wait for a window →
only then write `dev-pending.json`.

If `Deploy-TerminalSlots.ps1` ever runs here again: **`pwsh`, not `powershell`.** `OpenConsole.psm1`
is `#requires -Version 7.0`, so under 5.1 the deploy dies importing it — before building, packaging
or registering anything. From WSL that is the default you get, and piping the run through `tail`
hides it further: the pipeline reports `tail`'s exit code, so a deploy that never started reads as
success. See [`doc/troubleshooting.md`](doc/troubleshooting.md).

The Dev payload is *staged and left alone*. `dev-pending.json` (commit, branch, dirty, timestamp,
payload path) is how the running `muld` learns a newer build is waiting, so it can offer me the
promotion when I have no sessions I mind losing.

The mechanics underneath — for understanding, **not** for you to run by hand — are `makeappx unpack`
of the built `.msix` into the slot directory followed by
`Add-AppxPackage -Path <slot>\AppxManifest.xml -Register`. A rebuild alone changes nothing: the
registration points at the *unpacked copy*, so refreshing the `.msix` without re-unpacking deploys
nothing. Building the package from Visual Studio emits a loose layout directly (see
`doc/building.md`) — that path is Dev-registered on this machine and is therefore off limits.

### Promotion is my gesture

`tools\Register-DevSlot.ps1` re-registers the Dev slot from `C:\TerminalSlots\dev`. It refuses to run
while any process is live under the Dev payload — re-registering an identity from a different folder
requires removing the old registration first, and Windows will not remove a running package. It
refuses rather than terminating my windows, which is the correct instinct and the one you should
share. **You do not run it, full stop — no exception.** Either I press promote in `muld`, or I run it
myself.

This is a different script from the one the exception above names. `tools\Register-DevSlot.ps1` is
the raw, low-level re-registration — it is what `Promote-DevSlot.ps1` calls internally once a window
is already gone, and running it directly bypasses the swap, the settings backup, and the wait. The
exception covers only `Promote-DevSlot.ps1` — the actual button handler, which never force-closes a
window and simply waits (or gives up) when one is still open.

### Every Terminal built from this repo shares one process name

**`Get-Process -Name 'mullion'` matches the test build, the dev build, *and* the Terminal my
session is running in** (and a pre-rename build is `WindowsTerminal`, the same name as the Store app). Selecting `-First 1` and sending it input is how you type into my live
session. Always identify by executable path, and only ever target the test payload:

```powershell
Get-Process -Name 'mullion' | Where-Object {
    $_.Path -like 'C:\TerminalSlots\test\*' -and $_.MainWindowHandle -ne 0
}
```

Then record pre-existing PIDs before launching, refuse any PID that already existed, and confirm the
target window is foreground before sending a keystroke. A process whose path is under a Dev payload
or under `C:\Program Files\WindowsApps\` is off limits — read it if you must, never write to it.

## Every hidden background launch goes through `Invoke-Hidden.vbs`

This machine has Windows Terminal set as its default terminal-delegation host
(`HKCU\Console\%%Startup\DelegationTerminal`). That means a console-subsystem `.exe` launched with
`-WindowStyle Hidden` (PowerShell) or `SW_HIDE` (`ShellExecuteExW`) still gets a Windows Terminal
window created for it before the hidden-window request can suppress it — the console-delegation
handoff happens at a layer those flags don't reach. `tools\Invoke-Hidden.vbs` avoids this: it runs
the target via `WScript.Shell.Run(cmd, 0, False)`, which applies its hidden flag through
`ShellExecute` at a layer that doesn't trigger the delegation handoff at all.

Every current background launch in this repo goes through it: `Install-CIBuildPoller.ps1`'s
Scheduled Task action, and `TerminalPage::_armPromotionHelper` /
`TerminalPage::_ReconcileCiPollInterval` (`TerminalPage.SlotPromotion.cpp`) via `wscript.exe` as
`ShellExecuteExW`'s target instead of `powershell.exe`/`pwsh.exe` directly. Any new hidden launch
this repo adds should follow the same pattern rather than reintroducing a bare `-WindowStyle
Hidden`/`SW_HIDE` call.

## What CI will not tell you, and what costs a round trip to learn

Builds happen only in CI, so every mistake below costs ~40 minutes to discover.
Three of them cost exactly that on 2026-09-06; the first cost more, because it
does not fail at all.

- **A packaged asset must be covered by `src/cascadia/CascadiaResources.build.items`.**
  Each asset *directory* gets one entry, and that entry **is** a recursive glob —
  `ProfileIcons`, `ProfileGeneratorIcons` and `IntegrationIcons` are all
  `<Content Include="...\<Dir>\**\*">` with
  `<Link><Dir>\%(RecursiveDir)%(FileName)%(Extension)</Link>`. So **a new file
  dropped into one of those three directories is packaged with no edit at all**
  (verified 2026-09-12: `IntegrationIcons\shefrd.png` reached both staged payloads
  without being named anywhere). What still needs an entry is a **new directory**,
  or a file that lives outside them — and the `<Link>` is what decides where it
  lands, so without one the file goes to the package root. **The failure is
  silent**: green build, package produced, missing file, and you find out from a
  screenshot. Do not infer packaging from the source tree — check the staged
  payload (`C:\TerminalSlots\{dev,test}-staged-ci\...`), which is the only thing
  that proves it. And note the file extension: a grep filtered to
  `*.vcxproj`/`*.targets`/`*.props`/`*.wapproj` will not match `.build.items` and
  will tell you nothing references it.
- **A new runtimeclass with a constructor needs its `.g.cpp` compiled.** Declaring
  one in an `.idl` makes `module.g.cpp` reference a factory; the definition only
  exists once the generated `.g.cpp` is included somewhere. `EventArgs.cpp` lists
  every type in `EventArgs.idl` for exactly this reason. Miss it and you get
  `unresolved external symbol winrt_make_<Namespace>_<Type>` at link.
- **Qualify `IInspectable` and `Input::` in the Cascadia sources.** Files here
  carry `using namespace winrt::Windows::Foundation;` and
  `using namespace winrt::Windows::UI;` alongside `...::Xaml;`, so a bare
  `IInspectable` is ambiguous with the ABI struct of the same name at global
  scope, and a bare `Input::PointerRoutedEventArgs` binds to `Windows::UI::Input`
  rather than `Windows::UI::Xaml::Input`. Both were hit within an hour of each
  other by two different sessions.
- **`get_weak()` inside an implementation hands back the implementation, not the
  projection.** `weakThis.get()` is a `com_ptr<implementation::Foo>`, so reach
  private members straight through it — `page->_Thing()`. Two mistakes follow from
  forgetting that, and both cost a CI round on 2026-09-12, in opposite directions:
  calling a *projected* member on it (`page.IsLoaded()`) fails with
  `error C2039: 'IsLoaded': is not a member of 'winrt::com_ptr<D>'` and needs a
  `try_as<WUX::FrameworkElement>()` first; and wrapping it in `get_self`
  (`get_self<MainPage>(page)`) asks for `producer<MainPage, MainPage>` and fails
  with `C2079 ... uses undefined struct produce<D,I>` from inside `base.h`, which
  names no code of ours until you read the instantiation context. `get_self` is
  for going the other way, from a projected type to the implementation behind it —
  a `weak_ref<Editor::MainPage>` resolved from file scope does need it.
- **A `winrt/…h` the pch does not carry is a compile error at the first call, not
  at the type name.** `using namespace winrt::Windows::UI::Xaml::Documents;` and
  `Documents::Run r;` both compile against the forward declarations; `r.Text(...)`
  is what fails, with `C3779: a function that returns 'auto' cannot be used before
  it is defined`. The editor's pch carries `Media` and `Markup` but not
  `Documents`, so anything building inlines by hand includes it itself.
- **`StyleProperty` is on `FrameworkElement`, not `Control`.** `Control` inherits
  `Style`, but the static DP accessor is declared on the base. Same trap with
  **`Grid::SetRow`/`SetColumn`/`SetRowSpan`/`SetColumnSpan`**, which take a
  `FrameworkElement` even though `Grid::Children().Append` takes a `UIElement` —
  so `try_as<UIElement>` on something you are about to position in a grid is a
  compile error one round trip away.
- **`GETSET_BINDABLE_ENUM_SETTING`'s setter raises no `PropertyChanged`.** If
  anything on the page derives from that value, hand-write the setter and notify
  the derived properties — but never the property the binding just wrote (see
  `doc/troubleshooting.md`, "The whole Terminal vanishes while you are in Settings").
- **The view-model macros leave the class in `private:`.** `GETSET_BINDABLE_ENUM_SETTING`
  and `VIEW_MODEL_OBSERVABLE_PROPERTY` both end there, so anything declared straight
  after one lands private. A method the projection needs — an `x:Bind` target, or
  something reached through `get_self` — then fails to compile from a *generated*
  header (`error C2248: cannot access private member`, pointing at
  `Generated Files\winrt\...h`), which reads as a problem with the generated code
  rather than with where you put the declaration. Write `public:` before it. Cost
  a CI round twice on 2026-09-09, once for each macro.

Green means it compiles, packages and passes two unit suites. It says nothing
about whether a page draws, an icon resolves, or a setting takes effect — see
`doc/troubleshooting.md`, "A settings page comes up half filled in".

### Sharing the tree with another session

The CI half of this is under "Getting a build" above. The working-tree half:
stage commits by **explicit path**, never `git add -A` or `git commit -a`, so a
shared tree cannot sweep up half of someone else's change.

That is necessary and not sufficient. Explicit paths only choose *files*; if two
sessions have edited the same file, staging it by name still takes both sets of
edits. It happened to this very file on 2026-09-06 — two sessions independently
wrote up the CI-starvation problem, and whoever committed first carried the
other's paragraphs. Before staging a file someone else might be in, check
`git diff -- <path>` and confirm every hunk is yours.

## Settings UI: cards, expanders, and search

**A group of related settings is an accordion, not a run of loose cards.** Profiles →
Appearance is the reference (`Appearances.xaml:600` background image, `:197` colour scheme),
and every new or reworked group follows it.

- **`local:SettingsCard` = one setting.** **`local:SettingsExpander` = a group** whose members
  only make sense together, or a master setting plus what it gates.
- Three slots, three jobs:
  - **`Content`** — live controls that belong *in the collapsed header row* (the path box and
    Browse button, a current-value summary).
  - **`Items`** — a list of `SettingsCard` rows, one setting each. They are styled
    automatically by `SettingsExpanderItemStyleSelector`, so **never set `Style` or `Padding`**
    on them; a local `Style` suppresses the selector outright
    (`SettingsExpander.cpp::_ApplyItemContainerStyles`).
  - **`ItemsHeader`** — body content that is not a row of cards (a colour picker, a grid,
    nested expanders). Wrap it in `<ContentPresenter Padding="16,12,16,16">`; that exact
    padding is the established value.
- **Never put `IsEnabled` on the expander itself.** It disables the header too, so the master
  control you are gating on becomes unclickable. Repeat the binding on each nested card, which
  is what the background-image group does.
- **Every card and expander needs both `x:Uid` and `x:Name`.** The uid supplies
  `<uid>.Header`/`.Description` *and* is what `tools/GenerateSettingsIndex.ps1` keys the search
  entry on; a missing `x:Name` leaves that entry's `ElementName` empty, so a search result
  navigates to the page but never scrolls to or expands the setting.
- Nesting costs search nothing. The generator's XPath is a descendant axis, so cards inside
  `Items` and `ItemsHeader` get their own flat entries, and `Utils.cpp`'s
  `ExpandAncestorsAndBringIntoView` walks the logical tree, expands every ancestor expander,
  then waits out the 333 ms expand animation before scrolling. Exclude a uid only when search
  genuinely cannot reach it — see the Link Tooltip rule-editor block in `$ProhibitedUids`.
- **`IsForkFeature` works on both.** `SettingsExpander` forwards it to the `SettingsCard` that
  draws its header, so a group this fork added still gets the ◆ under `aylith.imprint`. Mark
  the expander, not every child.

One deliberate exception: `LinkTooltip.xaml`'s per-platform rule groups use a raw
`muxc:Expander`. That is a data-templated list with its own header controls, not a settings
group, and it stays as it is.

## Driving the UI

Automating the Settings UI means taking my keyboard and focus — see the idle rules in my global
CLAUDE.md, and use `AgentDriver.exe` (`~/projects/windows-control/agent-driver/`) rather than
hand-rolled input. Two specifics for this repo:

- **AgentDriver cannot send `Ctrl+,`**, the default Settings hotkey — its chord parser
  (`InputDriver.cs:198`) handles named keys plus single letters/digits only, no punctuation. Bind
  `Terminal.OpenSettingsUI` to a letter chord in the dev build's `settings.json` instead
  (`ctrl+shift+o` works).
- Prefer UI Automation over pixel coordinates for finding controls; it needs **Windows PowerShell
  5.1** (`Add-Type -AssemblyName UIAutomationClient`), not pwsh 7. Conversely the `GetLastInputInfo`
  idle snippet uses `Environment.TickCount64`, which does **not** exist in .NET Framework — under
  5.1 use `unchecked((uint)Environment.TickCount - dwTime)` or the type fails to compile and every
  idle check silently throws.
