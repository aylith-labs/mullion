// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// ActivityLog.h
//
// An append-only record of every process this Terminal starts, and every console
// client handed to it, so that "where did that window come from?" has an answer
// after the fact.
//
// Why a file and not ETW: the interesting case is a window that has already
// appeared. ETW keeps nothing unless a trace session was running before the
// event, which is exactly the situation you are not in when something surprises
// you. Upstream's defterm logging (microsoft/terminal#11537) is ETW plus a .wprp
// for that reason and answers a different question -- reproducing a known
// problem, not explaining one that already happened.
//
// Why it lives in WinRTUtils: the spawn sites are spread across projects that do
// not reference each other. TerminalConnection owns the profile launch and the
// defterm handoff, TerminalApp owns the background helpers, ShellExtension runs
// inside Explorer. WinRTUtils is the one library all of them already reference.
//
// Two constraints the implementation has to keep, both learned here rather than
// documented anywhere:
//
//   * Nothing on a launch path may block on disk. Record() formats the line on
//     the caller's thread and returns; the append happens on a threadpool timer.
//     See TerminalPage.SlotPromotion.cpp's "nothing on this path should ever
//     block a window from opening".
//   * The log is shared between processes. Each Terminal window can be its own
//     process, and both slots write to their own package's folder but several
//     windows share one. Appends are therefore done with FILE_APPEND_DATA and a
//     single WriteFile per batch, which the OS places at the end of the file
//     atomically -- no cross-process lock, and no read-modify-write.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::ActivityLog
{
    // What kind of event this is. Kept as a short stable string in the JSON so
    // the file stays readable and a new kind never renumbers an old one.
    namespace Kind
    {
        // A profile launched: we built the command line and started it.
        inline constexpr std::wstring_view ProfileLaunch{ L"profileLaunch" };
        // A console client was handed to us by console delegation. We did not
        // start it -- this is the case that explains a window appearing on its own.
        inline constexpr std::wstring_view Handoff{ L"handoff" };
        // One of our own background helpers (promotion, CI poll reconcile).
        inline constexpr std::wstring_view Helper{ L"helper" };
        // A short-lived probe, e.g. enumerating WSL distributions.
        inline constexpr std::wstring_view Probe{ L"probe" };
        // Explorer's "Open in Windows Terminal" verb, which runs in Explorer.
        inline constexpr std::wstring_view ShellVerb{ L"shellVerb" };
        // The elevation shim re-launching us as administrator.
        inline constexpr std::wstring_view Elevate{ L"elevate" };
        // A request on the control pipe (doc/control-pipe.md). Not a spawn: the
        // process fields name the CLIENT that asked, and commandLine says what it
        // asked for -- for send-input, the text it typed into a pane.
        inline constexpr std::wstring_view ControlPipe{ L"controlPipe" };
    }

    // One record. Every field is optional: fill in what the call site actually
    // knows rather than inventing a value, because a wrong cwd is worse than an
    // absent one when you are trying to identify a process.
    struct Entry
    {
        std::wstring_view kind;

        std::wstring exe; // full image path where we have it, not just a filename
        std::wstring commandLine;
        std::wstring cwd;

        uint32_t pid{ 0 };
        uint32_t parentPid{ 0 };
        std::wstring parentExe;

        std::wstring profileName;
        std::wstring profileGuid;
        std::wstring sessionId;

        // Free-form: why this happened, in the terms of the call site. "split
        // pane", "new tab", "promotion helper", "WSL distro probe".
        std::wstring reason;

        // controlPipe only, so a script reading the file does not have to parse
        // commandLine back apart: the op ("send-input"), the pane it named
        // ("1.0.3"), and how it ended ("ok", or the error code it was answered).
        std::wstring op;
        std::wstring target;
        std::wstring result;
    };

    // Queue a record. Cheap, non-throwing, and safe from any thread. Does no
    // disk I/O on the calling thread. A no-op when logging is disabled.
    void Record(const Entry& entry) noexcept;

    // Turn recording on or off, and bound the file. Called when settings load and
    // whenever they change; until it is called the log is off, so a crash before
    // settings are read cannot write anything.
    //
    // The on/off state CANNOT live in a static here. WinRTUtils is a
    // StaticLibrary, so TerminalApp, TerminalConnection, TerminalControl and
    // WindowsTerminal.exe each link their own copy of this translation unit and
    // therefore their own copy of any static. Settings are read in TerminalApp,
    // while the two records that matter most -- the profile launch and the
    // defterm handoff -- are raised in TerminalConnection, which would never see
    // the flag TerminalApp set. It compiles, links, and records nothing; found by
    // running it, because no build could have told us.
    //
    // So Configure writes a marker file beside the log and Record consults it.
    // That crosses module boundaries and, for free, process boundaries too --
    // several Terminal windows are several processes, and only the one that
    // happened to load settings would otherwise log at all.
    void Configure(bool enabled, uint32_t maxFileKilobytes) noexcept;

    // Whether recording is currently on, according to the marker. Cached briefly,
    // so calling it per record costs nothing.
    bool Enabled() noexcept;

    // Write anything queued, synchronously. For process shutdown -- the throttled
    // flush would otherwise be cancelled with records still pending.
    void Flush() noexcept;

    // Where the log is written. Alongside settings.json, so it is per-package:
    // the Dev and Test slots keep separate logs, which is what you want when
    // comparing them. Empty if the location cannot be determined.
    std::filesystem::path Path() noexcept;
}
