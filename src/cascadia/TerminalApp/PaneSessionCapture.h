// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Working out what a pane is ACTUALLY running right now -- with its flags --
// and what command would bring that back on the next start.
//
// Nothing upstream tracks this. TerminalPaneContent::GetNewTerminalArgs
// persists the commandline a pane was LAUNCHED with, so a pane sitting in
// `claude --resume <id>` comes back as a bare shell. This module answers the
// other question, and its output rides along in the same persisted layout.
//
// Two detection paths, because a pane's real work may not be a Windows
// process at all:
//
//   Native  - walk the Windows process tree down from the pane's own client
//             process and read the deepest descendant's command line out of
//             its PEB.
//   WSL     - ONE `wsl.exe` call per distro (never per pane) running a probe
//             script that reports every process carrying a WT_SESSION, its
//             foreground process group, argv, cwd, and any agent session
//             directory it holds open. Gated process-wide: never two at once,
//             at most one per 30 s while healthy, backing off to 5 min while
//             the distro is not answering, and the whole wsl.exe tree
//             (conhost included) killed through a job object at 10 s.
//
//             A cheaper source was considered and rejected. Reading /proc over
//             \\wsl.localhost goes through the same wedged service, as a
//             blocking file read in this process that no deadline can
//             interrupt -- strictly worse than a child we can kill. Shell
//             integration (OSC 7 and friends) gives a cwd, not the foreground
//             argv or the agent session id, and only for configured shells.
//
// The WSL path works because ConptyConnection gives every connection its own
// WT_SESSION guid AND appends it to WSLENV (ConptyConnection.cpp:61-94), so
// the value crosses into the distro and the shell for a given pane can be
// identified exactly. That is what makes three panes each running shefrd
// distinguishable; matching on process names alone cannot do it.
//
// Everything here is blocking -- a process snapshot, and a child process
// launch for WSL. Call it from a background thread; TerminalPage::
// _CaptureResumeCommands does.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TerminalApp::SessionResume
{
    // How an agent spells "reopen this conversation". Mirrors the table in
    // shefrd's src/agent_resume.rs; each row there was verified against the
    // agent's own --help, so keep this in sync with that file rather than
    // guessing new rows from memory.
    enum class SelectorStyle
    {
        FlagSpace, // claude --resume <id>
        FlagEquals, // copilot --resume=<id>
        Subcommand, // codex resume <id>
    };

    struct AgentRow
    {
        std::wstring_view Name; // matched against argv[0]'s basename
        std::wstring_view Program; // what we actually run, when it differs
        std::wstring_view Selector;
        SelectorStyle Style;
    };

    // Deliberately only the agents whose resume syntax is known. gemini,
    // aider, amp, goose and crush are absent because neither shefrd nor stith
    // has a verified row for them -- emitting a guessed flag would produce a
    // command that fails at restore, which is worse than restoring a shell.
    // Those can still be resumed as plain commands via resumeExtraPrograms.
    inline constexpr std::array<AgentRow, 17> AgentTable{ {
        { L"claude", L"claude", L"--resume", SelectorStyle::FlagSpace },
        { L"codex", L"codex", L"resume", SelectorStyle::Subcommand },
        { L"copilot", L"copilot", L"--resume", SelectorStyle::FlagEquals },
        { L"devin", L"devin", L"--resume", SelectorStyle::FlagSpace },
        { L"droid", L"droid", L"--resume", SelectorStyle::FlagSpace },
        { L"kimi", L"kimi", L"--session", SelectorStyle::FlagSpace },
        { L"mastracode", L"mastracode", L"--thread", SelectorStyle::FlagSpace },
        { L"pi", L"pi", L"--session", SelectorStyle::FlagSpace },
        { L"omp", L"omp", L"--resume", SelectorStyle::FlagEquals },
        { L"hermes", L"hermes", L"--resume", SelectorStyle::FlagSpace },
        { L"opencode", L"opencode", L"--session", SelectorStyle::FlagSpace },
        { L"qodercli", L"qodercli", L"--resume", SelectorStyle::FlagSpace },
        { L"qwen", L"qwen", L"--resume", SelectorStyle::FlagSpace },
        { L"kilo", L"kilo", L"--session", SelectorStyle::FlagSpace },
        { L"cursor", L"cursor-agent", L"--resume", SelectorStyle::FlagSpace },
        { L"agy", L"agy", L"--conversation", SelectorStyle::FlagSpace },
        { L"grok", L"grok", L"--resume", SelectorStyle::FlagSpace },
    } };

    // Daemon-backed session hosts. These need no session id: the server
    // outlives the Terminal (shefrd's runs at PPID 1, holding its own shells
    // and agents), so re-invoking the bare command reattaches to what is
    // already there. Never recurse into one looking for the agents inside --
    // the daemon restores those itself, and resuming them here too would
    // start a second copy of every one.
    inline constexpr std::array<std::wstring_view, 5> MultiplexerNames{
        L"shefrd", L"herdr", L"tmux", L"screen", L"zellij"
    };

    // What a pane was found to be running.
    struct CapturedPane
    {
        std::wstring SessionId; // WT_SESSION, joins this back to its pane
        std::vector<std::wstring> Argv; // foreground argv, flags included
        std::wstring AgentSessionId; // empty unless an agent session was found
        std::wstring TranscriptPath; // evidence the session is resumable
        // Where the foreground program runs, when it could be read (native panes:
        // from its PEB). The restored pane starts here, so the resume command runs
        // in the directory its conversation belongs to.
        std::wstring Cwd;
    };

    // What to type into the restored pane's shell.
    struct ResumePlan
    {
        std::wstring CommandLine;
        // Set only when the command reopens an agent CONVERSATION, as opposed
        // to merely re-running a program. Such a pane must NOT repaint its
        // saved scrollback: the agent redraws its own history, and showing
        // both leaves the pane with two copies of the same transcript.
        bool ResumesAgentSession{ false };
        // Where to start the restored pane so CommandLine runs in the right place.
        // Empty when unknown, which leaves the pane's own starting directory alone.
        std::wstring WorkingDirectory;
    };

    // Which programs the user has allowed back.
    struct Policy
    {
        bool Agents{ true };
        bool Multiplexers{ true };
        std::vector<std::wstring> Extra;
        std::vector<std::wstring> Excluded;
    };

    // What to ask about one pane. Distro empty means a native Windows pane.
    struct PaneProbe
    {
        std::wstring SessionId;
        unsigned long RootPid{ 0 };
        std::wstring Distro;
        // Only consulted for native panes, where there is no /proc to read an
        // exact agent session id from and the directory is the only handle we
        // have on which conversation was open.
        std::wstring Cwd;
    };

    // argv[0] reduced to the name a table row would match: no directory, no
    // .exe/.cmd/.bat suffix. `/home/me/.local/bin/claude` -> `claude`.
    std::wstring_view BaseName(std::wstring_view argv0) noexcept;

    const AgentRow* FindAgent(std::wstring_view name) noexcept;
    bool IsMultiplexer(std::wstring_view name) noexcept;

    // Drops a session selector the recorded argv already carries, so appending
    // a fresh one cannot leave two. Mirrors shefrd's strip_session_selectors,
    // including its deliberate omission of `-c`: codex spells --config that
    // way, and dropping a config flag would change what the command does
    // rather than merely de-duplicating a selector.
    std::vector<std::wstring> StripSessionSelectors(const std::vector<std::wstring>& argv);

    // The command that brings this pane back, or nullopt when policy excludes
    // it or nothing is worth replaying (a bare shell, an unknown program).
    std::optional<ResumePlan> BuildPlan(const CapturedPane& captured, const Policy& policy);

    // Whether a command line built by BuildPlan reopens an agent conversation,
    // recomputed from the string itself at restore time. Derived rather than
    // persisted alongside it so there is one source of truth: a command and a
    // flag saying what it does can disagree after a hand-edited state file,
    // and the flag is the half nothing would notice was wrong.
    bool ResumesAgentSession(std::wstring_view commandLine) noexcept;

    struct CaptureOutput
    {
        std::vector<CapturedPane> Panes;
        // Sessions whose distro was not probed this round -- a probe for it
        // was still outstanding, it is backing off after not answering, or it
        // just timed out. Their previous answer still stands; clearing it
        // would throw away a good resume command because WSL was busy.
        std::vector<std::wstring> Unknown;
    };

    // Blocking. One process snapshot plus at most one wsl.exe launch per
    // distinct distro, regardless of how many panes are asked about -- and
    // none at all for a distro whose previous probe is still running or that
    // is backing off (see ProbeGate.h). `urgent` (the shutdown capture) skips
    // the healthy-probe spacing, never the backoff.
    CaptureOutput Capture(const std::vector<PaneProbe>& panes, bool urgent = false);
}
