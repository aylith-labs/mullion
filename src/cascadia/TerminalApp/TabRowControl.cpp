// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TabRowControl.h"
#include "BuildInfo.h"

#include "TabRowControl.g.cpp"

using namespace winrt::Windows::ApplicationModel::DataTransfer;

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Windows::UI::Text;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
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
        while (_pipeCalls.size() > MaxListedPipeCalls)
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

        auto children = list.Children();
        children.Clear();

        const WUX::Media::FontFamily mono{ L"Cascadia Mono, Consolas, monospace" };

        WUX::Controls::TextBlock title;
        title.Text(RS_(L"PipeActivity_FlyoutTitle"));
        title.FontWeight(FontWeights::SemiBold());
        title.Margin(WUX::Thickness{ 0, 0, 0, 4 });
        children.Append(title);

        for (const auto& call : _pipeCalls)
        {
            // "14:03:07  send-input 1.0.3  ok" - and a failure is marked, because
            // a refused write is the row worth noticing in a list of fine ones.
            std::wstring headline{ std::wstring_view{ call.Time } };
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

            // Who asked, and what for - "powershell.exe (4242)  \"echo hi\\r\"".
            std::wstring context{ std::wstring_view{ call.Client } };
            if (!call.Detail.empty())
            {
                context.append(L"  ");
                context.append(std::wstring_view{ call.Detail });
            }

            WUX::Controls::TextBlock second;
            second.Text(winrt::hstring{ context });
            second.FontFamily(mono);
            second.FontSize(11);
            second.Opacity(0.7);
            second.TextWrapping(WUX::TextWrapping::NoWrap);
            second.TextTrimming(WUX::TextTrimming::CharacterEllipsis);

            WUX::Controls::StackPanel row;
            row.Children().Append(first);
            row.Children().Append(second);
            children.Append(row);
        }

        WUX::Controls::HyperlinkButton showAll;
        showAll.Content(winrt::box_value(RS_(L"PipeActivity_ShowAll")));
        showAll.Margin(WUX::Thickness{ 0, 4, 0, 0 });
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
        children.Append(showAll);
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
