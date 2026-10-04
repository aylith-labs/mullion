# Troubleshooting a locally built Terminal

Failures specific to *this* fork's two-slot dev loop, written down because each
one cost real time and none of them says what it means. See `AGENTS.md` for the
slot doctrine itself; this file is only about diagnosing things that break.

## A slot starts and vanishes without ever showing a window

What you see: `mult` or `muld` "launches", no window appears, and the process is
gone in a couple of seconds. `Get-WinEvent -LogName Application` (event ID 1000)
reports something like:

```
Faulting application path: C:\TerminalSlots\dev\mullion.exe
Faulting module name:      ucrtbase.dll
Exception code:            0xc0000409
Exception Data (P10):      0000000000000007
```

**Read the exception data, not just the code.** `0xC0000409` is reported as
"stack buffer overrun", which it usually is not. The trailing parameter is the
`__fastfail` reason, and `7` is `FAST_FAIL_FATAL_APP_EXIT` — plain `abort()`.
Other values mean genuinely different things (`2` really is a stack cookie
failure, `5` is an invalid argument from the CRT). Faulting module `ucrtbase.dll`
just means `abort()` lives there; it says nothing about who called it.

### The cause seen on 2026-08-27

An **incremental build with stale generated artifacts**. Commit `8fef97d22`
changed `TabRowControl.idl`/`.xaml` and `AboutDialog.idl`/`.xaml`, but msbuild
considered several dependent DLLs up to date, so the deployed payload mixed
newly generated interfaces with DLLs built ~19 hours earlier. The result aborted
inside `AppHost::Initialize`, before any window existed.

The fix was a **full solution build**. Two incremental deploys did not fix it;
the full build did, and copying only those consistently built binaries into the
slot — same package, same settings, nothing else changed — made it start.

The precise stale artifact was never isolated to a single file, so treat this as
the rule rather than a story about one DLL:

> After changing a `.idl` or a `.xaml`, do not trust an incremental build.
> Build the whole solution before deploying, or verify the result actually
> starts.

`tools\Deploy-TerminalSlots.ps1` now enforces the second half: it starts the
registered Test slot and waits for a window before writing `dev-pending.json`,
so a build that cannot start can no longer be staged for promotion into the slot
that hosts live sessions. If it fails, Test stays registered so `mult` reproduces
it for you.

## Neither slot opens, and the process is *still running*

What you see: `muld` and/or `mult` "launch" and nothing appears — no window, no
error, no crash dialog, and nothing in the event log. Unlike the case above, the
process does **not** exit: `Get-Process` shows a healthy `mullion.exe`
that has been up for hours, responding, idling, using no CPU.

Confirm it in one command. The tell is a process that owns no window at all:

```powershell
tools\Repair-TerminalSlots.ps1          # report
tools\Repair-TerminalSlots.ps1 -Force   # clear the Test slot
```

`MainWindowHandle` is **not** a sound check on its own: it only finds a *visible*
top-level window, so a Terminal minimised to the notification area reads as `0`
and looks like a zombie when it is perfectly fine. The script counts every
top-level window the process owns, hidden ones included, which is the
distinction that actually matters.

### Why one failure becomes a permanent one

The emperor owns the single-instance identity for its package — the window class
and mutex mix in the package family name (`WindowEmperor::HandleCommandlineArgs`).
A process sitting there with zero windows therefore still answers for the whole
app: every later launch finds it, hands its commandline over via `WM_COPYDATA`,
and exits. One failed window creation swallows *every* subsequent launch, with no
symptom other than nothing happening, until the process is killed by hand.

`_postQuitMessageIfNeeded()` covers the ordinary routes to zero windows (the last
window closed; startup deliberately created none). It did not cover a creation
that *failed* — that exception is swallowed by `safe_void_coroutine` or by
`_messageHandler`'s catch-all — nor a handoff that produced nothing.
`WindowEmperor::_armNoWindowWatchdog()` is the backstop for both: after a failed
creation or a handoff, if there is still no window a few seconds later the
process quits, so the next launch gets a fresh process instead of talking to a
broken one.

### Seen on 2026-09-05

Both slots went windowless at 14:42, sixteen seconds apart, and stayed that way
for nearly seven hours. Neither was launched by hand — a script started them
while the desktop was busy elsewhere. Each still held a live headless ConPTY and
a `wsl.exe` child, so the tabs were never torn down: the window never arrived
(or died) while the session behind it survived. ManicTime put the last active
Terminal window at 14:21:51, well before the processes started, which is what
ruled out "the window opened and was closed".

Two things kept it invisible. A caught window-creation failure logs nowhere you
would think to look, and `Deploy-TerminalSlots.ps1`'s boot check *skipped itself*
whenever any process was already running under the payload — so once a zombie
existed, every later deploy quietly stopped verifying. The boot check now clears
windowless processes first, and only then decides whether something real is up.

## The whole Terminal vanishes while you are in Settings

What you see: you change something on a settings page, carry on, and every window
disappears a few seconds later. No dialog, no error. The event log has:

```
Faulting application name: mullion.exe
Faulting module name:      Windows.UI.Xaml.dll
Exception code:            0xc000041d
```

**Do not trust that exception code.** `0xC000041D` is
`STATUS_FATAL_USER_CALLBACK_EXCEPTION`, which reads like "an exception escaped a
XAML callback" and sends you hunting for a throw. The instance found on the Link
Tooltip page was a **stack overflow** (`0xC00000FD`); `0xC000041D` is only the
kernel reporting a user callback that never returned. There was no exception at
all.

### The actual cause: a setter that announces its own property

A `Current*` view-model setter behind a combo box is the write half of
`SelectedItem="{x:Bind ..., Mode=TwoWay}"`. The binding calls it *because* the
selection already changed. Calling `_NotifyChanges(L"CurrentThatSameProperty")`
inside it tells the binding to re-read the getter and assign `SelectedItem`
again; the combo box raises its change; the binding calls the setter again.

`GETSET_BINDABLE_ENUM_SETTING` in `Utils.h` deliberately does not notify, which
is why every page built on the macro is unaffected and only pages with
hand-written setters die. Announcing a *different* property is fine.

### Why it is so hard to recognise

Every symptom points somewhere other than the cause:

- **It looks asynchronous.** It is not. It is a synchronous loop that takes about
  five seconds to exhaust a megabyte of stack, so the process dies one
  interaction after the click that started it -- which is why it reads as "any
  dropdown, at random, later", and why "it dies when I scroll" is a coincidence:
  the scroll was just occupying the time.
- **Nothing catches it.** `Application.UnhandledException` never fires (a stack
  overflow is not a managed exception). A first-chance vectored handler logs
  nothing either, because no exception is raised. Both were tried here; both are
  dead ends, and their silence is itself the clue that no throw is involved.
- **Bisecting the data teaches nothing**, because the data is irrelevant: it
  crashes with `hyperlink.tooltipRules` set to `[]` and with every integration
  `enabled: false`.

### What actually finds it

Attach a debugger; nothing else worked. WinDbg ships with `cdb.exe`, and no
elevation is needed to debug your own process:

```powershell
$cdb = "C:\Program Files\WindowsApps\Microsoft.WinDbg_*_x64__8wekyb3d8bbwemd64\cdb.exe"
# script.txt:  .symfix / .reload / sxe -c "kb 120; .logclose; qd" c00000fd / g
& $cdb -p <pid-of-mult> -cf script.txt -logo out.log
```

Then reproduce. The stack names the loop directly -- a repeating cycle of
`CItemsControl::SetValue` -> `NotifyPropertyChanged` ->
`CEventSourceBase<IDependencyPropertyChangedCallback>::Raise` -> our frames ->
`SetValueByKnownIndex` -> back to the top. Symbols for the fork's own DLLs are
not published, so those frames show as `module+offset`, which is enough: seeing
our module inside the cycle at all is the finding.

### A related shape worth keeping in mind

A binding getter that can *throw* produces a genuinely similar-looking crash.
`IMap::Lookup` throws `hresult_out_of_bounds` for an absent key, and settings
view models are full of `_SomethingMap.Lookup(_Settings.Something())`. In a
getter reached by `x:Bind` that is not a failed binding, it is a dead process.
The macro and `_lookupEnumEntry` in `LinkTooltipViewModel.cpp` both guard it now.
That was not the cause of the crash above -- `Lookup` was never throwing -- but
it is the other way to kill the process from a binding.

The same class also arrives as a use-after-free: a lambda that captured a raw
`this` and now lives inside a view model object XAML still holds through an
`ItemsSource`, read during teardown. Weak references, or capture the settings
object rather than the view model.

## Opening Settings kills the Terminal instantly

Symptom-wise this is the twin of the section above, and it is a different bug.
Telling them apart is worth thirty seconds, because the section above sends you
looking for recursion and there is none.

```
Exception code: 0xc0000005   Fault offset: 0x4dbed
Faulting module name: Microsoft.Terminal.Settings.Model.dll
```

Two things separate it from the stack overflow. The **faulting module is our own
DLL**, not `Windows.UI.Xaml.dll`. And the event log holds **two records five
seconds apart for one PID** -- `0xC0000005` first, then `0xC000041D` at the same
offset. That pairing is one death reported twice, not a five-second recursion:
the AV is the crash, and `0xC000041D` is only the kernel noting afterwards that
the user callback never returned. The stack-overflow case has the same delay for
a different reason, which is exactly why the two get confused.

### The cause: a resource string that does not exist

`ScopedResourceLoader::GetLocalizedString` is one unguarded line
(`ScopedResourceLoader.cpp:36`):

```cpp
return _resourceMap.GetValue(resourceName, _resourceContext).ValueAsString();
```

A missing key does not give you a blank label. `GetValue` returns a **null**
`ResourceCandidate` and `ValueAsString()` dereferences it. Every `RS_`, `RS_fmt`,
`RS_switchable_` and `USES_RESOURCE` in the tree bottoms out here, so any absent
string is a hard crash at the first lookup.

The trap is that the key is often never written down. `ActionArgsMagic.h` pastes
it together from the argument's own name -- `LOCALIZED_NAME(Amount)` becomes
`L"AmountActionArgumentLocalized"` -- so adding one line to an args macro creates
a resource requirement that no grep for the string will ever find. The Settings
UI asks every action for its argument descriptors as it opens, so the miss lands
the instant you go to Settings, far from the commit that caused it.

`tools\Check-SettingsModelConsistency.ps1` now covers both halves (every action
argument has its localized name; every key named in the Settings Model exists in
the resw) and runs in CI **before** the build. Nothing later can catch this: the
offending commit compiled, packaged and passed both test suites.

### What found it, with no build and no running Terminal

Windows was already writing full dumps to `%LOCALAPPDATA%\CrashDumps`, and the
WinDbg package ships `cdb.exe`, which needs no elevation:

```powershell
& "$env:ProgramFiles\WindowsApps\Microsoft.WinDbg_*_x64__8wekyb3d8bbwe\amd64\cdb.exe" `
    -z "$env:LOCALAPPDATA\CrashDumps\mullion.exe.<pid>.dmp" `
    -y 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols' -c ".ecxr; k 30; q"
```

Our frames come back as `Module!DllGetActivationFactory+0xNNNN` because the
payload ships no PDBs, which is still enough: subtract that export's RVA to get a
module RVA, then read the code out of the shipped DLL with the MSVC `dumpbin` that
Visual Studio installs (`dumpbin /disasm:nobytes`, image base `0x180000000`).
The frame that loads the key shows it literally --

```
lea rax,[00000001801E6300h]      ; the key
mov qword ptr [rbp-50h],1Dh      ; 29 wide chars
```

-- and mapping that RVA through the section table prints
`"AmountActionArgumentLocalized"`. `makepri dump /if resources.pri /dt detailed`
on the payload confirms whether a key really shipped, which beats reading the
`.resw` and assuming.

This is worth remembering generally: **the dumps are already there.** Neither
this hunt nor the one above needed the Test slot, a rebuild, or the keyboard.

## The Dev slot went *backwards* after pressing promote

What you see: you promote, `muld` relaunches, and the tab row or About dialog now
names an older commit than the one you were running.

`C:\TerminalSlots\promote-dev.log` is the record and it names the payload, so
start there:

```
08:41:57Z promotion requested (waitForPid=38916 relaunch=True payload=C:\TerminalSlots\dev)
08:41:59Z Swapping in the staged build from C:\TerminalSlots\dev-staged-ci
```

`waitForPid` set with `relaunch=True` is the in-app button; `waitForPid=0` is a
hand-run promotion.

### Why it happened on 2026-08-28

`SlotPromotion::DiffersFromRunning()` returned `true` for **any** commit
mismatch, before it ever reached its timestamp comparison. A different commit is
not evidence of being newer — it is just as easily older. A stale
`dev-pending-ci.json` describing an Aug-26 CI build was therefore offered to a
window running a newer local build, and promoting it installed the older one
over the newer. The function's own comment claimed an older marker "never
advertises itself as new", which the code did not implement.

It is now `IsNewerThanRunning()`, and build time is checked first and
unconditionally: a payload built before the running build is never offered,
whatever commit it names.

### Checking what is on offer

Every `dev-pending*.json` in `C:\TerminalSlots` is a candidate — one per
producer (`dev-pending.json` from a local deploy, `dev-pending-ci.json` from
`Fetch-CIBuild.ps1`), by design, because the producers run on different machines
and know nothing about each other. Compare each marker's `timestampUtc` against
the running build before promoting:

```powershell
Get-ChildItem C:\TerminalSlots -Filter 'dev-pending*.json' |
    ForEach-Object { $_.Name; Get-Content $_.FullName | ConvertFrom-Json |
                     Select-Object commit, branch, timestampUtc, payload }
```

A marker describing a build older than the one you are running is stale.
Promotion consumes only the marker it used, so another producer's marker
survives on purpose — it may still describe something genuinely newer.

## Diagnosing a startup abort without a debugger

There is no `cdb.exe`/WinDbg on this machine, and WER keeps only `Report.wer`
(no minidump) in `ReportArchive`. What works instead is making the process
report on itself. Add this temporarily to
`src\cascadia\WindowsTerminal\main.cpp`, build **only** that project, and revert
it afterwards — it is a diagnostic, never something to commit.

Three pieces, and you want all three:

- **A vectored exception handler** (`AddVectoredExceptionHandler(1, ...)`),
  filtering on `0xE06D7363` (the MSVC C++ throw code). This is process-wide and
  fires on *every* thread, before unwinding. It is noisy by design — Terminal
  throws and catches routinely — so read the last entry.
- **A `SIGABRT` handler** plus
  `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)`. `abort()`
  bypasses SEH and VEH entirely, so hooking the signal is the only way to see
  *who called it*. This is the one that actually located the fault.
- **`std::set_terminate`** — useful, but see the trap below: it is per-thread in
  MSVC, so it silently does nothing if the failure is on a UI thread.

Symbolize in-process with dbghelp (`SymInitialize` + `SymFromAddr` over
`RtlCaptureStackBackTrace`); `#pragma comment(lib, "dbghelp.lib")`. The PDBs sit
next to the built binaries and the path is embedded in the image, so frames
resolve with no extra setup — as long as the binary and its PDB match.

## A settings page comes up half filled in, and nothing crashes

What you see: a page opens with some controls correct and the rest blank or
stale. No dialog, no crash, nothing in the event log. On the Link Tooltip page
this presented as a rule editor that worked the first time you opened a rule and
came up completely empty on every visit afterwards — while the breadcrumb above
it still named the rule correctly, and the list behind it still showed the
rule's pattern.

**The data is fine. The page never finished drawing itself.**

### The cause: an exception swallowed inside a binding update

A single `PropertyChanged` for a property with dependents — `CurrentRule`, say —
is answered by XAML running *every* `ViewModel.CurrentRule.*` binding on the page
from one generated function. That function is straight-line code with no error
handling, and it runs inside a delegate. `winrt::impl::invoke` in the SDK's
`base.h` wraps every delegate call in `catch (...)` and reports nothing:

```cpp
template <typename Delegate, typename... Arg>
bool invoke(Delegate const& delegate, Arg const&... args) noexcept
{
    try { delegate(args...); }
    catch (...) { return report_failed_invoke(); }
    return true;
}
```

So the first target that throws abandons every binding after it in that
function, silently. No crash, because the throw never leaves the event raise.

The instance found here was a ComboBox whose `ItemsSource` was being replaced
while it still held a selection from the previous item. The fix was to bind
`ItemsSource` to a **page-level** collection that never changes — which is what
`KindList`, `FileTypeGroupList` and `IntegrationChoices` already did, and why
only the two pickers built from a per-rule list were affected.

### Reading the symptom

The order matters and it is the whole diagnosis. Bindings are updated in
**reverse document order**, with multiply-referenced properties hoisted first.
So:

- **What is still correct tells you where it stopped.** On the blank rule editor,
  the `IsLinkKind`/`IsTextKind` visibilities were right for each rule while
  everything else was stale — those two are hoisted to the front, and nothing
  after them ran.
- **A property announced on its own still works.** Applying a preset filled the
  same page in completely, because `ApplyPreset` raises its properties one at a
  time; each is a separate event raise, so one bad target costs only its own
  field. That contrast is the confirmation.

### Guarding against it

Two habits, both used on that page now:

- Never hand a `Selector` a different `ItemsSource` as part of a batched update.
  Bind item lists to page-level properties.
- For a page whose whole body hangs off one object, announce that object's
  properties individually as well after the batch (`NotifyAllProperties`). It is
  belt and braces, but the batch is not something a page should depend on.

Note this is the *opposite* symptom to the stack overflow above, on the same
page, from the same family of mistake: there, a setter announced its own
property and never returned; here, a target threw and was silently dropped. If a
settings page misbehaves, establish first whether it is dying or merely
incomplete — they are found in completely different ways.

## Tests that lie

Every one of these produced a confident, wrong conclusion.

**Launching a slot payload exe directly.** `C:\TerminalSlots\test\mullion.exe`
started from PowerShell gets **no package identity**, so activating the
`TerminalApp.App` WinRT class fails with `0x80040154 REGDB_E_CLASSNOTREG` and
the process aborts — *for a perfectly healthy build*. Launch through the alias
(`mult`) or the AUMID (`shell:AppsFolder\WindowsTerminalTest_8wekyb3d8bbwe!App`).
The alias is a stub that exits immediately, so find the real process by payload
path, not by the PID you started. A run with no identity is visible after the
fact: event 1000 shows an empty `Faulting package full name`.

**"I rebuilt and it still crashes."** Check that the rebuild actually produced a
new binary. An incremental build can decide the exe is up to date and never
relink it, so you have tested nothing. Compare the PE timestamp
(`Sig[3] Application Timestamp` in WER, or the `Application Timestamp` field of
event 1000) across runs — not the file's `LastWriteTime`, which `makeappx
unpack` rewrites for every file in the payload anyway.

**Frames that resolve to a random export.** A stack full of
`DllGetActivationFactory + 0x19e93` and `DllCanUnloadNow + 0x4c2f1` does not
mean the code is anywhere near activation. It means dbghelp had no symbols for
that module and fell back to the nearest preceding export. Usually the DLL in
the slot is older than the PDB next to the rebuilt one. Copy the freshly built
DLLs into the payload and the names appear.

**Pipeline exit codes.** `& msbuild.exe ... | Select-String ...` leaves
`$LASTEXITCODE` set by `Select-String`, so a failed build reports success. This
is the same trap `AGENTS.md` records for `tail`. Redirect to a file, read
`$LASTEXITCODE` on the very next line, then grep the file.

**`api-ms-win-*.dll` "missing".** Those are API sets resolved by the loader, not
files on disk. A naive existence check reports dozens of missing dependencies
for a perfectly loadable DLL. Likewise, `LoadLibraryW` on a payload DLL fails
with error 126 purely because the caller's directory is not on the search path —
use `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`.

**Checking out `upstream/main` in the working tree.** It regenerates the WinRT
projections, so coming back to `main` leaves `Microsoft.Terminal.Settings.Model.winmd`
without fork-only members and the next build fails with
`error C2039: 'HyperlinkClickable': is not a member of ...`. Recover with a
full solution build. Bisect in a separate worktree at a short path instead (see
`AGENTS.md` on `MAX_PATH` and on WSL and `git worktree prune`).

## Builds that die on memory, not on code

```
error C3859: Failed to create virtual memory for PCH
error C1076: compiler limit: internal heap limit reached
```

This is the commit limit, not a disk or code problem. Check
`Get-Counter '\Memory\Committed Bytes','\Memory\Commit Limit'` rather than free
RAM: a large resident WSL VM (measured at 84 GB here) can leave ~46 GB of commit
against a 209 GB limit while RAM looks fine.

**`/m` alone is not enough.** It bounds how many *projects* build at once, while
`MultiProcessorCompilation` independently forks one `cl.exe` per core *inside*
each project. On this 32-core machine a full rebuild died at both `/m` and
`/m:4`, and survived at `/m:2 /p:CL_MPCount=2`. `Deploy-TerminalSlots.ps1
-MaxCpuCount N` now caps both.

## Verifying without taking the keyboard

A startup check has to launch a GUI app, which normally steals focus. To test
while the machine is in use, launch onto a **separate desktop**: `CreateDesktop`,
then `CreateProcess` with `STARTUPINFO.lpDesktop` set to it. Windows created
there cannot take focus, and `EnumDesktopWindows` still tells you whether one
appeared (`Process.MainWindowHandle` will not — it only enumerates the calling
thread's desktop). Read the exit code from the process handle you own via
`GetExitCodeProcess`; `Process.ExitCode` on a process .NET did not start throws
and is easily mistaken for a crash code.

Note the limitation that made this insufficient on its own here: a
separate-desktop launch is still an *unpackaged* launch, so it hits the
identity trap above. It is a good way to smoke-test a binary, not a substitute
for starting the registered slot.

## Local build traps, retained for reference

Builds happen in CI now (see the build section of `AGENTS.md`), so none of these
should bite in day-to-day work. They are kept because they each cost a real
investigation, and because a one-off local build — bisecting, or reproducing
something CI cannot — will meet them again.

### `LNK1104: cannot open file ...` on a file that is plainly there

It means "path too long", not "missing". Some package references expand through
unnormalised `build\native\..\..\runtimes\...` segments, which pushes them past
`MAX_PATH` (260). A worktree under `%TEMP%\claude\<session>\scratchpad\`
produced a 267-character reference and failed to link; the same worktree at
`C:\wt-tm` produced 155 and linked. MSBuild also warns `MSB8029` about output
directories under Temp. Keep any build tree on a short root.

### `Invoke-OpenConsoleBuild -p:Configuration=Release` silently loses the switches

PowerShell parses `-p:Configuration=Release` as the cmdlet's own `-p:` parameter
with the value `Configuration=Release`, strips the prefix, and MSBuild receives a
bare `Configuration=Release` — then dies with `MSB1008: Only one project can be
specified`. Call `msbuild.exe` directly with `/p:` switches, or pass `--%` to
stop PowerShell parsing the rest of the line.

### Building a single project needs `SolutionDir`

`.vcxproj` files here import `$(SolutionDir)src\common.build.pre.props`, which is
empty when msbuild is pointed at a project rather than the solution:
`MSB4019: The imported project "...\src\common.build.pre.props" was not found`.
Pass it explicitly, with the trailing separator:
`msbuild .\src\types\lib\types.vcxproj /p:SolutionDir=C:\Users\steve\projects\terminal\`.

### A build tree is expensive

A warm tree here measured 40.7 GB: 33.9 GB across the eight `obj\` directories,
5.5 GB in `bin\`, plus `packages\` and `AppPackages\`. `NuGet.Config` pins
`globalPackagesFolder` and `repositoryPath` to `.\packages` relative to itself, so
every worktree pays the whole cost again. Nothing under those paths is tracked:

```powershell
Get-ChildItem . -Recurse -Directory -Filter obj | Remove-Item -Recurse -Force
Remove-Item .\bin, .\packages, .\src\cascadia\CascadiaPackage\AppPackages -Recurse -Force
```

See also "Builds that die on memory, not on code" above, which is the other cost
of building wide on this machine.
