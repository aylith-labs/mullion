// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The promote affordance for the local Dev slot: notice that a deploy has
// staged a newer build, offer it, and - only if asked - hand the swap to a
// detached helper.
//
// Why any of this exists: the Dev slot is where live sessions run, so a deploy
// deliberately does not touch it. It registers the Test slot, stages the Dev
// payload, and leaves the decision here, because only the person using the
// windows knows whether losing them is acceptable right now.

#include "pch.h"
#include "TerminalPage.h"
#include "BuildInfo.h"
#include "SlotPromotion.h"
#include "WindowListRequest.g.h"

#include <ActivityLog.h>
#include <atomic>

#include <LibraryResources.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace
{
    namespace WUX = winrt::Windows::UI::Xaml;

    WUX::Controls::TextBlock Paragraph(const winrt::hstring& text)
    {
        WUX::Controls::TextBlock block;
        block.Text(text);
        block.TextWrapping(WUX::TextWrapping::Wrap);
        return block;
    }

    constexpr std::wstring_view SiteRoot{ L"https://mullion.aylith.com/" };

    // A line of links that wraps like text: Hyperlink inlines in a TextBlock,
    // rather than HyperlinkButtons in a horizontal StackPanel, which would clip
    // in a narrow window or a longer translation.
    struct LinkLine
    {
        WUX::Controls::TextBlock Block;

        explicit LinkLine(double fontSize)
        {
            Block.TextWrapping(WUX::TextWrapping::Wrap);
            Block.FontSize(fontSize);
        }

        void Text(const winrt::hstring& text)
        {
            WUX::Documents::Run run;
            run.Text(text);
            Block.Inlines().Append(run);
        }

        // Click, not NavigateUri: the dialog's other links open through
        // ShellExecute, and NavigateUri's launcher is not one to rely on inside
        // a XAML island. accessibleName is for links whose text alone ("#12004")
        // says too little to a screen reader.
        void Link(const winrt::hstring& text, std::wstring url, const winrt::hstring& accessibleName = {})
        {
            WUX::Documents::Run run;
            run.Text(text);
            WUX::Documents::Hyperlink link;
            link.Click([url = std::move(url)](auto&&, auto&&) {
                ShellExecute(nullptr, nullptr, url.c_str(), nullptr, nullptr, SW_SHOW);
            });
            link.Inlines().Append(run);
            if (!accessibleName.empty())
            {
                WUX::Automation::AutomationProperties::SetName(link, accessibleName);
            }
            Block.Inlines().Append(link);
        }

        bool Empty() const
        {
            return Block.Inlines().Size() == 0;
        }
    };

    // Under a commit's subject: the feature page it belongs to and the upstream
    // issues behind it. An issue with a row on mullion.aylith.com's upstream
    // page links to that row; one without links to GitHub.
    WUX::Controls::TextBlock CommitLinks(const ::TerminalApp::SlotPromotion::StagedCommit& commit)
    {
        LinkLine line{ 12 };
        for (const auto& feature : commit.Features)
        {
            if (!line.Empty())
            {
                line.Text(L" · ");
            }
            line.Link(feature.Title, fmt::format(FMT_COMPILE(L"{}feature?id={}"), SiteRoot, std::wstring_view{ feature.Id }));
        }

        std::vector<std::pair<uint32_t, bool>> issues;
        for (const auto n : commit.Issues)
        {
            issues.emplace_back(n, true);
        }
        for (const auto n : commit.Refs)
        {
            issues.emplace_back(n, false);
        }
        std::sort(issues.begin(), issues.end());
        if (!issues.empty())
        {
            if (!line.Empty())
            {
                line.Text(L" · ");
            }
            line.Text(RS_(L"PromoteSlotDialog_CommitUpstream") + L" ");
            for (size_t i = 0; i < issues.size(); ++i)
            {
                const auto [n, onSite] = issues[i];
                if (i > 0)
                {
                    line.Text(L", ");
                }
                line.Link(winrt::hstring{ fmt::format(FMT_COMPILE(L"#{}"), n) },
                          onSite ? fmt::format(FMT_COMPILE(L"{}upstream#issue-{}"), SiteRoot, n) :
                                   fmt::format(FMT_COMPILE(L"https://github.com/microsoft/terminal/issues/{}"), n),
                          winrt::hstring{ RS_fmt(L"PromoteSlotDialog_IssueLinkName", n) });
            }
        }
        return line.Block;
    }

    // The promote dialog's body: what is staged, what it adds over the build
    // this window runs, and what applying it costs. Built in code because the
    // list of commits is. Rows look like the About dialog's release notes.
    WUX::UIElement PromotionDialogContent(const ::TerminalApp::SlotPromotion::StagedBuild& staged, const winrt::hstring& cost)
    {
        WUX::Controls::StackPanel content;
        content.Spacing(12);
        content.Children().Append(Paragraph(winrt::hstring{ RS_fmt(L"PromoteSlotDialog_Staged", std::wstring_view{ staged.Describe() }) }));

        const auto changes{ staged.ChangesSinceRunning() };
        if (changes.Commits.empty())
        {
            // Complete with nothing in it means the notes reached the running
            // commit straight away: a rebuild of it, typically with local changes.
            auto none{ Paragraph(changes.Complete ? RS_(L"PromoteSlotDialog_WhatsNewNothing") : RS_(L"PromoteSlotDialog_WhatsNewUnknown")) };
            none.Opacity(0.7);
            content.Children().Append(none);
        }
        else
        {
            WUX::Controls::StackPanel section;
            section.Spacing(6);
            auto heading{ Paragraph(winrt::hstring{ changes.Complete ? RS_fmt(L"PromoteSlotDialog_WhatsNew", changes.Commits.size()) :
                                                                       RS_fmt(L"PromoteSlotDialog_WhatsNewPartial", changes.Commits.size()) }) };
            heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            section.Children().Append(heading);

            const WUX::Media::FontFamily monospace{ L"Cascadia Mono, Consolas, monospace" };
            WUX::Controls::StackPanel list;
            list.Spacing(6);
            // Room for the scrollbar, so it never sits over the text.
            list.Padding(WUX::ThicknessHelper::FromLengths(0, 0, 12, 0));
            for (const auto& commit : changes.Commits)
            {
                WUX::Controls::Grid row;
                WUX::Controls::ColumnDefinition shaColumn;
                shaColumn.Width(WUX::GridLengthHelper::Auto());
                WUX::Controls::ColumnDefinition subjectColumn;
                subjectColumn.Width(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
                row.ColumnDefinitions().Append(shaColumn);
                row.ColumnDefinitions().Append(subjectColumn);
                row.ColumnSpacing(10);

                WUX::Controls::TextBlock sha;
                sha.Text(commit.Sha);
                sha.FontFamily(monospace);
                sha.Opacity(0.6);
                sha.IsTextSelectionEnabled(true);
                WUX::Controls::Grid::SetColumn(sha, 0);

                auto subject{ Paragraph(commit.Subject) };
                subject.IsTextSelectionEnabled(true);

                WUX::Controls::StackPanel text;
                text.Spacing(2);
                text.Children().Append(subject);
                if (!commit.Features.empty() || !commit.Issues.empty())
                {
                    text.Children().Append(CommitLinks(commit));
                }
                WUX::Controls::Grid::SetColumn(text, 1);

                row.Children().Append(sha);
                row.Children().Append(text);
                list.Children().Append(row);
            }

            WUX::Controls::ScrollViewer scroller;
            scroller.MaxHeight(220);
            scroller.HorizontalScrollBarVisibility(WUX::Controls::ScrollBarVisibility::Disabled);
            scroller.VerticalScrollBarVisibility(WUX::Controls::ScrollBarVisibility::Auto);
            scroller.Content(list);
            section.Children().Append(scroller);
            content.Children().Append(section);
        }

        // The full diff, for when the subjects are not enough (only when both
        // ends are commits GitHub can know about), and the site's feature and
        // upstream-issue lists beside it.
        LinkLine links{ 14 };
        const std::wstring_view running{ TERMINAL_BUILD_COMMIT_FULL };
        const std::wstring_view target{ staged.CommitFull.empty() ? staged.Commit : staged.CommitFull };
        if (running.size() >= 7 && target.size() >= 7 && !running.starts_with(target) && !target.starts_with(running))
        {
            links.Link(RS_(L"PromoteSlotDialog_Compare"),
                       fmt::format(FMT_COMPILE(L"https://github.com/aylith-labs/mullion/compare/{}...{}"), running, target));
            links.Text(L"      ");
        }
        links.Link(RS_(L"PromoteSlotDialog_Features"), fmt::format(FMT_COMPILE(L"{}features"), SiteRoot));
        links.Text(L"      ");
        links.Link(RS_(L"PromoteSlotDialog_UpstreamIssues"), fmt::format(FMT_COMPILE(L"{}upstream"), SiteRoot));
        content.Children().Append(links.Block);

        content.Children().Append(Paragraph(winrt::hstring{ RS_fmt(L"PromoteSlotDialog_Body", std::wstring_view{ cost }) }));
        return content;
    }
}

namespace winrt::TerminalApp::implementation
{
    // Ask whether anything is staged, and put the answer in the tab row. Cheap
    // enough to call whenever we might have missed a change; it reads one small
    // file and returns nothing at all outside the Dev slot.
    void TerminalPage::RefreshPendingPromotion()
    {
        if (!_tabRow)
        {
            return;
        }

        const auto pending{ ::TerminalApp::SlotPromotion::PendingPromotion() };
        _tabRow.PromotionAvailable(pending.has_value());
        _tabRow.PromotionDescription(pending ? pending->Describe() : hstring{});
    }

    // How much is about to be thrown away. The window count comes from the
    // emperor, which is the only thing that can see all of them; the pane count
    // is this window's, which is the one the user is looking at.
    hstring TerminalPage::_describePromotionCost()
    {
        uint32_t windowCount{ 1 };
        if (const auto request{ winrt::make<WindowListRequest>() })
        {
            RequestWindowList.raise(*this, request);
            if (const auto entries{ request.Entries() }; entries && entries.Size() > 0)
            {
                windowCount = entries.Size();
            }
        }

        uint32_t paneCount{ 0 };
        for (const auto& tab : _tabs)
        {
            if (const auto tabImpl{ _GetTabImpl(tab) })
            {
                paneCount += gsl::narrow_cast<uint32_t>(std::max(0, tabImpl->GetLeafPaneCount()));
            }
        }

        return hstring{ RS_fmt(L"PromoteSlotDialog_WindowsAndPanes", windowCount, paneCount) };
    }

    safe_void_coroutine TerminalPage::_PromoteSlotRequested(IInspectable /*sender*/, IInspectable /*args*/)
    {
        const auto weak{ get_weak() };

        // Re-read rather than trusting what lit the button: the deploy may have
        // run again, or been undone, since it appeared.
        const auto pending{ ::TerminalApp::SlotPromotion::PendingPromotion() };
        if (!pending)
        {
            RefreshPendingPromotion();
            co_return;
        }

        ContentDialog dialog{};
        dialog.Title(box_value(hstring{ RS_(L"PromoteSlotDialog_Title") }));
        dialog.Content(PromotionDialogContent(*pending, _describePromotionCost()));
        dialog.PrimaryButtonText(RS_(L"PromoteSlotDialog_ApplyOnNextLaunch"));
        dialog.SecondaryButtonText(RS_(L"PromoteSlotDialog_ApplyNow"));
        dialog.CloseButtonText(RS_(L"PromoteSlotDialog_Cancel"));
        // Default to the button that destroys nothing. The primary action here
        // is disruptive enough that Enter must not be able to trigger the worst
        // of it by accident.
        dialog.DefaultButton(ContentDialogButton::Close);

        auto presenter{ _dialogPresenter.get() };
        if (!presenter)
        {
            co_return;
        }

        const auto result{ co_await presenter.ShowDialog(dialog) };

        // ShowDialog blocks until dismissed, so `this` may be gone by now.
        const auto strong{ weak.get() };
        if (!strong)
        {
            co_return;
        }

        if (result == ContentDialogResult::None)
        {
            co_return;
        }

        const auto applyNow{ result == ContentDialogResult::Secondary };
        // Hand the helper the payload we actually offered. It has its own
        // defaults for being run by hand, and those name the local deploy's
        // directory -- so leaving it to guess would quietly promote a local
        // build when the dialog described a fetched one.
        if (!strong->_armPromotionHelper(applyNow, *pending))
        {
            co_return;
        }

        if (applyNow)
        {
            // The helper is now waiting on our process id. Quitting takes the
            // normal path, so layouts persist exactly as they would on any
            // other quit and come back when the new build starts.
            strong->RequestQuit();
        }
    }

    // Spawn the detached helper that performs the swap once we are gone.
    //
    // It has to be a separate process: re-registering a package identity from a
    // different folder means removing the old registration first, and Windows
    // will not remove a package that is still running. Nothing here can do that
    // to itself.
    //
    // Returns false if the helper could not be started, in which case we must
    // not quit - closing every window to accomplish nothing is the one outcome
    // worse than not promoting.
    bool TerminalPage::_armPromotionHelper(bool relaunch, const ::TerminalApp::SlotPromotion::StagedBuild& staged)
    try
    {
        const auto helper{ std::filesystem::path{ ::TerminalApp::SlotPromotion::SlotRoot } / L"Promote-DevSlot.ps1" };
        if (!std::filesystem::exists(helper))
        {
            LOG_HR_MSG(E_INVALIDARG, "control slot promotion: helper missing");
            return false;
        }

        // Routed through Invoke-Hidden.vbs rather than launching powershell.exe
        // directly with SW_HIDE: on a machine that has Windows Terminal set as
        // its default terminal-delegation host, a "hidden" console-app launch
        // still gets a WT window created for it before the hidden-window request
        // can suppress it. See Invoke-Hidden.vbs's header for the full story.
        // Every token is independently quoted -- the vbs re-quotes each one
        // rather than trusting a flattened string, so paths with spaces survive.
        const auto hiddenLauncher{ std::filesystem::path{ ::TerminalApp::SlotPromotion::SlotRoot } / L"Invoke-Hidden.vbs" };
        const auto args{ fmt::format(LR"("{}" "powershell.exe" "-NoProfile" "-NonInteractive" "-ExecutionPolicy" "Bypass" "-File" "{}" "-WaitForPid" "{}" "-Staged" "{}" "-Marker" "{}"{})",
                                     hiddenLauncher.wstring(),
                                     helper.wstring(),
                                     GetCurrentProcessId(),
                                     std::wstring_view{ staged.Payload },
                                     staged.MarkerPath.wstring(),
                                     relaunch ? LR"( "-Relaunch")" : L"") };

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"open";
        info.lpFile = L"wscript.exe";
        info.lpParameters = args.c_str();
        // The helper outlives us on purpose - it cannot start work until we are
        // gone - so SEE_MASK_NOASYNC matters: it keeps the launch from being
        // torn down when this process begins exiting.
        info.nShow = SW_HIDE;

        if (!ShellExecuteExW(&info))
        {
            LOG_LAST_ERROR();
            return false;
        }

        // Record it before releasing the handle, while we can still name the pid.
        // This is one of our own hidden launches, and a hidden launch that turns
        // visible is precisely the thing the log is meant to explain -- so the
        // entry says it went through Invoke-Hidden.vbs, which is what makes a
        // stray window here a bug rather than a mystery.
        try
        {
            ::Microsoft::Terminal::ActivityLog::Record({
                .kind = ::Microsoft::Terminal::ActivityLog::Kind::Helper,
                .exe = L"wscript.exe",
                .commandLine = args,
                .pid = info.hProcess ? GetProcessId(info.hProcess) : 0u,
                .parentPid = GetCurrentProcessId(),
                .reason = relaunch ? L"slot promotion helper (will relaunch)" : L"slot promotion helper",
            });
        }
        CATCH_LOG()

        if (info.hProcess)
        {
            CloseHandle(info.hProcess);
        }
        return true;
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return false;
    }

    // Keep the CI-build-poller scheduled task in step with the user's configured
    // interval, so the setting in Compatibility.xaml is more than decoration.
    //
    // Only the Dev slot touches this: the task is one machine-wide Scheduled
    // Task, not one per slot, and it exists to keep the Dev slot's promote
    // button fresh. Letting the Test slot's own copy of the setting fight over
    // the same task would just mean whichever window started last wins, for no
    // benefit -- the Test slot has nothing that reads dev-pending*.json.
    //
    // Fire-and-forget like _armPromotionHelper, and for the same reason:
    // Register-ScheduledTask is a sub-second call, but nothing on this path
    // should ever block a window from opening. Install-CIBuildPoller.ps1's own
    // -Force registration is idempotent, so calling this once per window
    // startup -- guarded here to once per process -- costs nothing when the
    // interval hasn't changed.
    void TerminalPage::_ReconcileCiPollInterval()
    try
    {
        static std::atomic_bool reconciled{ false };
        if (reconciled.exchange(true))
        {
            return;
        }

        if (!::TerminalApp::SlotPromotion::ThisSlotCanBePromoted())
        {
            return;
        }

        const auto helper{ std::filesystem::path{ ::TerminalApp::SlotPromotion::SlotRoot } / L"Install-CIBuildPoller.ps1" };
        if (!std::filesystem::exists(helper))
        {
            return;
        }

        // Same Invoke-Hidden.vbs routing as _armPromotionHelper, and for the
        // same reason -- see that function's comment.
        const auto hiddenLauncher{ std::filesystem::path{ ::TerminalApp::SlotPromotion::SlotRoot } / L"Invoke-Hidden.vbs" };
        const auto intervalMinutes{ _settings.GlobalSettings().CiPollIntervalMinutes() };
        const auto args{ intervalMinutes > 0 ?
                             fmt::format(LR"("{}" "pwsh.exe" "-NoProfile" "-NonInteractive" "-ExecutionPolicy" "Bypass" "-File" "{}" "-IntervalMinutes" "{}")", hiddenLauncher.wstring(), helper.wstring(), intervalMinutes) :
                             fmt::format(LR"("{}" "pwsh.exe" "-NoProfile" "-NonInteractive" "-ExecutionPolicy" "Bypass" "-File" "{}" "-Uninstall")", hiddenLauncher.wstring(), helper.wstring()) };

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"open";
        info.lpFile = L"wscript.exe";
        info.lpParameters = args.c_str();
        info.nShow = SW_HIDE;

        if (ShellExecuteExW(&info))
        {
            // The scheduled task this registers is itself a later spawn source
            // the log cannot see, so record what reconciled it: when a console
            // window appears on the poller's schedule, this entry is the link
            // between the two.
            try
            {
                ::Microsoft::Terminal::ActivityLog::Record({
                    .kind = ::Microsoft::Terminal::ActivityLog::Kind::Helper,
                    .exe = L"wscript.exe",
                    .commandLine = args,
                    .pid = info.hProcess ? GetProcessId(info.hProcess) : 0u,
                    .parentPid = GetCurrentProcessId(),
                    .reason = intervalMinutes > 0 ? L"CI poll task reconcile" : L"CI poll task uninstall",
                });
            }
            CATCH_LOG()

            if (info.hProcess)
            {
                CloseHandle(info.hProcess);
            }
        }
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
    }

    // The badge says which build this window is; this puts that answer on the
    // clipboard, because the place you need it is a bug report or a message to
    // someone else, and reading it off a tooltip to retype it is the whole
    // friction. Same text the About dialog copies -- see BuildInfo::ClipboardText.
    void TerminalPage::_CopyBuildInfoRequested(const IInspectable& /*sender*/, const IInspectable& /*args*/)
    {
        using namespace winrt::Windows::ApplicationModel::DataTransfer;
        using namespace winrt::Microsoft::Terminal::Settings::Model;

        DataPackage package;
        package.SetText(::TerminalApp::BuildInfo::ClipboardText(CascadiaSettings::ApplicationDisplayName(),
                                                                CascadiaSettings::ApplicationVersion()));
        Clipboard::SetContent(package);

        // The badge does not change when clicked, so without a confirmation a
        // successful copy and a dead button look identical.
        if (_buildInfoCopiedToast == nullptr)
        {
            if (auto tip{ FindName(L"BuildInfoCopiedToast").try_as<winrt::Microsoft::UI::Xaml::Controls::TeachingTip>() })
            {
                _buildInfoCopiedToast = std::make_shared<Toast>(tip);
                // IsLightDismissEnabled == true is bugged in Xaml Islands: another
                // window opening a tip dismisses this one immediately (MUX#4382).
                tip.IsLightDismissEnabled(false);
                tip.Closed({ get_weak(), &TerminalPage::_FocusActiveControl });
            }
        }
        _UpdateTeachingTipTheme(BuildInfoCopiedToast().try_as<winrt::Windows::UI::Xaml::FrameworkElement>());

        if (_buildInfoCopiedToast != nullptr)
        {
            _buildInfoCopiedToast->Open();
        }
    }
}
