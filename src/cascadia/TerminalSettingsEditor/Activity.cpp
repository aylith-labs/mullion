// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Activity.h"
#include "Activity.g.cpp"
#include "ActivityViewModel.g.cpp"
#include "ActivityEntryViewModel.g.cpp"

#include <array>
#include <shellapi.h>
#include <utility>

using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // Two different caps, because they bound two different things.
    //
    // MaxRows is what the list realizes. It is an ItemsControl in the page's own
    // scroll flow rather than a ListView with a viewport of its own, so every row
    // it is given is realized, and 200 is already more than fits on a screen.
    //
    // MaxRecords is how much of the file is read and searched. Grouping is only
    // worth having if it has something to collapse - the interesting case is the
    // same handoff repeating hundreds of times - so the reader is given far more
    // than the list will ever draw, and one grouped row can stand for all of it.
    // Parsing is a per-line JSON parse with no locale calls; the derived work
    // that does cost a locale call happens once per reload, not per keystroke.
    static constexpr size_t MaxRows{ 200 };
    static constexpr size_t MaxRecords{ 2000 };

    namespace
    {
        namespace AL = ::Microsoft::Terminal::ActivityLog;

        // The order the event-type dropdown lists them in, which is also what
        // KindChoiceIndex indexes: 0 is every kind, 1 through 6 are these. The
        // dropdown's items are spelled out in Activity.xaml in the same order,
        // and nothing but this comment keeps the two in step.
        constexpr std::array<std::wstring_view, 6> KindOrder{
            AL::Kind::ProfileLaunch,
            AL::Kind::Handoff,
            AL::Kind::Helper,
            AL::Kind::Probe,
            AL::Kind::ShellVerb,
            AL::Kind::Elevate,
        };

        // What a row stands for. Collapsing identical command lines is the one
        // that pays for the whole feature: a log whose hundred entries are the
        // same scheduled task every five minutes becomes one row with a count.
        // The other three are summaries of the same shape - what has this
        // terminal been running, by program, by kind of event, by day.
        enum class GroupMode
        {
            None = 0,
            Command = 1,
            Program = 2,
            EventType = 3,
            Day = 4,
        };
        constexpr int32_t LastGroupMode{ 4 };

        bool _parseTimestamp(const std::wstring& value, SYSTEMTIME& utc) noexcept
        {
            int year{}, month{}, day{}, hour{}, minute{}, second{}, milliseconds{};
            if (swscanf_s(value.c_str(), L"%d-%d-%dT%d:%d:%d.%dZ", &year, &month, &day, &hour, &minute, &second, &milliseconds) != 7)
            {
                return false;
            }
            if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 ||
                hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 ||
                milliseconds < 0 || milliseconds > 999)
            {
                return false;
            }
            utc.wYear = gsl::narrow_cast<WORD>(year);
            utc.wMonth = gsl::narrow_cast<WORD>(month);
            utc.wDay = gsl::narrow_cast<WORD>(day);
            utc.wHour = gsl::narrow_cast<WORD>(hour);
            utc.wMinute = gsl::narrow_cast<WORD>(minute);
            utc.wSecond = gsl::narrow_cast<WORD>(second);
            utc.wMilliseconds = gsl::narrow_cast<WORD>(milliseconds);
            FILETIME fileTime{};
            return SystemTimeToFileTime(&utc, &fileTime);
        }

        struct LocalStamp
        {
            std::wstring date;
            std::wstring dateTime;
        };

        // The date is returned separately because it is also the key "group by
        // day" groups on, and asking the locale for it twice per record is the
        // expensive half of reading the log.
        LocalStamp _localStamp(const std::wstring& timestamp)
        {
            SYSTEMTIME utc{};
            SYSTEMTIME local{};
            std::array<wchar_t, 128> date{};
            std::array<wchar_t, 128> time{};
            if (_parseTimestamp(timestamp, utc) &&
                SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) &&
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date.data(), gsl::narrow_cast<int>(date.size()), nullptr) &&
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, nullptr, time.data(), gsl::narrow_cast<int>(time.size())))
            {
                const std::wstring_view dateView{ date.data() };
                const std::wstring_view timeView{ time.data() };
                return { std::wstring{ dateView }, fmt::format(FMT_COMPILE(L"{} {}"), dateView, timeView) };
            }

            // A timestamp that will not parse still has to group and display
            // somewhere, so fall back to the raw ISO value and its date prefix
            // rather than to nothing. A torn line is the reader's problem; a
            // clock that wrote something odd is not worth losing the row over.
            return { timestamp.substr(0, std::min<size_t>(timestamp.size(), 10)), timestamp };
        }

        std::wstring _relativeTime(const std::wstring& timestamp)
        {
            SYSTEMTIME utc{};
            FILETIME then{};
            if (!_parseTimestamp(timestamp, utc) || !SystemTimeToFileTime(&utc, &then))
            {
                return timestamp;
            }

            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER thenTicks{};
            thenTicks.LowPart = then.dwLowDateTime;
            thenTicks.HighPart = then.dwHighDateTime;
            ULARGE_INTEGER nowTicks{};
            nowTicks.LowPart = now.dwLowDateTime;
            nowTicks.HighPart = now.dwHighDateTime;
            const auto seconds{ nowTicks.QuadPart > thenTicks.QuadPart ? (nowTicks.QuadPart - thenTicks.QuadPart) / 10'000'000 : 0ull };
            if (seconds < 60)
            {
                return std::wstring{ RS_(L"Activity_JustNow/Text") };
            }
            if (seconds < 3600)
            {
                const auto minutes{ seconds / 60 };
                return minutes == 1 ? std::wstring{ RS_(L"Activity_MinuteAgo/Text") } : RS_fmt(L"Activity_MinutesAgo/Text", minutes);
            }
            if (seconds < 86400)
            {
                const auto hours{ seconds / 3600 };
                return hours == 1 ? std::wstring{ RS_(L"Activity_HourAgo/Text") } : RS_fmt(L"Activity_HoursAgo/Text", hours);
            }
            const auto days{ seconds / 86400 };
            return days == 1 ? std::wstring{ RS_(L"Activity_DayAgo/Text") } : RS_fmt(L"Activity_DaysAgo/Text", days);
        }

        std::pair<winrt::hstring, winrt::hstring> _eventText(const std::wstring& kind)
        {
            const std::wstring_view k{ kind };
            if (k == AL::Kind::ProfileLaunch)
            {
                return { RS_(L"Activity_KindProfileLaunch/Text"), RS_(L"Activity_KindProfileLaunchTip/Text") };
            }
            if (k == AL::Kind::Handoff)
            {
                return { RS_(L"Activity_KindHandoff/Text"), RS_(L"Activity_KindHandoffTip/Text") };
            }
            if (k == AL::Kind::Helper)
            {
                return { RS_(L"Activity_KindHelper/Text"), RS_(L"Activity_KindHelperTip/Text") };
            }
            if (k == AL::Kind::Probe)
            {
                return { RS_(L"Activity_KindProbe/Text"), RS_(L"Activity_KindProbeTip/Text") };
            }
            if (k == AL::Kind::ShellVerb)
            {
                return { RS_(L"Activity_KindShellVerb/Text"), RS_(L"Activity_KindShellVerbTip/Text") };
            }
            if (k == AL::Kind::Elevate)
            {
                return { RS_(L"Activity_KindElevate/Text"), RS_(L"Activity_KindElevateTip/Text") };
            }

            // A kind this build has no name for is still worth showing by its own
            // name: the file is written by whichever terminal ran, which may be a
            // newer one than the one reading it.
            return { kind.empty() ? RS_(L"Activity_KindUnknown/Text") : winrt::hstring{ kind }, RS_(L"Activity_KindUnknownTip/Text") };
        }

        // Opens Explorer with the log selected. Explorer is a GUI process, so
        // this is not the console-delegation case Invoke-Hidden.vbs exists for -
        // there is no console to hand off, and a visible window is the point.
        void _revealInExplorer(const std::wstring& path)
        {
            const std::filesystem::path file{ path };
            std::error_code error;
            const auto selectable{ std::filesystem::exists(file, error) && !error };

            // Recording may never have run, in which case there is nothing to
            // select and /select on a missing path opens the user's documents
            // folder rather than saying so. Show the folder it would be written
            // to instead, which answers the same question.
            const auto args{ selectable ?
                                 fmt::format(L"/select,\"{}\"", path) :
                                 fmt::format(L"\"{}\"", file.parent_path().wstring()) };

            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.lpVerb = nullptr;
            info.lpFile = L"explorer.exe";
            info.lpParameters = args.c_str();
            info.nShow = SW_SHOWNORMAL;
            LOG_LAST_ERROR_IF(!ShellExecuteExW(&info));
        }
    }

    winrt::com_ptr<ActivityEntryViewModel> ActivityEntryViewModel::From(const ActivityGroup& group, const ActivityEntryOptions& options)
    {
        auto entry{ winrt::make_self<ActivityEntryViewModel>() };
        if (!group.newest || !group.newestDerived)
        {
            return entry;
        }
        const auto& read{ *group.newest };
        const auto& derived{ *group.newestDerived };

        entry->_timestampDisplay = winrt::hstring{ options.useAbsoluteTime ? derived.absoluteTime : _relativeTime(read.timestamp) };
        entry->_timestampTooltip = winrt::hstring{ derived.absoluteTime };

        entry->_isGroup = group.count > 1;
        if (entry->_isGroup)
        {
            entry->_countText = winrt::hstring{ RS_fmt(L"Activity_Repeats/Text", group.count) };

            // A group whose members all landed in the same second - which the
            // clock's resolution makes possible - would read "first 1:04:07 PM"
            // beside "1:04:07 PM", so say nothing rather than something circular.
            if (group.oldestTimestamp != read.timestamp)
            {
                const auto oldest{ options.useAbsoluteTime ? group.oldestAbsoluteTime : _relativeTime(group.oldestTimestamp) };
                entry->_rangeText = winrt::hstring{ RS_fmt(L"Activity_FirstSeen/Text", oldest) };
            }
        }

        // A field the members of a group disagree about is not shown as though it
        // belonged to all of them. That is the same rule the writer follows when
        // it leaves a field it does not know empty: a wrong working directory is
        // worse than an absent one when the point is to identify a process.
        if (group.sameKind)
        {
            const auto [label, tip] = _eventText(read.kind);
            entry->_eventLabel = label;
            entry->_eventTooltip = tip;
        }
        else
        {
            entry->_eventLabel = RS_(L"Activity_MixedEvents/Text");
            entry->_eventTooltip = RS_(L"Activity_MixedEventsTip/Text");
        }

        entry->_exeName = group.sameProgram ? winrt::hstring{ read.exeName } : RS_(L"Activity_MixedPrograms/Text");
        entry->_commandLine = group.sameCommandLine ? winrt::hstring{ read.commandLine } : RS_(L"Activity_MixedCommands/Text");
        entry->_cwd = group.sameWorkingDirectory ? winrt::hstring{ read.cwd } : winrt::hstring{};
        entry->_parentExe = group.sameParent ? winrt::hstring{ read.parentExe } : winrt::hstring{};

        entry->_showCommandLineInline = options.showCommandLine && !entry->_commandLine.empty();
        entry->_showCommandLineInTooltip = !options.showCommandLine && !entry->_commandLine.empty();

        // The hover popover. Built here rather than out of a pile of bound
        // TextBlocks because it is prose, its lines are conditional, and a
        // tooltip that comes up empty is worse than no tooltip: the first two
        // lines always say something, so it never does.
        std::wstring summary;
        const auto appendLine = [&summary](const std::wstring& line) {
            if (line.empty())
            {
                return;
            }
            if (!summary.empty())
            {
                summary.push_back(L'\n');
            }
            summary.append(line);
        };

        if (entry->_isGroup)
        {
            appendLine(RS_fmt(L"Activity_HoverRange/Text", group.count, group.oldestAbsoluteTime, derived.absoluteTime));
        }
        else
        {
            appendLine(derived.absoluteTime);
        }
        appendLine(std::wstring{ entry->_eventLabel });
        if (group.sameProgram && read.exe != read.exeName)
        {
            appendLine(read.exe);
        }
        if (!entry->_cwd.empty())
        {
            appendLine(RS_fmt(L"Activity_HoverFolder/Text", std::wstring_view{ entry->_cwd }));
        }
        if (!entry->_parentExe.empty())
        {
            appendLine(RS_fmt(L"Activity_HoverParent/Text", std::wstring_view{ entry->_parentExe }));
        }
        if (!entry->_isGroup && !read.reason.empty())
        {
            appendLine(RS_fmt(L"Activity_HoverReason/Text", read.reason));
        }
        entry->_hoverSummary = winrt::hstring{ summary };

        return entry;
    }

    ActivityViewModel::ActivityViewModel(Model::CascadiaSettings settings) :
        _settings{ settings },
        _entries{ winrt::single_threaded_observable_vector<Editor::ActivityEntryViewModel>() },
        _programChoices{ winrt::single_threaded_observable_vector<winrt::hstring>() },
        _dayChoices{ winrt::single_threaded_observable_vector<winrt::hstring>() }
    {
        Reload();
    }

    void ActivityViewModel::Reload()
    {
        const auto path{ AL::Path() };
        _logPath = winrt::hstring{ path.wstring() };

        _all = AL::ReadRecent(MaxRecords);

        // Everything that costs a locale call, once per record per reload. The
        // filter box re-runs on every keystroke; asking the locale for a
        // formatted date a couple of thousand times per keystroke is the
        // difference between a filter that types and one that stutters.
        _derived.clear();
        _derived.reserve(_all.size());
        for (const auto& record : _all)
        {
            auto stamp{ _localStamp(record.timestamp) };
            ActivityDerived derived;
            derived.dayKey = std::move(stamp.date);
            derived.absoluteTime = std::move(stamp.dateTime);
            derived.programKey = record.exeName;
            std::transform(derived.programKey.begin(), derived.programKey.end(), derived.programKey.begin(), [](wchar_t c) { return til::tolower_ascii(c); });
            _derived.push_back(std::move(derived));
        }

        _rebuildChoices();
        _applyFilter();

        _NotifyChanges(L"LogPath", L"HasLogPath");
    }

    void ActivityViewModel::_rebuildChoices()
    {
        // Remember what was chosen by its key rather than its index. The lists
        // are rebuilt from whatever the log now holds, so a program that has
        // stopped appearing shifts every index after it.
        const auto previousProgram{ _programChoiceIndex > 0 && static_cast<size_t>(_programChoiceIndex) < _programKeys.size() ? _programKeys[static_cast<size_t>(_programChoiceIndex)] : std::wstring{} };
        const auto previousDay{ _dayChoiceIndex > 0 && static_cast<size_t>(_dayChoiceIndex) < _dayKeys.size() ? _dayKeys[static_cast<size_t>(_dayChoiceIndex)] : std::wstring{} };

        // Programs in a std::map, so the dropdown is alphabetical: a list of
        // program names is read by looking one up. Days in the order they were
        // met, which is newest first, because that is the order the list itself
        // is in.
        std::map<std::wstring, std::pair<std::wstring, uint32_t>> programs;
        std::vector<std::wstring> dayOrder;
        std::unordered_map<std::wstring, uint32_t> dayCounts;
        for (size_t i = 0; i < _all.size(); ++i)
        {
            if (const auto& key{ _derived[i].programKey }; !key.empty())
            {
                auto& slot{ programs[key] };
                if (slot.second == 0)
                {
                    slot.first = _all[i].exeName;
                }
                ++slot.second;
            }
            if (const auto& day{ _derived[i].dayKey }; !day.empty())
            {
                const auto [it, inserted] = dayCounts.try_emplace(day, 0u);
                if (inserted)
                {
                    dayOrder.push_back(day);
                }
                ++it->second;
            }
        }

        // A ComboBox whose items are cleared reports SelectedIndex -1 back
        // through its TwoWay binding while this runs. Without the flag that
        // would read as the user choosing nothing, and would re-filter against
        // a list that is half rebuilt.
        _rebuildingChoices = true;

        _programChoices.Clear();
        _programKeys.clear();
        _programChoices.Append(RS_(L"Activity_AllPrograms/Text"));
        _programKeys.emplace_back();
        for (const auto& [key, value] : programs)
        {
            _programChoices.Append(winrt::hstring{ RS_fmt(L"Activity_ChoiceWithCount/Text", value.first, value.second) });
            _programKeys.push_back(key);
        }

        _dayChoices.Clear();
        _dayKeys.clear();
        _dayChoices.Append(RS_(L"Activity_AnyDay/Text"));
        _dayKeys.emplace_back();
        for (const auto& day : dayOrder)
        {
            _dayChoices.Append(winrt::hstring{ RS_fmt(L"Activity_ChoiceWithCount/Text", day, dayCounts[day]) });
            _dayKeys.push_back(day);
        }

        _rebuildingChoices = false;

        const auto restore = [](const std::vector<std::wstring>& keys, const std::wstring& previous) -> int32_t {
            if (previous.empty())
            {
                return 0;
            }
            const auto found{ std::find(keys.begin(), keys.end(), previous) };
            return found == keys.end() ? 0 : gsl::narrow_cast<int32_t>(found - keys.begin());
        };
        _programChoiceIndex = restore(_programKeys, previousProgram);
        _dayChoiceIndex = restore(_dayKeys, previousDay);

        // After the items exist, never before: this is what puts the selection
        // back into a ComboBox that was just emptied.
        _NotifyChanges(L"ProgramChoiceIndex", L"DayChoiceIndex");
    }

    void ActivityViewModel::Filter(const winrt::hstring& value)
    {
        if (_filter == value)
        {
            return;
        }
        _filter = value;
        _NotifyChanges(L"Filter");
        _applyFilter();
    }

    void ActivityViewModel::KindChoiceIndex(int32_t value)
    {
        // A ComboBox reports -1 when nothing is selected, which for a filter
        // means the same thing as its first item: everything.
        const auto clamped{ value < 0 || static_cast<size_t>(value) > KindOrder.size() ? 0 : value };
        if (_kindChoiceIndex == clamped)
        {
            return;
        }
        _kindChoiceIndex = clamped;
        _NotifyChanges(L"KindChoiceIndex");
        _applyFilter();
    }

    void ActivityViewModel::ProgramChoiceIndex(int32_t value)
    {
        if (_rebuildingChoices)
        {
            return;
        }
        const auto clamped{ value < 0 || static_cast<size_t>(value) >= _programKeys.size() ? 0 : value };
        if (_programChoiceIndex == clamped)
        {
            return;
        }
        _programChoiceIndex = clamped;
        _NotifyChanges(L"ProgramChoiceIndex");
        _applyFilter();
    }

    void ActivityViewModel::DayChoiceIndex(int32_t value)
    {
        if (_rebuildingChoices)
        {
            return;
        }
        const auto clamped{ value < 0 || static_cast<size_t>(value) >= _dayKeys.size() ? 0 : value };
        if (_dayChoiceIndex == clamped)
        {
            return;
        }
        _dayChoiceIndex = clamped;
        _NotifyChanges(L"DayChoiceIndex");
        _applyFilter();
    }

    void ActivityViewModel::GroupModeIndex(int32_t value)
    {
        const auto clamped{ value < 0 || value > LastGroupMode ? 0 : value };
        if (_groupModeIndex == clamped)
        {
            return;
        }
        _groupModeIndex = clamped;
        _NotifyChanges(L"GroupModeIndex");
        _applyFilter();
    }

    void ActivityViewModel::UseAbsoluteTime(bool value)
    {
        if (_useAbsoluteTime == value)
        {
            return;
        }
        _useAbsoluteTime = value;
        _NotifyChanges(L"UseAbsoluteTime");
        _applyFilter();
    }

    void ActivityViewModel::ShowCommandLines(bool value)
    {
        if (_showCommandLines == value)
        {
            return;
        }
        _showCommandLines = value;
        _NotifyChanges(L"ShowCommandLines");

        // The rows carry the choice rather than reading it: an entry view model
        // raises no PropertyChanged of its own, so the list is rebuilt.
        _applyFilter();
    }

    void ActivityViewModel::_applyFilter()
    {
        std::wstring needle{ _filter };
        std::transform(needle.begin(), needle.end(), needle.begin(), [](wchar_t c) { return til::tolower_ascii(c); });

        const std::wstring_view kind{ _kindChoiceIndex > 0 && static_cast<size_t>(_kindChoiceIndex) <= KindOrder.size() ? KindOrder[static_cast<size_t>(_kindChoiceIndex) - 1] : std::wstring_view{} };
        std::wstring program;
        if (_programChoiceIndex > 0 && static_cast<size_t>(_programChoiceIndex) < _programKeys.size())
        {
            program = _programKeys[static_cast<size_t>(_programChoiceIndex)];
        }
        std::wstring day;
        if (_dayChoiceIndex > 0 && static_cast<size_t>(_dayChoiceIndex) < _dayKeys.size())
        {
            day = _dayKeys[static_cast<size_t>(_dayChoiceIndex)];
        }

        const auto mode{ static_cast<GroupMode>(_groupModeIndex) };
        std::vector<ActivityGroup> groups;
        std::unordered_map<std::wstring, size_t> byKey;
        size_t matched{ 0 };

        for (size_t i = 0; i < _all.size(); ++i)
        {
            const auto& record{ _all[i] };
            const auto& derived{ _derived[i] };

            if (!kind.empty() && std::wstring_view{ record.kind } != kind)
            {
                continue;
            }
            if (!program.empty() && derived.programKey != program)
            {
                continue;
            }
            if (!day.empty() && derived.dayKey != day)
            {
                continue;
            }
            if (!needle.empty() && record.searchText.find(needle) == std::wstring::npos)
            {
                continue;
            }

            ++matched;

            // _all is newest first, so the first member of a group that is met is
            // its newest and the last is its oldest. That is what lets the range
            // be accumulated without sorting anything.
            const auto open{ [&]() {
                auto& group{ groups.emplace_back() };
                group.newest = &record;
                group.newestDerived = &derived;
                group.oldestTimestamp = record.timestamp;
                group.oldestAbsoluteTime = derived.absoluteTime;
                group.count = 1;
            } };

            if (mode == GroupMode::None)
            {
                if (groups.size() < MaxRows)
                {
                    open();
                }
                continue;
            }

            std::wstring key;
            switch (mode)
            {
            case GroupMode::Command:
                // The kind and the image path are part of the identity: the same
                // command line reached two different ways is two different things
                // to explain. \n cannot occur in any of the three, so joining on
                // it cannot make two different triples collide.
                key.reserve(record.kind.size() + record.exe.size() + record.commandLine.size() + 2);
                key.append(record.kind);
                key.push_back(L'\n');
                key.append(record.exe);
                key.push_back(L'\n');
                key.append(record.commandLine);
                break;
            case GroupMode::Program:
                key = derived.programKey;
                break;
            case GroupMode::EventType:
                key = record.kind;
                break;
            case GroupMode::Day:
                key = derived.dayKey;
                break;
            case GroupMode::None:
            default:
                break;
            }

            const auto found{ byKey.find(key) };
            if (found == byKey.end())
            {
                // The row cap stops new groups being opened, not existing ones
                // being counted: a group already on screen keeps accumulating, so
                // its count stays true even once the cap is reached.
                if (groups.size() < MaxRows)
                {
                    byKey.emplace(std::move(key), groups.size());
                    open();
                }
                continue;
            }

            auto& group{ groups[found->second] };
            ++group.count;
            group.oldestTimestamp = record.timestamp;
            group.oldestAbsoluteTime = derived.absoluteTime;
            group.sameKind = group.sameKind && group.newest->kind == record.kind;
            group.sameProgram = group.sameProgram && group.newestDerived->programKey == derived.programKey;
            group.sameCommandLine = group.sameCommandLine && group.newest->commandLine == record.commandLine;
            group.sameWorkingDirectory = group.sameWorkingDirectory && group.newest->cwd == record.cwd;
            group.sameParent = group.sameParent && group.newest->parentExe == record.parentExe;
        }

        const ActivityEntryOptions options{ _useAbsoluteTime, _showCommandLines };
        _entries.Clear();
        for (const auto& group : groups)
        {
            _entries.Append(*ActivityEntryViewModel::From(group, options));
        }

        if (_logPath.empty())
        {
            _statusText = RS_(L"Activity_NoLocation/Text");
        }
        else if (_all.empty())
        {
            // An empty file and a switched-off log look the same from here, and
            // they call for opposite responses, so say which one it is.
            _statusText = AL::Enabled() ?
                              RS_(L"Activity_Empty/Text") :
                              RS_(L"Activity_Disabled/Text");
        }
        else if (mode == GroupMode::None)
        {
            _statusText = winrt::hstring{ RS_fmt(L"Activity_Count/Text", _entries.Size(), _all.size()) };
        }
        else
        {
            _statusText = winrt::hstring{ RS_fmt(L"Activity_GroupCount/Text", _entries.Size(), matched, _all.size()) };
        }

        _NotifyChanges(L"Entries", L"StatusText", L"HasEntries");
    }

    Activity::Activity()
    {
        InitializeComponent();
    }

    void Activity::OnNavigatedTo(const NavigationEventArgs& e)
    {
        const auto args = e.Parameter().as<Editor::NavigateToPageArgs>();
        _ViewModel = args.ViewModel().as<Editor::ActivityViewModel>();
        BringIntoViewWhenLoaded(args.ElementToFocus());

        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "NavigatedToPage",
            TraceLoggingDescription("Event emitted when the user navigates to a page in the settings UI"),
            TraceLoggingValue("activity", "PageId", "The identifier of the page that was navigated to"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    void Activity::RefreshButton_Click(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::RoutedEventArgs& /*e*/)
    {
        if (_ViewModel)
        {
            _ViewModel.Reload();
        }
    }

    void Activity::RevealButton_Click(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::RoutedEventArgs& /*e*/)
    {
        if (!_ViewModel)
        {
            return;
        }
        const std::wstring path{ _ViewModel.LogPath() };
        if (path.empty())
        {
            return;
        }
        _revealInExplorer(path);
    }
}
