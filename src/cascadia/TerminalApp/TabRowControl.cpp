// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TabRowControl.h"
#include "BuildInfo.h"

#include "TabRowControl.g.cpp"

#include <array>
#include <unordered_map>

using namespace winrt::Windows::ApplicationModel::DataTransfer;

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Windows::UI::Text;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
}

namespace
{
    // "12 seconds ago" - pipe requests come seconds apart, so under a minute
    // is worth counting - and BuildInfo's words from a minute up, so this and
    // the About dialog describe an age the same way.
    std::wstring _pipeTimeAgo(int64_t when)
    {
        const auto seconds = _time64(nullptr) - when;
        if (seconds < 5)
        {
            return L"just now";
        }
        if (seconds < 60)
        {
            return fmt::format(FMT_COMPILE(L"{} seconds ago"), seconds);
        }
        return ::TerminalApp::BuildInfo::RelativeAge(when);
    }

    // The user's short date and time, as the Activity page writes them.
    std::wstring _pipeDateTime(int64_t when)
    {
        // Unix seconds to a FILETIME, which counts 100 ns from 1601.
        ULARGE_INTEGER ticks{};
        ticks.QuadPart = (static_cast<uint64_t>(when) + 11'644'473'600ull) * 10'000'000ull;
        const FILETIME utcFile{ ticks.LowPart, ticks.HighPart };
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        std::array<wchar_t, 128> date{};
        std::array<wchar_t, 128> time{};
        if (FileTimeToSystemTime(&utcFile, &utc) &&
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) &&
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date.data(), gsl::narrow_cast<int>(date.size()), nullptr) &&
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, nullptr, time.data(), gsl::narrow_cast<int>(time.size())))
        {
            return fmt::format(FMT_COMPILE(L"{} {}"), std::wstring_view{ date.data() }, std::wstring_view{ time.data() });
        }
        return std::to_wstring(when);
    }
}

namespace winrt::TerminalApp::implementation
{
    TabRowControl::TabRowControl()
    {
        InitializeComponent();
    }

    winrt::hstring TabRowControl::SlotBadge()
    {
        return ::TerminalApp::BuildInfo::SlotBadge();
    }

    // Only the local Dev and Test slots get a badge. A normal install shows
    // nothing, so this costs a real user no tab-row space.
    bool TabRowControl::ShowSlotBadge()
    {
        return !SlotBadge().empty();
    }

    winrt::hstring TabRowControl::SlotBadgeTooltipTitle()
    {
        return winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::ApplicationDisplayName();
    }

    winrt::hstring TabRowControl::BuildCommit()
    {
        return ::TerminalApp::BuildInfo::Commit();
    }

    winrt::hstring TabRowControl::BuildBranch()
    {
        return ::TerminalApp::BuildInfo::Branch();
    }

    winrt::hstring TabRowControl::BuildTime()
    {
        return ::TerminalApp::BuildInfo::BuildTime();
    }

    winrt::hstring TabRowControl::BuildExePath()
    {
        return ::TerminalApp::BuildInfo::ExePath();
    }

    // The button only ever asks. Everything about what promotion costs, and the
    // decision itself, belongs to the dialog TerminalPage puts up in response.
    void TabRowControl::OnPromoteSlotButtonClick(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                 const winrt::Windows::UI::Xaml::RoutedEventArgs& /*args*/)
    {
        PromoteRequested.raise(*this, nullptr);
    }

    // The badge already says which build this is; clicking it puts that answer
    // somewhere you can paste it. Only asks, for the same reason the promote
    // button only asks: the clipboard and the confirmation are TerminalPage's.
    void TabRowControl::OnSlotBadgeClick(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                         const winrt::Windows::UI::Xaml::RoutedEventArgs& /*args*/)
    {
        CopyBuildInfoRequested.raise(*this, nullptr);
    }

    // A control pipe request was answered. The badge appears with the first one
    // and stays: once something has driven this Terminal through the pipe, that
    // is worth being able to see for the rest of the window's life.
    void TabRowControl::AddControlPipeCall(const winrt::TerminalApp::ControlPipeCall& call)
    {
        _pipeCalls.push_front(call);
        while (_pipeCalls.size() > MaxKeptPipeCalls)
        {
            _pipeCalls.pop_back();
        }

        ShowPipeActivity(true);
        _setUnseenPipeCalls(_unseenPipeCalls + 1);
    }

    void TabRowControl::_setUnseenPipeCalls(uint32_t count)
    {
        _unseenPipeCalls = count;
        HasUnseenPipeCalls(count > 0);
        UnseenPipeCallCount(count > 99 ? winrt::hstring{ L"99+" } : winrt::hstring{ std::to_wstring(count) });
    }

    // Opening the list is what "seen" means, so the count clears here. The rows
    // are built now rather than kept bound: the list is ten lines that only
    // matter while it is open, and a DataTemplate over a WinRT struct is more
    // machinery than that is worth.
    void TabRowControl::OnPipeActivityFlyoutOpening(const winrt::Windows::Foundation::IInspectable& sender,
                                                    const winrt::Windows::Foundation::IInspectable& /*args*/)
    {
        _setUnseenPipeCalls(0);
        if (const auto flyout = sender.try_as<WUX::Controls::Flyout>())
        {
            _rebuildPipeActivityList(flyout);
        }
    }

    void TabRowControl::_rebuildPipeActivityList(const WUX::Controls::Flyout& flyout)
    {
        const auto list = flyout.Content().try_as<WUX::Controls::StackPanel>();
        if (!list)
        {
            return;
        }

        // No wider than the monitor the badge was clicked on, less the
        // presenter's padding and a margin. The cursor stands in for the badge,
        // since XAML has no screen coordinates for it; from the keyboard it
        // picks the nearest monitor, which is the window's own nearly always.
        if (const auto root = XamlRoot())
        {
            POINT cursor{};
            MONITORINFO monitor{ sizeof(monitor) };
            if (::GetCursorPos(&cursor) &&
                ::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                const auto scale = root.RasterizationScale();
                const auto width = (monitor.rcWork.right - monitor.rcWork.left) / (scale > 0 ? scale : 1.0);
                list.MaxWidth(std::max(list.MinWidth(), width - 64));
            }
        }

        auto children = list.Children();
        children.Clear();

        const WUX::Media::FontFamily mono{ L"Cascadia Mono, Consolas, monospace" };

        WUX::Controls::TextBlock title;
        title.Text(RS_(L"PipeActivity_FlyoutTitle"));
        title.FontWeight(FontWeights::SemiBold());
        title.Margin(WUX::Thickness{ 0, 0, 0, 4 });
        children.Append(title);

        const auto formatTime = [absolute = _absolutePipeTimes](int64_t when) {
            return absolute ? _pipeDateTime(when) : _pipeTimeAgo(when);
        };

        // Repeats of one request - same program, same sender and reason, same
        // op on the same target asking the same thing, answered the same way -
        // collapse into the newest of them with a count. A one-shot client has
        // a new pid every call, so the pid is not part of what makes two the
        // same; the newest row's pid is the one shown.
        struct Row
        {
            const winrt::TerminalApp::ControlPipeCall* newest;
            int64_t oldest;
            uint32_t count;
        };
        std::vector<Row> rows;
        std::unordered_map<std::wstring, size_t> byKey;
        for (const auto& call : _pipeCalls)
        {
            if (!_groupPipeCalls)
            {
                if (rows.size() == MaxListedPipeRows)
                {
                    break;
                }
                rows.push_back({ &call, call.When, 1 });
                continue;
            }

            std::wstring key;
            for (const auto& part : { call.Program, call.Caller, call.Reason, call.Operation, call.Target, call.Detail })
            {
                key.append(std::wstring_view{ part });
                key.push_back(L'\n');
            }
            key.push_back(call.Ok ? L'1' : L'0');

            if (const auto found = byKey.find(key); found != byKey.end())
            {
                auto& row = rows[found->second];
                row.count++;
                row.oldest = call.When;
            }
            else if (rows.size() < MaxListedPipeRows)
            {
                byKey.emplace(std::move(key), rows.size());
                rows.push_back({ &call, call.When, 1 });
            }
        }

        for (const auto& row : rows)
        {
            const auto& call = *row.newest;

            // "3 minutes ago  send-input 1.0.3  ok" - and a failure is marked,
            // because a refused write is the row worth noticing in a list of
            // fine ones.
            auto headline = formatTime(call.When);
            headline.append(L"  ");
            if (!call.Ok)
            {
                headline.append(L"✗ ");
            }
            headline.append(std::wstring_view{ call.Operation });
            if (!call.Target.empty())
            {
                headline.push_back(L' ');
                headline.append(std::wstring_view{ call.Target });
            }
            headline.append(L"  ");
            headline.append(std::wstring_view{ call.Result });

            WUX::Controls::TextBlock first;
            first.Text(winrt::hstring{ headline });
            first.FontFamily(mono);
            first.FontSize(12);
            first.TextWrapping(WUX::TextWrapping::NoWrap);
            first.TextTrimming(WUX::TextTrimming::CharacterEllipsis);
            first.VerticalAlignment(WUX::VerticalAlignment::Center);

            WUX::Controls::StackPanel firstLine;
            firstLine.Orientation(WUX::Controls::Orientation::Horizontal);
            firstLine.Spacing(6);
            firstLine.Children().Append(first);
            if (row.count > 1)
            {
                // The same accent pill the Activity page puts on a group.
                WUX::Controls::TextBlock countText;
                countText.Text(winrt::hstring{ L"×" + std::to_wstring(row.count) });
                countText.Style(list.Resources().Lookup(winrt::box_value(L"PipeCountTextStyle")).as<WUX::Style>());

                WUX::Controls::Border pill;
                pill.Style(list.Resources().Lookup(winrt::box_value(L"PipeCountPillStyle")).as<WUX::Style>());
                pill.Child(countText);
                firstLine.Children().Append(pill);
            }

            // Why it was sent, in the sender's own words - the line that answers
            // "what is this", so it is not dimmed. Its absence is said outright.
            WUX::Controls::TextBlock why;
            why.Text(call.Reason.empty() ? RS_(L"PipeActivity_NoReason") : call.Reason);
            why.FontSize(12);
            why.TextWrapping(WUX::TextWrapping::NoWrap);
            why.TextTrimming(WUX::TextTrimming::CharacterEllipsis);
            if (call.Reason.empty())
            {
                why.FontStyle(FontStyle::Italic);
            }

            // Who sent it - its own name for itself, then the process on the
            // pipe - what it asked for, and for a group, since when.
            std::wstring context;
            if (!call.Caller.empty())
            {
                context.append(std::wstring_view{ call.Caller });
                context.append(L" · ");
            }
            context.append(std::wstring_view{ call.Client });
            if (!call.Detail.empty())
            {
                context.append(L"  ");
                context.append(std::wstring_view{ call.Detail });
            }
            if (row.count > 1)
            {
                // Two repeats in the same second, or the same minute in words,
                // would only print the headline's time again.
                if (const auto oldest = formatTime(row.oldest); oldest != formatTime(call.When))
                {
                    context.append(L"  · ");
                    context.append(RS_fmt(L"PipeActivity_FirstSeen", oldest));
                }
            }

            WUX::Controls::TextBlock second;
            second.Text(winrt::hstring{ context });
            second.FontFamily(mono);
            second.FontSize(11);
            second.Opacity(0.7);
            second.TextWrapping(WUX::TextWrapping::NoWrap);
            second.TextTrimming(WUX::TextTrimming::CharacterEllipsis);

            WUX::Controls::StackPanel entry;
            entry.Children().Append(firstLine);
            entry.Children().Append(why);
            entry.Children().Append(second);
            // The rest of the trace, where Windows can see it: the chain of
            // processes above the one that connected.
            if (!call.Ancestry.empty())
            {
                WUX::Controls::ToolTipService::SetToolTip(entry, winrt::box_value(winrt::hstring{ RS_fmt(L"PipeActivity_Chain", std::wstring_view{ call.Ancestry }) }));
            }
            children.Append(entry);
        }

        // The footer: the way to everything else, then the two view switches.
        WUX::Controls::Grid footer;
        footer.Margin(WUX::Thickness{ 0, 4, 0, 0 });
        footer.ColumnSpacing(12);
        {
            WUX::Controls::ColumnDefinition grow;
            grow.Width(WUX::GridLengthHelper::FromValueAndType(1, WUX::GridUnitType::Star));
            footer.ColumnDefinitions().Append(grow);
            for (auto i = 0; i < 2; ++i)
            {
                WUX::Controls::ColumnDefinition fit;
                fit.Width(WUX::GridLengthHelper::Auto());
                footer.ColumnDefinitions().Append(fit);
            }
        }

        // Says which form the times are in now; a click flips it.
        WUX::Controls::FontIcon clock;
        clock.FontSize(12);
        clock.Glyph(L"\xE823");
        WUX::Controls::TextBlock clockLabel;
        clockLabel.Text(_absolutePipeTimes ? RS_(L"PipeActivity_DateTime") : RS_(L"PipeActivity_TimeAgo"));
        clockLabel.VerticalAlignment(WUX::VerticalAlignment::Center);
        WUX::Controls::StackPanel clockContent;
        clockContent.Orientation(WUX::Controls::Orientation::Horizontal);
        clockContent.Spacing(6);
        clockContent.Children().Append(clock);
        clockContent.Children().Append(clockLabel);

        WUX::Controls::Button timeFormat;
        timeFormat.Content(clockContent);
        timeFormat.VerticalAlignment(WUX::VerticalAlignment::Center);
        WUX::Controls::ToolTipService::SetToolTip(timeFormat, winrt::box_value(RS_(L"PipeActivity_TimeFormatTip")));
        timeFormat.Click([weakThis = get_weak(), weakFlyout = winrt::make_weak(flyout)](const auto&, const auto&) {
            const auto self = weakThis.get();
            const auto open = weakFlyout.get();
            if (!self || !open)
            {
                return;
            }
            self->_absolutePipeTimes = !self->_absolutePipeTimes;
            self->_rebuildPipeActivityList(open);
        });
        WUX::Controls::Grid::SetColumn(timeFormat, 1);
        footer.Children().Append(timeFormat);

        WUX::Controls::CheckBox group;
        group.Content(winrt::box_value(RS_(L"PipeActivity_Group")));
        group.IsChecked(_groupPipeCalls);
        group.MinWidth(0);
        group.VerticalAlignment(WUX::VerticalAlignment::Center);
        WUX::Controls::Grid::SetColumn(group, 2);
        const auto toggled = [weakThis = get_weak(), weakFlyout = winrt::make_weak(flyout)](const winrt::Windows::Foundation::IInspectable& sender, const auto&) {
            const auto self = weakThis.get();
            const auto open = weakFlyout.get();
            const auto box = sender.try_as<WUX::Controls::CheckBox>();
            if (!self || !open || !box)
            {
                return;
            }
            self->_groupPipeCalls = box.IsChecked() && box.IsChecked().Value();
            self->_rebuildPipeActivityList(open);
        };
        group.Checked(toggled);
        group.Unchecked(toggled);
        footer.Children().Append(group);

        WUX::Controls::HyperlinkButton showAll;
        showAll.Content(winrt::box_value(RS_(L"PipeActivity_ShowAll")));
        showAll.VerticalAlignment(WUX::VerticalAlignment::Center);
        showAll.Click([weakThis = get_weak(), weakFlyout = winrt::make_weak(flyout)](const auto&, const auto&) {
            if (const auto open = weakFlyout.get())
            {
                open.Hide();
            }
            if (const auto self = weakThis.get())
            {
                self->ShowAllControlPipeCallsRequested.raise(*self, nullptr);
            }
        });
        footer.Children().Append(showAll);
        children.Append(footer);
    }

    // Method Description:
    // - Bound in the Xaml editor to the [+] button.
    // Arguments:
    // <unused>
    void TabRowControl::OnNewTabButtonClick(const IInspectable&, const Controls::SplitButtonClickEventArgs&)
    {
    }

    // Method Description:
    // - Bound in Drag&Drop of the Xaml editor to the [+] button.
    // Arguments:
    // <unused>
    void TabRowControl::OnNewTabButtonDrop(const IInspectable&, const winrt::Windows::UI::Xaml::DragEventArgs&)
    {
    }

    // Method Description:
    // - Bound in Drag-over of the Xaml editor to the [+] button.
    // Allows drop of 'StorageItems' which will be used as StartingDirectory
    // Arguments:
    //  - <unused>
    //  - e: DragEventArgs which hold the items
    void TabRowControl::OnNewTabButtonDragOver(const IInspectable&, const winrt::Windows::UI::Xaml::DragEventArgs& e)
    {
        // We can only handle drag/dropping StorageItems (files).
        // If the format on the clipboard is anything else, returning
        // early here will prevent the drag/drop from doing anything.
        if (!e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            return;
        }

        // Make sure to set the AcceptedOperation, so that we can later receive the path in the Drop event
        e.AcceptedOperation(DataPackageOperation::Copy);

        const auto modifiers = static_cast<uint32_t>(e.Modifiers());
        if (WI_IsFlagSet(modifiers, static_cast<uint32_t>(DragDrop::DragDropModifiers::Alt)))
        {
            e.DragUIOverride().Caption(RS_(L"DropPathTabSplit/Text"));
        }
        else if (WI_IsFlagSet(modifiers, static_cast<uint32_t>(DragDrop::DragDropModifiers::Shift)))
        {
            e.DragUIOverride().Caption(RS_(L"DropPathTabNewWindow/Text"));
        }
        else
        {
            e.DragUIOverride().Caption(RS_(L"DropPathTabRun/Text"));
        }

        // Sets if the caption is visible
        e.DragUIOverride().IsCaptionVisible(true);
        // Sets if the dragged content is visible
        e.DragUIOverride().IsContentVisible(false);
        // Sets if the glyph is visible
        e.DragUIOverride().IsGlyphVisible(false);
    }
}
