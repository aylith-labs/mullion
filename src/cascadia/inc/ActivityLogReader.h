// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Reads activity.jsonl back. The writer is ActivityLog.h in WinRTUtils; this is
// the other half.
//
// Header-only on purpose. There are two viewers - the pane in TerminalApp and
// the Settings page in Microsoft.Terminal.Settings.Editor - and they live in
// different DLLs. Putting the parse in WinRTUtils would work for those two but
// would also compile Windows.Data.Json into ShellExtension, which links the same
// static library and runs inside Explorer. Everything here is a pure function
// over its arguments, so a per-module copy costs nothing and shares nothing:
// unlike the writer's enable flag, there is no state to get duplicated. That
// distinction is why the flag had to become a file and this does not.

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <ActivityLog.h>
#include <til/io.h>
#include <til/u8u16convert.h>
#include <winrt/Windows.Data.Json.h>

namespace Microsoft::Terminal::ActivityLog
{
    // One record read back, as plain strings. Numbers included: pid and
    // parentPid are only ever displayed, and carrying them as text keeps every
    // field one type and the filter one substring test.
    struct ReadEntry
    {
        std::wstring timestamp;
        std::wstring kind;
        std::wstring exe;
        std::wstring exeName; // the leaf of exe, for a list headline
        std::wstring commandLine;
        std::wstring cwd;
        std::wstring parentExe;
        std::wstring reason;
        std::wstring pid;
        std::wstring parentPid;

        // controlPipe only: which op, which pane, and how it ended.
        std::wstring op;
        std::wstring target;
        std::wstring result;

        // Every field above, lowercased and joined, so a filter box is a single
        // substring test rather than the reader having to remember which field
        // a half-remembered fragment was in.
        std::wstring searchText;
    };

    namespace details
    {
        inline std::wstring StringField(const winrt::Windows::Data::Json::JsonObject& obj, std::wstring_view name)
        {
            if (!obj.HasKey(name))
            {
                return {};
            }
            const auto value{ obj.GetNamedValue(name) };
            switch (value.ValueType())
            {
            case winrt::Windows::Data::Json::JsonValueType::String:
                return std::wstring{ value.GetString() };
            case winrt::Windows::Data::Json::JsonValueType::Number:
                // As an integer, not GetNumber()'s double: 4242, not 4242.000000.
                return std::to_wstring(static_cast<int64_t>(value.GetNumber()));
            default:
                return {};
            }
        }
    }

    // Parses one line. Returns false for a line that is not a JSON object - a
    // torn final line from a process that died mid-append, which an append-only
    // log should skip rather than refuse to open for.
    inline bool ParseLine(std::string_view line, ReadEntry& out)
    {
        winrt::Windows::Data::Json::JsonObject obj{ nullptr };
        if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::hstring{ til::u8u16(line) }, obj) || !obj)
        {
            return false;
        }

        out.timestamp = details::StringField(obj, L"ts");
        out.kind = details::StringField(obj, L"kind");
        out.exe = details::StringField(obj, L"exe");
        out.commandLine = details::StringField(obj, L"commandLine");
        out.cwd = details::StringField(obj, L"cwd");
        out.parentExe = details::StringField(obj, L"parentExe");
        out.reason = details::StringField(obj, L"reason");
        out.pid = details::StringField(obj, L"pid");
        out.parentPid = details::StringField(obj, L"parentPid");
        out.op = details::StringField(obj, L"op");
        out.target = details::StringField(obj, L"target");
        out.result = details::StringField(obj, L"result");

        // Falls back to the whole string when there is no separator, so a bare
        // "wscript.exe" still shows something.
        const auto slash{ out.exe.find_last_of(L"\\/") };
        out.exeName = slash == std::wstring::npos ? out.exe : out.exe.substr(slash + 1);

        out.searchText.clear();
        for (const auto* part : { &out.timestamp, &out.kind, &out.exe, &out.commandLine, &out.cwd, &out.parentExe, &out.reason, &out.pid, &out.parentPid, &out.op, &out.target, &out.result })
        {
            out.searchText.append(*part);
            out.searchText.push_back(L' ');
        }
        std::transform(out.searchText.begin(), out.searchText.end(), out.searchText.begin(), [](wchar_t c) { return til::tolower_ascii(c); });

        return true;
    }

    // Newest first, at most maxEntries. Reads the rotated generation first so the
    // combined view runs in file order before it is reversed.
    //
    // Never throws: a viewer asking "what happened" should not itself fail. An
    // unreadable log reads as an empty one, which the caller distinguishes from
    // a genuinely empty log by asking Enabled().
    inline std::vector<ReadEntry> ReadRecent(size_t maxEntries) noexcept
    try
    {
        std::vector<ReadEntry> entries;

        const auto path{ Path() };
        if (path.empty())
        {
            return entries;
        }

        auto rotated{ path };
        rotated.replace_filename(L"activity.1.jsonl");

        std::string contents{ til::io::read_file_as_utf8_string_if_exists(rotated) };
        contents.append(til::io::read_file_as_utf8_string_if_exists(path));
        if (contents.empty())
        {
            return entries;
        }

        std::vector<ReadEntry> parsed;
        size_t pos{ 0 };
        while (pos < contents.size())
        {
            auto end{ contents.find('\n', pos) };
            if (end == std::string::npos)
            {
                end = contents.size();
            }
            auto line{ std::string_view{ contents }.substr(pos, end - pos) };
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                ReadEntry entry;
                if (ParseLine(line, entry))
                {
                    parsed.push_back(std::move(entry));
                }
            }
            pos = end + 1;
        }

        // Newest first: the reason anyone opens this is something that just
        // happened.
        entries.assign(std::make_move_iterator(parsed.rbegin()), std::make_move_iterator(parsed.rend()));
        if (entries.size() > maxEntries)
        {
            entries.resize(maxEntries);
        }
        return entries;
    }
    catch (...)
    {
        return {};
    }
}
