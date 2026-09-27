// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Activity.h"
#include "Activity.g.cpp"
#include "ActivityViewModel.g.cpp"
#include "ActivityEntryViewModel.g.cpp"

#include <array>
#include <utility>

using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // Deliberately far smaller than the pane's 4000. The list here is an
    // ItemsControl in the page's own scroll flow rather than a ListView with a
    // viewport of its own, so every row it is given is realized. 200 is more
    // than fits on a screen, the filter narrows it, and the pane is still there
    // for a full trawl.
    static constexpr size_t MaxEntries{ 200 };

    namespace
    {
        bool _parseTimestamp(const std::wstring& value, SYSTEMTIME& utc) noexcept
        {
            int year, month, day, hour, minute, second, milliseconds;
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

        std::wstring _absoluteLocalTime(const std::wstring& timestamp)
        {
            SYSTEMTIME utc{};
            SYSTEMTIME local{};
            if (!_parseTimestamp(timestamp, utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
            {
                return timestamp;
            }

            std::array<wchar_t, 128> date{};
            std::array<wchar_t, 128> time{};
            if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date.data(), gsl::narrow_cast<int>(date.size()), nullptr) ||
                !GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, nullptr, time.data(), gsl::narrow_cast<int>(time.size())))
            {
                return timestamp;
            }
            return fmt::format(FMT_COMPILE(L"{} {}"), date.data(), time.data());
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
            const auto seconds{ nowTicks.QuadPart > thenTicks.QuadPart ? (nowTicks.QuadPart - thenTicks.QuadPart) / 10'000'000 : 0 };
            if (seconds < 60)
            {
                return L"just now";
            }
            if (seconds < 3600)
            {
                const auto minutes{ seconds / 60 };
                return fmt::format(FMT_COMPILE(L"{} minute{} ago"), minutes, minutes == 1 ? L"" : L"s");
            }
            if (seconds < 86400)
            {
                const auto hours{ seconds / 3600 };
                return fmt::format(FMT_COMPILE(L"{} hour{} ago"), hours, hours == 1 ? L"" : L"s");
            }
            const auto days{ seconds / 86400 };
            return fmt::format(FMT_COMPILE(L"{} day{} ago"), days, days == 1 ? L"" : L"s");
        }

        std::pair<std::wstring, std::wstring> _eventText(const std::wstring& kind)
        {
            if (kind == L"profileLaunch")
            {
                return { L"profile launch", L"Terminal started this command through one of its profiles, such as when opening a new tab, pane, or window." };
            }
            if (kind == L"handoff")
            {
                return { L"console handoff", L"Windows console delegation handed this console program to Terminal to host, instead of starting it in a separate legacy console window." };
            }
            return { kind.empty() ? L"unknown event" : kind, L"The kind of activity Terminal recorded for this program." };
        }
    }

    winrt::com_ptr<ActivityEntryViewModel> ActivityEntryViewModel::From(const ::Microsoft::Terminal::ActivityLog::ReadEntry& read, bool useAbsoluteTime)
    {
        auto entry{ winrt::make_self<ActivityEntryViewModel>() };
        entry->_timestamp = winrt::hstring{ read.timestamp };
        const auto absoluteTime{ _absoluteLocalTime(read.timestamp) };
        entry->_timestampDisplay = winrt::hstring{ useAbsoluteTime ? absoluteTime : _relativeTime(read.timestamp) };
        entry->_timestampTooltip = winrt::hstring{ absoluteTime };
        entry->_kind = winrt::hstring{ read.kind };
        const auto [eventLabel, eventTooltip] = _eventText(read.kind);
        entry->_eventLabel = winrt::hstring{ eventLabel };
        entry->_eventTooltip = winrt::hstring{ eventTooltip };
        entry->_exe = winrt::hstring{ read.exe };
        entry->_exeName = winrt::hstring{ read.exeName };
        entry->_commandLine = winrt::hstring{ read.commandLine };
        entry->_cwd = winrt::hstring{ read.cwd };
        entry->_parentExe = winrt::hstring{ read.parentExe };
        entry->_reason = winrt::hstring{ read.reason };
        return entry;
    }

    ActivityViewModel::ActivityViewModel(Model::CascadiaSettings settings) :
        _settings{ settings },
        _entries{ winrt::single_threaded_observable_vector<Editor::ActivityEntryViewModel>() }
    {
        Reload();
    }

    void ActivityViewModel::Reload()
    {
        const auto path{ ::Microsoft::Terminal::ActivityLog::Path() };
        _logPath = winrt::hstring{ path.wstring() };

        _all = ::Microsoft::Terminal::ActivityLog::ReadRecent(MaxEntries);
        _applyFilter();

        _NotifyChanges(L"LogPath");
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

    void ActivityViewModel::KindFilter(const winrt::hstring& value)
    {
        if (_kindFilter == value)
        {
            return;
        }
        _kindFilter = value;
        _NotifyChanges(L"KindFilter");
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

    void ActivityViewModel::_applyFilter()
    {
        std::wstring needle{ _filter };
        std::transform(needle.begin(), needle.end(), needle.begin(), [](wchar_t c) { return til::tolower_ascii(c); });

        _entries.Clear();
        for (const auto& record : _all)
        {
            if ((_kindFilter.empty() || record.kind == _kindFilter) &&
                (needle.empty() || record.searchText.find(needle) != std::wstring::npos))
            {
                _entries.Append(*ActivityEntryViewModel::From(record, _useAbsoluteTime));
            }
        }

        if (_logPath.empty())
        {
            _statusText = RS_(L"Activity_NoLocation/Text");
        }
        else if (_all.empty())
        {
            // An empty file and a switched-off log look the same from here, and
            // they call for opposite responses, so say which one it is.
            _statusText = ::Microsoft::Terminal::ActivityLog::Enabled() ?
                              RS_(L"Activity_Empty/Text") :
                              RS_(L"Activity_Disabled/Text");
        }
        else
        {
            _statusText = winrt::hstring{ RS_fmt(L"Activity_Count/Text", _entries.Size(), _all.size()) };
        }

        _NotifyChanges(L"Entries");
        _NotifyChanges(L"StatusText");
        _NotifyChanges(L"HasEntries");
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
}
