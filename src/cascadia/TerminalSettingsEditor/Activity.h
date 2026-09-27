// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The Settings UI's view of activity.jsonl - every process this Terminal
// started and every console client handed to it.
//
// There is also a pane (TerminalApp's ActivityPaneContent), reachable from the
// command palette, and the two answer different moments. The pane is for the
// window that just surprised you and is still on screen. This page is where you
// go looking when you remember the question later, because it is where the
// setting that turns recording on already lives.
//
// Both read through ActivityLogReader.h, so they cannot disagree about the file.

#pragma once

#include "Activity.g.h"
#include "ActivityViewModel.g.h"
#include "ActivityEntryViewModel.g.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

#include "../inc/ActivityLogReader.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // Everything about one record that costs a locale call to work out, computed
    // once when the log is read. The filter box re-runs on every keystroke, and
    // GetDateFormatEx across a couple of thousand records per keystroke is the
    // difference between a filter that types and one that stutters.
    struct ActivityDerived
    {
        std::wstring absoluteTime; // local date and time, in the user's format
        std::wstring dayKey; // local date alone, which is what "group by day" groups on
        std::wstring programKey; // exe leaf, lowercased, for the program filter
    };

    // What one row summarises. A single event is a group of one, so the row
    // template has one shape rather than two: count 1 simply hides the badge and
    // the range.
    //
    // The pointers are into ActivityViewModel::_all, which outlives every pass
    // that builds these.
    struct ActivityGroup
    {
        const ::Microsoft::Terminal::ActivityLog::ReadEntry* newest{ nullptr };
        const ActivityDerived* newestDerived{ nullptr };
        std::wstring oldestAbsoluteTime;
        std::wstring oldestTimestamp;
        uint32_t count{ 0 };

        // Whether every member agrees on a field. A field the members disagree
        // about is not shown as though it belonged to all of them - a wrong
        // working directory is worse than an absent one, the same reason
        // ActivityLog::Entry leaves unknown fields empty.
        bool sameKind{ true };
        bool sameProgram{ true };
        bool sameCommandLine{ true };
        bool sameWorkingDirectory{ true };
        bool sameParent{ true };
    };

    // The two display choices the rows are built against. Both change what the
    // strings say, not just how they are drawn, so a change rebuilds the rows.
    struct ActivityEntryOptions
    {
        bool useAbsoluteTime{ false };
        bool showCommandLine{ true };
    };

    struct ActivityEntryViewModel : ActivityEntryViewModelT<ActivityEntryViewModel>
    {
        ActivityEntryViewModel() = default;

        winrt::hstring TimestampDisplay() const noexcept { return _timestampDisplay; }
        winrt::hstring TimestampTooltip() const noexcept { return _timestampTooltip; }
        winrt::hstring RangeText() const noexcept { return _rangeText; }
        winrt::hstring CountText() const noexcept { return _countText; }
        winrt::hstring EventLabel() const noexcept { return _eventLabel; }
        winrt::hstring EventTooltip() const noexcept { return _eventTooltip; }
        winrt::hstring ExeName() const noexcept { return _exeName; }
        winrt::hstring CommandLine() const noexcept { return _commandLine; }
        winrt::hstring WorkingDirectory() const noexcept { return _cwd; }
        winrt::hstring ParentExe() const noexcept { return _parentExe; }
        winrt::hstring HoverSummary() const noexcept { return _hoverSummary; }

        bool IsGroup() const noexcept { return _isGroup; }
        bool HasRange() const noexcept { return !_rangeText.empty(); }
        bool ShowCommandLineInline() const noexcept { return _showCommandLineInline; }
        bool ShowCommandLineInTooltip() const noexcept { return _showCommandLineInTooltip; }
        bool HasWorkingDirectory() const noexcept { return !_cwd.empty(); }
        bool HasParent() const noexcept { return !_parentExe.empty(); }

        static winrt::com_ptr<ActivityEntryViewModel> From(const ActivityGroup& group, const ActivityEntryOptions& options);

    private:
        winrt::hstring _timestampDisplay;
        winrt::hstring _timestampTooltip;
        winrt::hstring _rangeText;
        winrt::hstring _countText;
        winrt::hstring _eventLabel;
        winrt::hstring _eventTooltip;
        winrt::hstring _exeName;
        winrt::hstring _commandLine;
        winrt::hstring _cwd;
        winrt::hstring _parentExe;
        winrt::hstring _hoverSummary;
        bool _isGroup{ false };
        bool _showCommandLineInline{ false };
        bool _showCommandLineInTooltip{ false };
    };

    struct ActivityViewModel : ActivityViewModelT<ActivityViewModel>, ViewModelHelper<ActivityViewModel>
    {
    public:
        ActivityViewModel(Model::CascadiaSettings settings);

        // DON'T YOU DARE ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper. Do this instead:
        using ViewModelHelper<ActivityViewModel>::PropertyChanged;

        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_settings.GlobalSettings(), ActivityLog);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_settings.GlobalSettings(), ActivityLogMaxKilobytes);

        winrt::Windows::Foundation::Collections::IObservableVector<Editor::ActivityEntryViewModel> Entries() const noexcept { return _entries; }

        winrt::hstring Filter() const noexcept { return _filter; }
        void Filter(const winrt::hstring& value);

        // Which of the six recorded kinds to show. 0 is every kind; 1 through 6
        // index KindOrder in the .cpp. An index rather than the kind string so
        // the ComboBox binds SelectedIndex, which is an Int32 both ways - a
        // TwoWay SelectedValue against a String property has to round-trip
        // through Object, and an empty Tag for "all" is indistinguishable from
        // no selection.
        int32_t KindChoiceIndex() const noexcept { return _kindChoiceIndex; }
        void KindChoiceIndex(int32_t value);

        // Programs and days cannot be a fixed list: they are whatever is in the
        // log. Index 0 of each is "all", so the indices line up with the choice
        // vectors the ComboBoxes draw.
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> ProgramChoices() const noexcept { return _programChoices; }
        int32_t ProgramChoiceIndex() const noexcept { return _programChoiceIndex; }
        void ProgramChoiceIndex(int32_t value);

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> DayChoices() const noexcept { return _dayChoices; }
        int32_t DayChoiceIndex() const noexcept { return _dayChoiceIndex; }
        void DayChoiceIndex(int32_t value);

        int32_t GroupModeIndex() const noexcept { return _groupModeIndex; }
        void GroupModeIndex(int32_t value);

        bool UseAbsoluteTime() const noexcept { return _useAbsoluteTime; }
        void UseAbsoluteTime(bool value);

        bool ShowCommandLines() const noexcept { return _showCommandLines; }
        void ShowCommandLines(bool value);

        winrt::hstring StatusText() const noexcept { return _statusText; }
        winrt::hstring LogPath() const noexcept { return _logPath; }
        bool HasLogPath() const noexcept { return !_logPath.empty(); }
        bool HasEntries() const noexcept { return _entries.Size() != 0; }

        void Reload();

    private:
        Model::CascadiaSettings _settings;
        winrt::Windows::Foundation::Collections::IObservableVector<Editor::ActivityEntryViewModel> _entries;
        std::vector<::Microsoft::Terminal::ActivityLog::ReadEntry> _all;
        std::vector<ActivityDerived> _derived; // parallel to _all

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> _programChoices;
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> _dayChoices;
        std::vector<std::wstring> _programKeys; // parallel to _programChoices; [0] empty means all
        std::vector<std::wstring> _dayKeys; // parallel to _dayChoices; [0] empty means all

        winrt::hstring _filter;
        int32_t _kindChoiceIndex{ 0 };
        int32_t _programChoiceIndex{ 0 };
        int32_t _dayChoiceIndex{ 0 };
        int32_t _groupModeIndex{ 0 };
        bool _useAbsoluteTime{ false };
        bool _showCommandLines{ true };
        winrt::hstring _statusText;
        winrt::hstring _logPath;

        // True only while Reload() swaps the contents of a choice vector. A
        // ComboBox whose items are cleared writes SelectedIndex -1 back through
        // the TwoWay binding as it happens, which would otherwise be mistaken
        // for the user choosing nothing and would re-filter against a list that
        // is half rebuilt.
        bool _rebuildingChoices{ false };

        void _rebuildChoices();
        void _applyFilter();
    };

    struct Activity : public HasScrollViewer<Activity>, ActivityT<Activity>
    {
        Activity();

        void OnNavigatedTo(const winrt::Windows::UI::Xaml::Navigation::NavigationEventArgs& e);

        void RefreshButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void RevealButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);

        til::property_changed_event PropertyChanged;
        WINRT_OBSERVABLE_PROPERTY(Editor::ActivityViewModel, ViewModel, PropertyChanged.raise, nullptr);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    // No factory for ActivityEntryViewModel: its runtimeclass declares no
    // constructor, so nothing activates it from outside.
    BASIC_FACTORY(Activity);
    BASIC_FACTORY(ActivityViewModel);
}
