
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AboutDialog.h"
#include "AboutDialog.g.cpp"

#include <WtExeUtils.h>
#include "BuildInfo.h"

#include "../../types/inc/utils.hpp"
#include "Utils.h"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal;
using namespace ::TerminalApp;
using namespace std::chrono_literals;

namespace winrt
{
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace winrt::TerminalApp::implementation
{
    AboutDialog::AboutDialog()
    {
        InitializeComponent();
        // Set here rather than from the resources, whose PrimaryButtonText is still
        // upstream's "Send feedback" in every locale.
        PrimaryButtonText(RS_(L"AboutDialog_ReportIssueButton"));
        _PopulateReleaseNotes();
    }

    winrt::hstring AboutDialog::ApplicationDisplayName()
    {
        return CascadiaSettings::ApplicationDisplayName();
    }

    winrt::hstring AboutDialog::ApplicationVersion()
    {
        if (CascadiaSettings::IsPortableMode())
        {
            // Portable builds have no package version. Identify the actual compiled
            // source with the same generated build producer used by the Commit row.
            return winrt::hstring{ fmt::format(FMT_COMPILE(L"source {}"), ::TerminalApp::BuildInfo::Commit()) };
        }
        return CascadiaSettings::ApplicationVersion();
    }

    winrt::hstring AboutDialog::BuildCommit()
    {
        return ::TerminalApp::BuildInfo::Commit();
    }

    winrt::hstring AboutDialog::BuildBranch()
    {
        return ::TerminalApp::BuildInfo::Branch();
    }

    winrt::hstring AboutDialog::BuildTime()
    {
        return ::TerminalApp::BuildInfo::BuildTime();
    }

    winrt::hstring AboutDialog::BuildExePath()
    {
        return ::TerminalApp::BuildInfo::ExePath();
    }

    // Everything needed to identify this binary, for pasting into a bug report.
    // The text itself lives in BuildInfo so this and the tab row badge cannot copy
    // different things -- they are the two places you reach for the same answer.
    winrt::hstring AboutDialog::BuildInfoForClipboard()
    {
        return ::TerminalApp::BuildInfo::ClipboardText(ApplicationDisplayName(), ApplicationVersion());
    }

    void AboutDialog::_CopyBuildInfoOnClick(const IInspectable& /*sender*/, const Windows::UI::Xaml::RoutedEventArgs& /*eventArgs*/)
    {
        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(BuildInfoForClipboard());
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    }

    // The dialog's one button opens a new issue on Mullion's own tracker. Upstream's
    // went to Microsoft's feedback pages, which know nothing about this fork.
    void AboutDialog::_ReportIssueOnClick(const IInspectable& /*sender*/, const Windows::UI::Xaml::Controls::ContentDialogButtonClickEventArgs& /*eventArgs*/)
    {
        ShellExecute(nullptr, nullptr, L"https://github.com/aylith-labs/mullion/issues/new", nullptr, nullptr, SW_SHOW);
    }

    // One collapsed group per CI build of main, newest first and the newest open,
    // titled by how long ago it was built. The list is compiled in
    // (TerminalBuildInfo.h, from tools/Write-ReleaseNotes.ps1); a build made
    // anywhere but CI has none and says so instead.
    void AboutDialog::_PopulateReleaseNotes()
    {
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;
        namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;

        if (TerminalReleaseNoteBuildCount == 0 || TerminalReleaseNoteBuildList == nullptr)
        {
            ReleaseNotesEmpty().Visibility(WUX::Visibility::Visible);
            return;
        }

        const std::wstring_view runningCommit{ TERMINAL_BUILD_COMMIT_FULL };
        const auto monospace = WUX::Media::FontFamily{ L"Cascadia Mono, Consolas, monospace" };
        const auto list = ReleaseNotesList();

        for (int i = 0; i < TerminalReleaseNoteBuildCount; ++i)
        {
            const auto& build = TerminalReleaseNoteBuildList[i];
            const std::wstring_view sha{ build.Sha };
            const auto isRunning = !sha.empty() && runningCommit.starts_with(sha);

            // "3 hours ago", with the absolute time on hover.
            auto age = ::TerminalApp::BuildInfo::RelativeAge(build.BuiltAt);
            if (!age.empty())
            {
                age[0] = static_cast<wchar_t>(towupper(age[0]));
            }
            std::wstring absolute;
            {
                std::tm utc{};
                const auto seconds = static_cast<std::time_t>(build.BuiltAt);
                if (gmtime_s(&utc, &seconds) == 0)
                {
                    wchar_t buffer[32]{};
                    if (wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M UTC", &utc) > 0)
                    {
                        absolute = buffer;
                    }
                }
            }

            WUXC::StackPanel header;
            header.Orientation(WUXC::Orientation::Vertical);
            WUXC::TextBlock title;
            title.Text(winrt::hstring{ isRunning ? fmt::format(FMT_COMPILE(L"{} \u00B7 {}"), age, std::wstring_view{ RS_(L"AboutDialog_ThisBuild") }) : age });
            title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            if (!absolute.empty())
            {
                WUXC::ToolTipService::SetToolTip(title, winrt::box_value(winrt::hstring{ absolute }));
            }
            header.Children().Append(title);

            WUXC::TextBlock detail;
            detail.Text(winrt::hstring{ fmt::format(FMT_COMPILE(L"{} \u00B7 build #{} \u00B7 {} commit{}"),
                                                    sha,
                                                    build.RunNumber,
                                                    build.CommitCount,
                                                    build.CommitCount == 1 ? L"" : L"s") });
            detail.FontSize(12);
            detail.Opacity(0.7);
            header.Children().Append(detail);

            WUXC::StackPanel body;
            body.Spacing(6);
            for (int j = 0; j < build.CommitCount; ++j)
            {
                const auto& commit = build.Commits[j];
                WUXC::Grid row;
                WUXC::ColumnDefinition shaColumn;
                shaColumn.Width(WUX::GridLengthHelper::Auto());
                WUXC::ColumnDefinition subjectColumn;
                subjectColumn.Width(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
                row.ColumnDefinitions().Append(shaColumn);
                row.ColumnDefinitions().Append(subjectColumn);
                row.ColumnSpacing(10);

                WUXC::TextBlock commitSha;
                commitSha.Text(commit.Sha);
                commitSha.FontFamily(monospace);
                commitSha.Opacity(0.6);
                commitSha.IsTextSelectionEnabled(true);
                WUXC::Grid::SetColumn(commitSha, 0);

                WUXC::TextBlock subject;
                subject.Text(commit.Subject);
                subject.TextWrapping(WUX::TextWrapping::Wrap);
                subject.IsTextSelectionEnabled(true);
                WUXC::Grid::SetColumn(subject, 1);

                row.Children().Append(commitSha);
                row.Children().Append(subject);
                body.Children().Append(row);
            }

            MUXC::Expander expander;
            expander.HorizontalAlignment(WUX::HorizontalAlignment::Stretch);
            expander.HorizontalContentAlignment(WUX::HorizontalAlignment::Left);
            expander.Header(header);
            expander.Content(body);
            expander.IsExpanded(i == 0);
            WUX::Automation::AutomationProperties::SetName(expander, winrt::hstring{ fmt::format(FMT_COMPILE(L"{}, {}"), age, sha) });
            list.Children().Append(expander);
        }
    }
}
