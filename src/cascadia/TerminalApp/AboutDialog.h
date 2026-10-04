// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "AboutDialog.g.h"

namespace winrt::TerminalApp::implementation
{
    struct AboutDialog : AboutDialogT<AboutDialog>
    {
    public:
        AboutDialog();

        winrt::hstring ApplicationDisplayName();
        winrt::hstring ApplicationVersion();
        winrt::hstring BuildCommit();
        winrt::hstring BuildBranch();
        winrt::hstring BuildTime();
        winrt::hstring BuildExePath();
        winrt::hstring BuildInfoForClipboard();

        til::property_changed_event PropertyChanged;

    private:
        friend struct AboutDialogT<AboutDialog>; // for Xaml to bind events

        void _CopyBuildInfoOnClick(const IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& eventArgs);
        void _ReportIssueOnClick(const IInspectable& sender, const Windows::UI::Xaml::Controls::ContentDialogButtonClickEventArgs& eventArgs);
        void _PopulateReleaseNotes();
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(AboutDialog);
}
