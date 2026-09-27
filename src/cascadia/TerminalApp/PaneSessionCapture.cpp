// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "PaneSessionCapture.h"
#include "ProcessCapture.h"

#include <TlHelp32.h>
#include <shellapi.h>
#include <winternl.h>
#include <algorithm>
#include <limits>
#include <map>
#include <sstream>
#include <til/string.h>
#include <til/u8u16convert.h>

namespace TerminalApp::SessionResume
{
    namespace
    {
        // Shells and hosts that are the pane's scaffolding rather than its
        // work. A pane whose deepest descendant is one of these is running
        // nothing worth bringing back.
        constexpr std::wstring_view ShellNames[]{
            L"cmd", L"powershell", L"pwsh", L"wsl", L"wslhost", L"bash", L"sh", L"zsh", L"fish", L"conhost", L"windowsterminal", L"mullion", L"openconsole"
        };

        // Interpreters that front for the program we actually care about:
        // `node .../claude` is claude, not node.
        constexpr std::wstring_view ScriptHosts[]{
            L"node", L"python", L"python3", L"py", L"ruby", L"bun", L"deno", L"perl"
        };

        bool NameIn(std::wstring_view name, const std::wstring_view* first, size_t count) noexcept
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (til::equals_insensitive_ascii(name, first[i]))
                {
                    return true;
                }
            }
            return false;
        }

        // Record and field separators the WSL probe uses. Chosen because no
        // argv, path or guid can contain them, so parsing never has to guess.
        constexpr wchar_t FieldSep = L'\x1e';
        constexpr wchar_t ArgSep = L'\x1f';

        std::vector<std::wstring> SplitOn(std::wstring_view text, wchar_t sep)
        {
            std::vector<std::wstring> parts;
            size_t start = 0;
            while (start <= text.size())
            {
                const auto next = text.find(sep, start);
                if (next == std::wstring_view::npos)
                {
                    parts.emplace_back(text.substr(start));
                    break;
                }
                parts.emplace_back(text.substr(start, next - start));
                start = next + 1;
            }
            return parts;
        }

        // Every character that isn't alphanumeric becomes '-'. This is the
        // encoding Claude Code uses for its per-project directories:
        // C:\Users\steve\projects\terminal -> C--Users-steve-projects-terminal
        // /home/stevenp/.claude            -> -home-stevenp--claude
        std::wstring EncodeProjectDir(std::wstring_view path)
        {
            std::wstring encoded;
            encoded.reserve(path.size());
            for (const auto ch : path)
            {
                const auto alnum = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
                encoded.push_back(alnum ? ch : L'-');
            }
            return encoded;
        }

        std::wstring QuoteIfNeeded(std::wstring_view arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\"'\\$`|&;<>()*?[]{}~#!") == std::wstring_view::npos)
            {
                return std::wstring{ arg };
            }

            // Single quotes, POSIX style: everything inside is literal, and an
            // embedded quote is spelled by closing, escaping, reopening. The
            // command is typed into the pane's own shell, which on the WSL
            // side is where every recorded argv came from in the first place.
            std::wstring quoted{ L'\'' };
            for (const auto ch : arg)
            {
                if (ch == L'\'')
                {
                    quoted.append(LR"('\'')");
                }
                else
                {
                    quoted.push_back(ch);
                }
            }
            quoted.push_back(L'\'');
            return quoted;
        }

        std::wstring JoinArgv(const std::vector<std::wstring>& argv)
        {
            std::wstring line;
            for (const auto& arg : argv)
            {
                if (!line.empty())
                {
                    line.push_back(L' ');
                }
                line.append(QuoteIfNeeded(arg));
            }
            return line;
        }

        // ---- native Windows process tree ------------------------------------

        struct NativeProcess
        {
            DWORD Pid{};
            DWORD ParentPid{};
            std::wstring Name;
        };

        std::vector<NativeProcess> SnapshotProcesses()
        {
            std::vector<NativeProcess> all;

            wil::unique_handle snapshot{ CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
            if (!snapshot)
            {
                return all;
            }

            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot.get(), &entry))
            {
                do
                {
                    all.push_back(NativeProcess{ entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile });
                } while (Process32NextW(snapshot.get(), &entry));
            }

            return all;
        }

        // The command line of a running process, read straight out of its PEB.
        // ConptyConnection has the same routine privately for the defterm
        // handoff; it lives in another DLL and is not projected, so this is a
        // deliberate second copy rather than a new cross-project WinRT surface
        // built to share twenty lines.
        std::wstring CommandLineOf(DWORD pid)
        {
            wil::unique_handle process{ OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid) };
            if (!process)
            {
                return {};
            }

            // Spelled out locally rather than taken from winternl.h, matching
            // ConptyConnection::_commandlineFromProcess: the public header's
            // version is mostly Reserved fields.
            struct PROCESS_BASIC_INFORMATION
            {
                NTSTATUS ExitStatus;
                PPEB PebBaseAddress;
                ULONG_PTR AffinityMask;
                KPRIORITY BasePriority;
                ULONG_PTR UniqueProcessId;
                ULONG_PTR InheritedFromUniqueProcessId;
            } info{};

            const auto status = NtQueryInformationProcess(process.get(), ProcessBasicInformation, &info, sizeof(info), nullptr);
            if (status < 0 || !info.PebBaseAddress)
            {
                return {};
            }

            PEB peb{};
            if (!ReadProcessMemory(process.get(), info.PebBaseAddress, &peb, sizeof(peb), nullptr) || !peb.ProcessParameters)
            {
                return {};
            }

            RTL_USER_PROCESS_PARAMETERS params{};
            if (!ReadProcessMemory(process.get(), peb.ProcessParameters, &params, sizeof(params), nullptr) || !params.CommandLine.Buffer || params.CommandLine.Length == 0)
            {
                return {};
            }

            std::wstring commandLine(params.CommandLine.Length / sizeof(wchar_t), L'\0');
            if (!ReadProcessMemory(process.get(), params.CommandLine.Buffer, commandLine.data(), params.CommandLine.Length, nullptr))
            {
                return {};
            }
            return commandLine;
        }

        std::vector<std::wstring> SplitCommandLine(const std::wstring& commandLine)
        {
            std::vector<std::wstring> argv;
            if (commandLine.empty())
            {
                return argv;
            }

            auto count = 0;
            const wil::unique_hlocal_ptr<PWSTR[]> parsed{ CommandLineToArgvW(commandLine.c_str(), &count) };
            if (!parsed)
            {
                return argv;
            }
            for (auto i = 0; i < count; ++i)
            {
                argv.emplace_back(parsed.get()[i]);
            }
            return argv;
        }

        // The SHALLOWEST descendant that isn't shell scaffolding -- the first
        // thing the pane's shell actually launched.
        //
        // Not the deepest: an agent spawns children of its own, so a pane
        // running `claude` has node processes for every MCP server hanging off
        // it, and the deepest descendant is one of those rather than claude.
        // The WSL side gets this right from the terminal's foreground process
        // group; Windows has no equivalent, so depth from the pane's own root
        // stands in for it.
        //
        // Depth, rather than "the first name we recognize", so a pane running
        // something no table covers still reports honestly and BuildPlan is
        // left to decide whether policy allows it back.
        std::optional<DWORD> FirstWorkerBelow(DWORD rootPid, const std::vector<NativeProcess>& all)
        {
            std::map<DWORD, std::vector<DWORD>> childrenOf;
            std::map<DWORD, std::wstring> nameOf;
            for (const auto& proc : all)
            {
                childrenOf[proc.ParentPid].push_back(proc.Pid);
                nameOf[proc.Pid] = proc.Name;
            }

            std::optional<DWORD> best;
            auto bestDepth = std::numeric_limits<int>::max();

            std::vector<std::pair<DWORD, int>> queue{ { rootPid, 0 } };
            std::vector<DWORD> seen{ rootPid };
            while (!queue.empty())
            {
                const auto [pid, depth] = queue.back();
                queue.pop_back();

                if (depth > 0)
                {
                    const auto name = BaseName(nameOf[pid]);
                    if (!NameIn(name, ShellNames, std::size(ShellNames)) && depth < bestDepth)
                    {
                        bestDepth = depth;
                        best = pid;
                    }
                }

                const auto kids = childrenOf.find(pid);
                if (kids == childrenOf.end())
                {
                    continue;
                }
                for (const auto child : kids->second)
                {
                    // A parent pid can be recycled onto an unrelated process;
                    // without this the walk can loop forever.
                    if (std::find(seen.begin(), seen.end(), child) != seen.end())
                    {
                        continue;
                    }
                    seen.push_back(child);
                    queue.emplace_back(child, depth + 1);
                }
            }
            return best;
        }

        // Newest transcript for a directory, used on the Windows side where
        // there is no /proc to read an exact session id out of. Ambiguous when
        // two native panes share a cwd; the caller hands out one transcript
        // per pane, newest first, so at least they don't all collide on one.
        std::vector<std::wstring> TranscriptsForCwd(const std::wstring& cwd)
        {
            std::vector<std::pair<uint64_t, std::wstring>> found;
            if (cwd.empty())
            {
                return {};
            }

            wchar_t profile[MAX_PATH]{};
            DWORD length = ARRAYSIZE(profile);
            if (!GetEnvironmentVariableW(L"USERPROFILE", profile, length))
            {
                return {};
            }

            const auto dir = std::wstring{ profile } + LR"(\.claude\projects\)" + EncodeProjectDir(cwd);
            WIN32_FIND_DATAW data{};
            wil::unique_hfind find{ FindFirstFileW((dir + LR"(\*.jsonl)").c_str(), &data) };
            if (!find)
            {
                return {};
            }
            do
            {
                if (data.nFileSizeLow == 0 && data.nFileSizeHigh == 0)
                {
                    continue;
                }
                const uint64_t written = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                std::wstring name{ data.cFileName };
                if (const auto dot = name.rfind(L'.'); dot != std::wstring::npos)
                {
                    name.erase(dot);
                }
                found.emplace_back(written, std::move(name));
            } while (FindNextFileW(find.get(), &data));

            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

            std::vector<std::wstring> ids;
            ids.reserve(found.size());
            for (auto& item : found)
            {
                ids.push_back(std::move(item.second));
            }
            return ids;
        }

        // ---- WSL probe -------------------------------------------------------

        // Reports every process carrying a WT_SESSION: its pid, parent, process
        // group, the foreground group of its terminal, cwd, the agent session
        // directory it holds open, and argv. The caller matches WT_SESSION back
        // to a pane and picks the foreground process out of the group.
        //
        // Two filters carry all the weight, and without them the answer is
        // noise. A daemon started from a pane INHERITS that pane's WT_SESSION
        // and keeps it forever, so on this machine one guid covered 143
        // processes across unrelated shells:
        //
        //   tpgid > 0            drops detached daemons, which have no
        //                        controlling terminal and so can never be
        //                        what a pane is showing.
        //   no ownership marker  drops everything a multiplexer owns.
        //                        HERDR_ENV/TMUX/STY/ZELLIJ are set on the
        //                        processes shefrd, tmux, screen and zellij
        //                        run for themselves; those are the daemon's
        //                        to restore, and recursing into them would
        //                        start a second copy of every agent inside.
        //
        // With both, that same guid resolves to exactly the two processes that
        // belong to the pane: its shell, and the shefrd client in front of it.
        //
        // Fed to `sh -s` on stdin rather than passed as -c "...", which keeps
        // every quote in here literal instead of surviving two rounds of
        // expansion on the way through wsl.exe.
        constexpr std::string_view WslProbeScript = R"SH(
for d in /proc/[0-9]*; do
  p=${d##*/}
  [ -r "$d/environ" ] || continue
  e=$(tr '\0' '\n' < "$d/environ" 2>/dev/null)
  w=$(printf '%s\n' "$e" | grep -am1 '^WT_SESSION=' | cut -d= -f2)
  [ -n "$w" ] || continue
  printf '%s\n' "$e" | grep -aqE '^(HERDR_ENV|TMUX|STY|ZELLIJ)=' && continue
  s=$(sed 's/.*) //' "$d/stat" 2>/dev/null)
  [ -n "$s" ] || continue
  set -- $s
  [ "$6" -gt 0 ] 2>/dev/null || continue
  a=$(tr '\0\n' '\037 ' < "$d/cmdline" 2>/dev/null)
  c=$(readlink "$d/cwd" 2>/dev/null)
  g=$(ls -l "$d/fd" 2>/dev/null | grep -o '/tmp/claude-[0-9]*/[^/ ]*/[0-9a-f][0-9a-f-]*' | head -1)
  if [ -n "$g" ]; then
    enc=${g%/*}; enc=${enc##*/}
    uid=${g##*/}
    t=""
    for r in "$HOME/.claude" "$HOME"/.claude-profiles/*; do
      f="$r/projects/$enc/$uid.jsonl"
      if [ -s "$f" ]; then t="$f"; break; fi
    done
    [ -n "$t" ] || uid=""
    g="$uid"
  fi
  printf '%s\036%s\036%s\036%s\036%s\036%s\036%s\036%s\n' "$w" "$p" "$2" "$3" "$6" "$c" "$g" "$a"
done
)SH";

        // Runs `wsl.exe -d <distro> -- sh -s`, writes the script to its stdin
        // and reads stdout to EOF.
        //
        // The pipes, the CREATE_NO_WINDOW and the polled read with a deadline
        // all live in RunProcessCapture now: the integration fetch pipeline
        // needs exactly the same thing for its `command` steps, and two copies
        // of that would drift.
        std::string RunWslProbe(const std::wstring& distro, DWORD timeoutMs)
        {
            return ::TerminalApp::RunProcessCapture(fmt::format(LR"(wsl.exe -d {} -- sh -s)", distro), WslProbeScript, timeoutMs);
        }

        struct WslRecord
        {
            std::wstring Session;
            long Pid{};
            long ParentPid{};
            long Pgrp{};
            long Tpgid{};
            std::wstring Cwd;
            std::wstring AgentSessionId;
            std::vector<std::wstring> Argv;
        };

        long ToLong(const std::wstring& text) noexcept
        {
            try
            {
                return text.empty() ? 0 : std::stol(text);
            }
            catch (...)
            {
                return 0;
            }
        }

        std::vector<WslRecord> ParseWslProbe(const std::string& output)
        {
            std::vector<WslRecord> records;
            const auto wide = til::u8u16(output);

            std::wistringstream lines{ wide };
            std::wstring line;
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == L'\r')
                {
                    line.pop_back();
                }
                const auto fields = SplitOn(line, FieldSep);
                if (fields.size() < 8)
                {
                    continue;
                }

                WslRecord record;
                record.Session = fields[0];
                record.Pid = ToLong(fields[1]);
                record.ParentPid = ToLong(fields[2]);
                record.Pgrp = ToLong(fields[3]);
                record.Tpgid = ToLong(fields[4]);
                record.Cwd = fields[5];
                record.AgentSessionId = fields[6];
                for (auto& arg : SplitOn(fields[7], ArgSep))
                {
                    if (!arg.empty())
                    {
                        record.Argv.push_back(std::move(arg));
                    }
                }
                if (!record.Argv.empty())
                {
                    records.push_back(std::move(record));
                }
            }
            return records;
        }
    }

    std::wstring_view BaseName(std::wstring_view argv0) noexcept
    {
        if (const auto slash = argv0.find_last_of(L"/\\"); slash != std::wstring_view::npos)
        {
            argv0 = argv0.substr(slash + 1);
        }
        if (const auto dot = argv0.find_last_of(L'.'); dot != std::wstring_view::npos && dot != 0)
        {
            const auto ext = argv0.substr(dot + 1);
            if (til::equals_insensitive_ascii(ext, L"exe") || til::equals_insensitive_ascii(ext, L"cmd") ||
                til::equals_insensitive_ascii(ext, L"bat") || til::equals_insensitive_ascii(ext, L"ps1"))
            {
                argv0 = argv0.substr(0, dot);
            }
        }
        return argv0;
    }

    const AgentRow* FindAgent(std::wstring_view name) noexcept
    {
        for (const auto& row : AgentTable)
        {
            if (til::equals_insensitive_ascii(name, row.Name))
            {
                return &row;
            }
        }
        return nullptr;
    }

    bool IsMultiplexer(std::wstring_view name) noexcept
    {
        return NameIn(name, MultiplexerNames.data(), MultiplexerNames.size());
    }

    std::vector<std::wstring> StripSessionSelectors(const std::vector<std::wstring>& argv)
    {
        std::vector<std::wstring> stripped;
        stripped.reserve(argv.size());

        auto skipValue = false;
        for (size_t i = 0; i < argv.size(); ++i)
        {
            const auto& arg = argv[i];
            if (skipValue)
            {
                skipValue = false;
                continue;
            }

            if (arg == L"--resume" || arg == L"-r" || arg == L"--session" || arg == L"--thread" || arg == L"--conversation")
            {
                skipValue = true;
                continue;
            }
            if (arg == L"--continue" ||
                arg.starts_with(L"--resume=") || arg.starts_with(L"--session=") ||
                arg.starts_with(L"--thread=") || arg.starts_with(L"--conversation="))
            {
                continue;
            }
            // codex resumes through a subcommand, and only in first position.
            // Its session id follows as a bare argument, so dropping the
            // subcommand alone would leave the id behind as codex's first
            // positional and change what the command means.
            if (i == 1 && arg == L"resume")
            {
                skipValue = i + 1 < argv.size() && !argv[i + 1].starts_with(L'-');
                continue;
            }
            stripped.push_back(arg);
        }
        return stripped;
    }

    namespace
    {
        // The session name a multiplexer was started with, if it names one.
        // tmux and zellij spell it -s, screen -S, and any of them may take
        // --session; -t is how tmux and screen name a target to attach to.
        std::wstring SessionNameIn(const std::vector<std::wstring>& argv)
        {
            for (size_t i = 1; i < argv.size(); ++i)
            {
                const auto& arg = argv[i];
                if ((arg == L"-s" || arg == L"-S" || arg == L"-t" || arg == L"--session") && i + 1 < argv.size())
                {
                    return argv[i + 1];
                }
                for (const std::wstring_view prefix : { L"--session=", L"-s=", L"-S=" })
                {
                    if (arg.starts_with(prefix))
                    {
                        return arg.substr(prefix.size());
                    }
                }
            }
            return {};
        }

        // How to get BACK into a multiplexer, which is not the command that
        // started it. Replaying the launch verbatim is actively wrong: `tmux
        // new-session -s demo` answers "duplicate session: demo" and leaves
        // the pane at a prompt with the session still detached.
        //
        // shefrd and herdr are the exception -- their client attaches to the
        // running server on its own, so the bare command is already the right
        // answer. The rest need an explicit attach, spelled differently by
        // each. The `||` fallbacks are safe because this is typed into the
        // pane's own shell: if there is nothing to attach to, because the
        // server died with the machine, a fresh session is the sane outcome.
        std::wstring MultiplexerResume(std::wstring_view name, const std::vector<std::wstring>& argv)
        {
            const auto session = SessionNameIn(argv);

            if (til::equals_insensitive_ascii(name, L"tmux"))
            {
                return session.empty() ? L"tmux attach || tmux" :
                                         fmt::format(FMT_COMPILE(L"tmux new-session -A -s {}"), QuoteIfNeeded(session));
            }
            if (til::equals_insensitive_ascii(name, L"screen"))
            {
                // -R reattaches if it can and creates if it cannot, which is
                // exactly the semantic wanted here.
                return session.empty() ? L"screen -R" :
                                         fmt::format(FMT_COMPILE(L"screen -R {}"), QuoteIfNeeded(session));
            }
            if (til::equals_insensitive_ascii(name, L"zellij"))
            {
                return session.empty() ? L"zellij attach || zellij" :
                                         fmt::format(FMT_COMPILE(L"zellij attach -c {}"), QuoteIfNeeded(session));
            }

            // shefrd, herdr: the client attaches by itself.
            return std::wstring{ name };
        }
    }

    bool ResumesAgentSession(std::wstring_view commandLine) noexcept
    {
        // The program is the first whitespace-delimited token; BuildPlan never
        // quotes it, because a program name that needed quoting would not have
        // matched a table row in the first place.
        const auto end = commandLine.find_first_of(L" \t");
        if (end == std::wstring_view::npos)
        {
            // No arguments at all, so no selector either -- a bare multiplexer
            // or a plain replay.
            return false;
        }

        // Matched against Program as well as Name, because a row may rename
        // what it runs: `cursor` is resumed by invoking `cursor-agent`.
        const auto program = BaseName(commandLine.substr(0, end));
        for (const auto& row : AgentTable)
        {
            if (til::equals_insensitive_ascii(program, row.Name) ||
                til::equals_insensitive_ascii(program, row.Program))
            {
                return commandLine.find(row.Selector) != std::wstring_view::npos;
            }
        }
        return false;
    }

    std::optional<ResumePlan> BuildPlan(const CapturedPane& captured, const Policy& policy)
    {
        if (captured.Argv.empty())
        {
            return std::nullopt;
        }

        // `node /path/to/claude` is claude, not node. Owned rather than a view,
        // because the interpreter gets dropped from the argv below and that
        // would leave a view dangling into a vector that has moved on.
        const auto& original = captured.Argv;
        auto name = std::wstring{ BaseName(original[0]) };
        auto hostLaunched = false;
        if (NameIn(name, ScriptHosts, std::size(ScriptHosts)) && original.size() > 1 && !original[1].starts_with(L'-'))
        {
            name = std::wstring{ BaseName(original[1]) };
            hostLaunched = true;
        }

        const auto matches = [&](const std::vector<std::wstring>& list) {
            return std::any_of(list.begin(), list.end(), [&](const auto& entry) {
                return til::equals_insensitive_ascii(name, BaseName(entry));
            });
        };

        // An exclusion beats every other reason to bring something back,
        // including an explicit extra entry -- it is the only way to say "not
        // this one" about a program the built-in lists already cover.
        if (matches(policy.Excluded))
        {
            return std::nullopt;
        }

        const auto* agent = FindAgent(name);
        const auto multiplexer = IsMultiplexer(name);
        const auto extra = matches(policy.Extra);

        const auto allowed = (agent && policy.Agents) || (multiplexer && policy.Multiplexers) || extra;
        if (!allowed)
        {
            return std::nullopt;
        }

        // A multiplexer's daemon outlives us and still owns its shells, so the
        // bare command reattaches. Anything the user added by hand replays as
        // it was: we know nothing about its session model.
        // Getting back into a multiplexer is an attach, not a re-launch.
        if (multiplexer)
        {
            return ResumePlan{ MultiplexerResume(name, original), false };
        }

        if (!agent || captured.AgentSessionId.empty())
        {
            // Replayed exactly as found, interpreter and all: we are not
            // rewriting a command we have no table row for.
            return ResumePlan{ JoinArgv(original), false };
        }

        auto argv = original;
        if (hostLaunched)
        {
            // Drop the interpreter. The row names a program to run, and
            // `claude /usr/lib/claude --foo` would run neither.
            argv.erase(argv.begin());
        }

        auto rebuilt = StripSessionSelectors(argv);
        if (rebuilt.empty())
        {
            return std::nullopt;
        }
        // Run the program the row names, keeping every other flag the user
        // launched it with -- `claude --dangerously-skip-permissions` must come
        // back with that flag, not as a bare `claude`.
        rebuilt[0] = std::wstring{ agent->Program };

        switch (agent->Style)
        {
        // A subcommand lands in the same place a flag does -- after everything
        // the command was already carrying. That is what keeps a global flag
        // applying to it: `codex --yolo resume <id>`, which is the order codex
        // wants, rather than `codex resume <id> --yolo`. The two styles differ
        // in the token they add, not in where it goes; only StripSessionSelectors
        // has to tell them apart, because an old subcommand is removed
        // differently from an old flag.
        case SelectorStyle::FlagSpace:
        case SelectorStyle::Subcommand:
            rebuilt.emplace_back(agent->Selector);
            rebuilt.push_back(captured.AgentSessionId);
            break;
        case SelectorStyle::FlagEquals:
            rebuilt.push_back(std::wstring{ agent->Selector } + L"=" + captured.AgentSessionId);
            break;
        }

        return ResumePlan{ JoinArgv(rebuilt), true };
    }

    std::vector<CapturedPane> Capture(const std::vector<PaneProbe>& panes)
    {
        std::vector<CapturedPane> captured;
        if (panes.empty())
        {
            return captured;
        }

        // --- WSL panes: one probe per distinct distro, never per pane.
        std::vector<std::wstring> distros;
        for (const auto& pane : panes)
        {
            if (!pane.Distro.empty() && std::find(distros.begin(), distros.end(), pane.Distro) == distros.end())
            {
                distros.push_back(pane.Distro);
            }
        }

        std::vector<WslRecord> wslRecords;
        for (const auto& distro : distros)
        {
            const auto output = RunWslProbe(distro, 8000);
            auto parsed = ParseWslProbe(output);
            wslRecords.insert(wslRecords.end(), std::make_move_iterator(parsed.begin()), std::make_move_iterator(parsed.end()));
        }

        for (const auto& pane : panes)
        {
            if (pane.Distro.empty())
            {
                continue;
            }

            std::vector<const WslRecord*> mine;
            for (const auto& record : wslRecords)
            {
                if (til::equals_insensitive_ascii(record.Session, pane.SessionId))
                {
                    mine.push_back(&record);
                }
            }
            if (mine.empty())
            {
                continue;
            }

            // The shell is the one process in the set whose parent isn't also
            // in it -- every descendant inherited the same WT_SESSION.
            const WslRecord* shell = nullptr;
            for (const auto* record : mine)
            {
                const auto parentInSet = std::any_of(mine.begin(), mine.end(), [&](const auto* other) { return other->Pid == record->ParentPid; });
                if (!parentInSet && (!shell || record->Pid < shell->Pid))
                {
                    shell = record;
                }
            }
            if (!shell || shell->Tpgid <= 0 || shell->Tpgid == shell->Pgrp)
            {
                // Nothing in the foreground but the shell itself.
                continue;
            }

            const WslRecord* foreground = nullptr;
            for (const auto* record : mine)
            {
                if (record->Pgrp == shell->Tpgid && record->Pid != shell->Pid)
                {
                    // Prefer the group leader; a pipeline puts several
                    // processes in one group and the leader is the one typed.
                    if (!foreground || record->Pid == record->Pgrp)
                    {
                        foreground = record;
                    }
                }
            }
            if (!foreground)
            {
                continue;
            }

            captured.push_back(CapturedPane{ pane.SessionId, foreground->Argv, foreground->AgentSessionId, {} });
        }

        // --- native panes
        const auto anyNative = std::any_of(panes.begin(), panes.end(), [](const auto& pane) { return pane.Distro.empty(); });
        if (anyNative)
        {
            const auto all = SnapshotProcesses();
            std::map<std::wstring, std::vector<std::wstring>> transcriptsByCwd;

            for (const auto& pane : panes)
            {
                if (!pane.Distro.empty() || pane.RootPid == 0)
                {
                    continue;
                }
                const auto worker = FirstWorkerBelow(pane.RootPid, all);
                if (!worker)
                {
                    continue;
                }
                auto argv = SplitCommandLine(CommandLineOf(*worker));
                if (argv.empty())
                {
                    continue;
                }

                CapturedPane found{ pane.SessionId, std::move(argv), {}, {} };

                auto name = BaseName(found.Argv[0]);
                if (NameIn(name, ScriptHosts, std::size(ScriptHosts)) && found.Argv.size() > 1 && !found.Argv[1].starts_with(L'-'))
                {
                    name = BaseName(found.Argv[1]);
                }

                // No /proc here, so the session id is the newest transcript
                // recorded for this directory. Panes sharing a cwd take
                // successive transcripts rather than all claiming the newest
                // one -- still a guess, but not a guess that collides.
                if (til::equals_insensitive_ascii(name, L"claude") && !pane.Cwd.empty())
                {
                    const auto entry = transcriptsByCwd.find(pane.Cwd);
                    if (entry == transcriptsByCwd.end())
                    {
                        transcriptsByCwd.emplace(pane.Cwd, TranscriptsForCwd(pane.Cwd));
                    }
                    auto& pool = transcriptsByCwd[pane.Cwd];
                    if (!pool.empty())
                    {
                        found.AgentSessionId = pool.front();
                        pool.erase(pool.begin());
                    }
                }

                captured.push_back(std::move(found));
            }
        }

        return captured;
    }
}
