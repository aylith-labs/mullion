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
// * Only focus-pane and run-action move focus. focus-pane because moving it is
//   its whole job; run-action only inside the window, because an action acts on
//   the focused pane and it never raises the window. Listing, capturing and
//   writing never focus, activate, summon or switch anything: a control client
//   has to be able to read and write a pane in a background tab of a minimised
//   window without the user's focus moving a pixel.
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

    // Runs one action as if its key binding had been pressed in that pane.
    //
    // The vocabulary is settings.json's, because the parse is the settings
    // model's own: ActionAndArgs::Deserialize is what reads a serialized action
    // list everywhere else, so a name or an argument it would not accept in a
    // settings file is refused here the same way.
    //
    // A key binding is dispatched with no sender, so its handler acts on the
    // focused tab and its active pane. Making the target that pane first is
    // therefore what "in that pane" means, and it is done the way
    // ControlPipeFocusPane does it minus the summon: the tab is selected and
    // the pane made active inside this window, and the window itself is
    // neither raised nor activated. Each step is skipped when it is already
    // true, so an action on the pane the user is looking at moves nothing.
    TerminalApp::ControlPipeActionResult TerminalPage::ControlPipeRunAction(uint32_t tabIndex, uint32_t paneId, hstring action)
    {
        TerminalApp::ControlPipeActionResult result{};
        result.Status = TerminalApp::ControlPipeStatus::NoSuchPane;

        const auto pane = _controlPipeFindPane(tabIndex, paneId);
        if (!pane || !pane->GetTerminalControl())
        {
            return result;
        }

        winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs parsed{ nullptr };
        try
        {
            // Deserialize reads an "actions" array of exactly what settings.json
            // holds under "command", which is what the client sent.
            const auto list = winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs::Deserialize(
                hstring{ L"{\"actions\":[" + std::wstring{ action } + L"]}" });
            if (list && list.Size() == 1)
            {
                parsed = list.GetAt(0);
            }
        }
        catch (...)
        {
            // A client's malformed action is the client's error, answered as
            // bad-action below; it is not worth an app error in the log.
        }

        // Invalid is what the model makes of an unknown name, of "unbound", and
        // of arguments its parser refused.
        if (!parsed || parsed.Action() == winrt::Microsoft::Terminal::Settings::Model::ShortcutAction::Invalid)
        {
            result.Status = TerminalApp::ControlPipeStatus::BadAction;
            return result;
        }

        // _controlPipeFindPane already proved the tab and its root exist.
        const auto tabImpl = _GetTabImpl(_tabs.GetAt(tabIndex));
        if (const auto focusedTab = _GetFocusedTabIndex(); !focusedTab || *focusedTab != tabIndex)
        {
            _SelectTab(tabIndex);
        }
        if (tabImpl->GetActivePane() != pane)
        {
            if (tabImpl->IsZoomed())
            {
                _UnZoomIfNeeded();
            }
            tabImpl->FocusPane(paneId);
        }

        result.Status = TerminalApp::ControlPipeStatus::Ok;
        result.Handled = _actionDispatch->DoAction(parsed);
        return result;
    }

    // A request was answered on this process's pipe. The tab row counts it and
    // lists the latest few; recording it durably already happened, in the
    // activity log, on the pipe thread.
    void TerminalPage::ControlPipeCallRecorded(const TerminalApp::ControlPipeCall& call)
    {
        if (_tabRow)
        {
            _tabRow.AddControlPipeCall(call);
        }
    }

    // "Show all" under the badge's list: the Activity page, narrowed to pipe
    // requests. OpenSettingsUI makes or focuses the settings tab, and either way
    // leaves _settingsUI live - the same route the link tooltip's rule link takes.
    void TerminalPage::_ShowAllControlPipeCallsRequested(const Windows::Foundation::IInspectable& /*sender*/, const Windows::Foundation::IInspectable& /*args*/)
    {
        OpenSettingsUI();

        if (const auto sui = _settingsUI.get())
        {
            sui.NavigateToActivity(L"controlPipe");
        }
    }
}
