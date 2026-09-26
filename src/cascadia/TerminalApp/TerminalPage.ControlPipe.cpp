// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The window-side half of the control pipe (see doc/control-pipe.md).
//
// WindowEmperor runs the named pipe server on background threads and marshals
// every request onto the UI thread before it gets here, so everything in this
// file runs on the UI thread and can touch panes directly.
//
// Two rules this file exists to keep:
//
// * Only focus-pane moves focus, because moving it is its whole job. Listing,
//   capturing and writing never focus, activate, summon or switch anything: a
//   control client has to be able to read and write a pane in a background tab
//   of a minimised window without the user's focus moving a pixel.
// * Input goes to the connection, never through the keyboard. We call the same
//   TermControl::SendInput the sendInput action uses.

#include "pch.h"
#include "TerminalPage.h"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::TerminalApp::implementation
{
    // Find the leaf pane with this id in this tab. Returns nullptr if either the
    // tab index or the pane id doesn't name a live pane.
    std::shared_ptr<Pane> TerminalPage::_controlPipeFindPane(uint32_t tabIndex, uint32_t paneId) const
    {
        if (tabIndex >= _tabs.Size())
        {
            return nullptr;
        }

        const auto tabImpl = _GetTabImpl(_tabs.GetAt(tabIndex));
        if (!tabImpl)
        {
            return nullptr;
        }

        const auto rootPane = tabImpl->GetRootPane();
        if (!rootPane)
        {
            return nullptr;
        }

        return rootPane->FindPane(paneId);
    }

    IVector<TerminalApp::ControlPipePaneInfo> TerminalPage::ControlPipeListPanes(hstring containing)
    {
        auto results = single_threaded_vector<TerminalApp::ControlPipePaneInfo>();
        const auto focusedTabIndex = _GetFocusedTabIndex();

        for (uint32_t tabIndex = 0; tabIndex < _tabs.Size(); tabIndex++)
        {
            const auto tabImpl = _GetTabImpl(_tabs.GetAt(tabIndex));
            if (!tabImpl)
            {
                continue;
            }

            const auto rootPane = tabImpl->GetRootPane();
            if (!rootPane)
            {
                continue;
            }

            const auto tabIsActive = focusedTabIndex.has_value() && focusedTabIndex.value() == tabIndex;
            const auto activePane = tabImpl->GetActivePane();

            rootPane->WalkTree([&](const auto& pane) {
                const auto id = pane->Id();
                if (!id)
                {
                    // A parent node in the split tree, not a pane you can address.
                    return;
                }

                const auto control = pane->GetTerminalControl();
                if (!control)
                {
                    // Some other kind of pane content (the settings tab, a
                    // scratchpad). It has no buffer and no connection, so
                    // there's nothing a control client could do with it.
                    return;
                }

                // The filter runs in here, against the pane's own screen, so
                // that a polling client never has to pull buffers across the
                // pipe just to find the one pane it cares about.
                if (!containing.empty() && !control.ViewportContains(containing))
                {
                    return;
                }

                TerminalApp::ControlPipePaneInfo info{};
                info.TabIndex = tabIndex;
                info.PaneId = id.value();
                info.Focused = tabIsActive && activePane && activePane == pane;

                if (const auto content = pane->GetContent())
                {
                    info.Title = content.Title();
                }

                if (const auto connection = control.Connection())
                {
                    info.SessionId = connection.SessionId();
                    // The same test send-input uses, so a client that checks
                    // this first and a client that just writes and reads the
                    // error can never disagree about a pane.
                    info.Alive = connection.State() == ConnectionState::Connected;

                    if (const auto conpty = connection.try_as<ConptyConnection>())
                    {
                        if (const auto handle = conpty.RootProcessHandle())
                        {
                            info.ProcessId = ::GetProcessId(reinterpret_cast<HANDLE>(handle));
                        }
                    }
                }

                results.Append(info);
            });
        }

        return results;
    }

    TerminalApp::ControlPipeCaptureResult TerminalPage::ControlPipeCapturePane(uint32_t tabIndex, uint32_t paneId, int32_t lines)
    {
        TerminalApp::ControlPipeCaptureResult result{};
        result.Status = TerminalApp::ControlPipeStatus::NoSuchPane;

        const auto pane = _controlPipeFindPane(tabIndex, paneId);
        if (!pane)
        {
            return result;
        }

        const auto control = pane->GetTerminalControl();
        if (!control)
        {
            return result;
        }

        result.Status = TerminalApp::ControlPipeStatus::Ok;
        result.Text = control.ReadViewportText(lines);
        return result;
    }

    TerminalApp::ControlPipeStatus TerminalPage::ControlPipeSendInput(uint32_t tabIndex, uint32_t paneId, hstring text, hstring requireContains)
    {
        const auto pane = _controlPipeFindPane(tabIndex, paneId);
        if (!pane)
        {
            return TerminalApp::ControlPipeStatus::NoSuchPane;
        }

        const auto control = pane->GetTerminalControl();
        if (!control)
        {
            return TerminalApp::ControlPipeStatus::NoSuchPane;
        }

        const auto connection = control.Connection();
        if (!connection || connection.State() != ConnectionState::Connected)
        {
            return TerminalApp::ControlPipeStatus::Disconnected;
        }

        // The guard, and the reason this is one call rather than a
        // capture-then-send pair on the client: between a client's find and its
        // write, tabs can be reordered, panes closed and pane ids reused. Every
        // one of those turns a correct pane address into a wrong one, and the
        // failure mode of a wrong pane address is a command typed into somebody
        // else's shell. Re-checking here means the pane cannot change identity
        // between the check and the write - we never yield in between.
        if (!requireContains.empty() && !control.ViewportContains(requireContains))
        {
            return TerminalApp::ControlPipeStatus::NeedleGone;
        }

        // Verbatim: no newline appended, no bracketed paste, no escaping. The
        // caller sends exactly the characters it wants the shell to receive,
        // and sends the Enter as its own call - a lot of TUIs treat one write
        // containing both text and \r as a bulk paste and never submit it.
        control.SendInput(text);
        return TerminalApp::ControlPipeStatus::Ok;
    }

    // Selects the pane's tab and focuses the pane within it. Raising the window
    // is WindowEmperor's half, since only it can summon an AppHost.
    TerminalApp::ControlPipeStatus TerminalPage::ControlPipeFocusPane(uint32_t tabIndex, uint32_t paneId)
    {
        const auto pane = _controlPipeFindPane(tabIndex, paneId);
        if (!pane || !pane->GetTerminalControl())
        {
            return TerminalApp::ControlPipeStatus::NoSuchPane;
        }

        // _controlPipeFindPane already proved the tab and its root exist.
        const auto tabImpl = _GetTabImpl(_tabs.GetAt(tabIndex));
        _SelectTab(tabIndex);

        // A zoomed tab shows only its active pane. Unzoom only when the target
        // is some other pane, so focusing the zoomed pane itself keeps the zoom.
        if (tabImpl->IsZoomed() && tabImpl->GetActivePane() != pane)
        {
            _UnZoomIfNeeded();
        }
        tabImpl->FocusPane(paneId);
        return TerminalApp::ControlPipeStatus::Ok;
    }
}
