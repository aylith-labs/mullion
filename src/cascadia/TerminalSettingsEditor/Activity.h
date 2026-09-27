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
    struct ActivityEntryViewModel : ActivityEntryViewModelT<ActivityEntryViewModel>
    {
        ActivityEntryViewModel() = default;

        winrt::hstring Timestamp() const noexcept { return _timestamp; }
        winrt::hstring TimestampDisplay() const noexcept { return _timestampDisplay; }
        winrt::hstring TimestampTooltip() const noexcept { return _timestampTooltip; }
        winrt::hstring Kind() const noexcept { return _kind; }
        winrt::hstring EventLabel() const noexcept { return _eventLabel; }
        winrt::hstring EventTooltip() const noexcept { return _eventTooltip; }
        winrt::hstring Exe() const noexcept { return _exe; }
        winrt::hstring ExeName() const noexcept { return _exeName; }
        winrt::hstring CommandLine() const noexcept { return _commandLine; }
        winrt::hstring WorkingDirectory() const noexcept { return _cwd; }
        winrt::hstring ParentExe() const noexcept { return _parentExe; }
        winrt::hstring Reason() const noexcept { return _reason; }

        bool HasCommandLine() const noexcept { return !_commandLine.empty(); }
        bool HasWorkingDirectory() const noexcept { return !_cwd.empty(); }
        bool HasParent() const noexcept { return !_parentExe.empty(); }

        static winrt::com_ptr<ActivityEntryViewModel> From(const ::Microsoft::Terminal::ActivityLog::ReadEntry& read, bool useAbsoluteTime);

    private:
        winrt::hstring _timestamp;
        winrt::hstring _timestampDisplay;
        winrt::hstring _timestampTooltip;
        winrt::hstring _kind;
        winrt::hstring _eventLabel;
        winrt::hstring _eventTooltip;
        winrt::hstring _exe;
        winrt::hstring _exeName;
        winrt::hstring _commandLine;
        winrt::hstring _cwd;
        winrt::hstring _parentExe;
        winrt::hstring _reason;
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

        winrt::hstring KindFilter() const noexcept { return _kindFilter; }
        void KindFilter(const winrt::hstring& value);

        bool UseAbsoluteTime() const noexcept { return _useAbsoluteTime; }
        void UseAbsoluteTime(bool value);

        winrt::hstring StatusText() const noexcept { return _statusText; }
        winrt::hstring LogPath() const noexcept { return _logPath; }
        bool HasEntries() const noexcept { return _entries.Size() != 0; }

        void Reload();

    private:
        Model::CascadiaSettings _settings;
        winrt::Windows::Foundation::Collections::IObservableVector<Editor::ActivityEntryViewModel> _entries;
        std::vector<::Microsoft::Terminal::ActivityLog::ReadEntry> _all;
        winrt::hstring _filter;
        winrt::hstring _kindFilter;
        bool _useAbsoluteTime{ false };
        winrt::hstring _statusText;
        winrt::hstring _logPath;

        void _applyFilter();
    };

    struct Activity : public HasScrollViewer<Activity>, ActivityT<Activity>
    {
        Activity();

        void OnNavigatedTo(const winrt::Windows::UI::Xaml::Navigation::NavigationEventArgs& e);

        void RefreshButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);

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
