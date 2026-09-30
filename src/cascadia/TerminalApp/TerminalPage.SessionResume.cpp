// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Working out what each pane is running, and stashing the command that would
// bring it back on the pane itself, so that when the layout is serialized
// moments later GetNewTerminalArgs can simply read it off.
//
// The split between the two entry points here is about who is still alive:
//
//   RefreshResumeCommands    the periodic path. A coroutine, because the
//                            message pump is running and the expensive part
//                            can be done on a background thread and applied
//                            back on the UI thread.
//   CaptureResumeCommandsNow the shutdown path. The pump is gone by the time
//                            WindowEmperor finalizes persistence, so a
//                            coroutine would never resume. Runs the same work
//                            on a detached thread and waits with a deadline,
//                            keeping whatever the periodic pass last cached
//                            if that deadline passes.

#include "pch.h"
#include "TerminalPage.h"
#include "Tab.h"
#include "TerminalPaneContent.h"
#include "PaneSessionCapture.h"
#include "../../types/inc/utils.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt
{
    namespace WUX = Windows::UI::Xaml;
    namespace MUX = Microsoft::UI::Xaml;
}

namespace winrt::TerminalApp::implementation
{
    // Whether anything is eligible at all. Gates three things: the capture
    // work itself (with everything off, no amount of process walking could
    // produce a result), seeding a restored pane, and actually running what a
    // saved layout asks for -- so turning the feature off stops commands that
    // were recorded while it was on, rather than leaving them to replay out of
    // a layout written earlier.
    bool TerminalPage::_resumeEnabled() const
    {
        if (!_settings)
        {
            return false;
        }
        const auto globals = _settings.GlobalSettings();
        if (globals.ResumeAgents() || globals.ResumeMultiplexers())
        {
            return true;
        }
        const auto extra = globals.ResumeExtraPrograms();
        return extra && extra.Size() > 0;
    }

    static ::TerminalApp::SessionResume::Policy _policyFrom(const CascadiaSettings& settings)
    {
        const auto globals = settings.GlobalSettings();
        ::TerminalApp::SessionResume::Policy policy;
        policy.Agents = globals.ResumeAgents();
        policy.Multiplexers = globals.ResumeMultiplexers();
        if (const auto extra = globals.ResumeExtraPrograms())
        {
            for (const auto& entry : extra)
            {
                policy.Extra.emplace_back(entry);
            }
        }
        if (const auto excluded = globals.ResumeExcludedPrograms())
        {
            for (const auto& entry : excluded)
            {
                policy.Excluded.emplace_back(entry);
            }
        }
        return policy;
    }

    // One probe per live terminal pane. Must run on the UI thread: it touches
    // the pane tree and every control in it.
    std::vector<::TerminalApp::SessionResume::PaneProbe> TerminalPage::_collectResumeProbes()
    {
        std::vector<::TerminalApp::SessionResume::PaneProbe> probes;

        for (const auto& tab : _tabs)
        {
            const auto tabImpl = _GetTabImpl(tab);
            if (!tabImpl)
            {
                continue;
            }
            const auto rootPane = tabImpl->GetRootPane();
            if (!rootPane)
            {
                continue;
            }

            rootPane->WalkTree([&](const auto& pane) {
                const auto control = pane->GetTerminalControl();
                if (!control)
                {
                    return;
                }
                const auto connection = control.Connection();
                // A pane whose shell has already exited has nothing running,
                // and its recorded pid may since have been reused by an
                // unrelated process -- the same check ControlPipe makes before
                // trusting a pane's identity.
                if (!connection || connection.State() != ConnectionState::Connected)
                {
                    return;
                }
                const auto sessionId = connection.SessionId();
                if (sessionId == winrt::guid{})
                {
                    return;
                }

                ::TerminalApp::SessionResume::PaneProbe probe;
                probe.SessionId = ::Microsoft::Console::Utils::GuidToPlainString(sessionId);
                probe.Cwd = control.WorkingDirectory();

                if (const auto conpty = connection.try_as<ConptyConnection>())
                {
                    if (const auto handle = conpty.RootProcessHandle())
                    {
                        probe.RootPid = ::GetProcessId(reinterpret_cast<HANDLE>(handle));
                    }
                    // The distro comes from the profile's own launch command,
                    // so a Debian pane is probed as Debian. Empty means this is
                    // a native Windows pane.
                    const auto settings = control.Settings();
                    probe.Distro = ::Microsoft::Console::Utils::WslDistroForCommandline(
                        conpty.Commandline(),
                        settings && settings.PathTranslationStyle() == winrt::Microsoft::Terminal::Control::PathTranslationStyle::WSL);
                }

                probes.push_back(std::move(probe));
            });
        }

        return probes;
    }

    // Hand each plan back to the pane it came from. Re-walks rather than
    // holding pane pointers across the background hop, because a pane can be
    // closed while the probe is still running.
    void TerminalPage::_applyResumeCommands(const std::map<std::wstring, winrt::hstring>& commands, const std::set<std::wstring>& unknown)
    {
        for (const auto& tab : _tabs)
        {
            const auto tabImpl = _GetTabImpl(tab);
            if (!tabImpl)
            {
                continue;
            }
            const auto rootPane = tabImpl->GetRootPane();
            if (!rootPane)
            {
                continue;
            }

            rootPane->WalkTree([&](const auto& pane) {
                const auto control = pane->GetTerminalControl();
                if (!control)
                {
                    return;
                }
                const auto connection = control.Connection();
                if (!connection)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto termContent = content.try_as<winrt::TerminalApp::TerminalPaneContent>();
                if (!termContent)
                {
                    return;
                }

                const auto key = ::Microsoft::Console::Utils::GuidToPlainString(connection.SessionId());
                // Not probed this round (WSL busy, backing off, or timed
                // out): keep whatever the last answered round decided.
                if (unknown.count(key) != 0)
                {
                    return;
                }
                const auto found = commands.find(key);
                const auto impl = winrt::get_self<implementation::TerminalPaneContent>(termContent);
                // A pane that no longer resolves to anything resumable is
                // cleared, not left holding a stale command from last time.
                impl->ResumeCommand(found == commands.end() ? winrt::hstring{} : found->second);
            });
        }
    }

    namespace
    {
        struct ResumeCapture
        {
            std::map<std::wstring, winrt::hstring> Commands;
            std::set<std::wstring> Unknown;
        };
    }

    static ResumeCapture _plansFor(const std::vector<::TerminalApp::SessionResume::PaneProbe>& probes,
                                   const ::TerminalApp::SessionResume::Policy& policy,
                                   bool urgent)
    {
        ResumeCapture result;
        auto captured = ::TerminalApp::SessionResume::Capture(probes, urgent);
        for (const auto& pane : captured.Panes)
        {
            if (const auto plan = ::TerminalApp::SessionResume::BuildPlan(pane, policy))
            {
                result.Commands.emplace(pane.SessionId, winrt::hstring{ plan->CommandLine });
            }
        }
        result.Unknown.insert(std::make_move_iterator(captured.Unknown.begin()), std::make_move_iterator(captured.Unknown.end()));
        return result;
    }

    // Periodic path. Fire and forget: the persist pass that follows uses
    // whatever the last completed round cached, which is at most one interval
    // stale and costs the UI thread nothing.
    safe_void_coroutine TerminalPage::RefreshResumeCommands()
    {
        if (!_resumeEnabled())
        {
            co_return;
        }

        auto strongThis{ get_strong() };
        const auto policy = _policyFrom(_settings);
        auto probes = _collectResumeProbes();
        if (probes.empty())
        {
            co_return;
        }

        const auto dispatcher = Dispatcher();
        co_await winrt::resume_background();

        // Empty on a throw: then nothing is applied, rather than every pane's
        // command being cleared as if it had been found running nothing.
        std::optional<ResumeCapture> capture;
        try
        {
            capture = _plansFor(probes, policy, false);
        }
        CATCH_LOG();

        // Back to the UI thread either way, so strongThis is released there.
        co_await wil::resume_foreground(dispatcher);
        if (capture)
        {
            _applyResumeCommands(capture->Commands, capture->Unknown);
        }
    }

    // Shutdown path. Bounded, because this runs while the app is on its way
    // out and a wedged distro must not be able to hold the process open.
    void TerminalPage::CaptureResumeCommandsNow(uint32_t timeoutMs)
    try
    {
        if (!_resumeEnabled())
        {
            return;
        }

        const auto policy = _policyFrom(_settings);
        auto probes = _collectResumeProbes();
        if (probes.empty())
        {
            return;
        }

        struct Shared
        {
            std::mutex Mutex;
            std::condition_variable Ready;
            bool Done{ false };
            std::optional<ResumeCapture> Capture;
        };
        const auto shared = std::make_shared<Shared>();

        // Detached, and every piece of state it touches is owned by the
        // shared_ptr it captured -- so when the wait below gives up, the
        // straggler can finish and tidy up after itself rather than writing
        // into a stack frame that is already gone. A wsl.exe it started is in
        // a kill-on-close job, so if the process exits under it the child
        // tree goes too rather than outliving us.
        //
        // Urgent: skips the healthy-probe spacing, so a pane that started an
        // agent seconds ago is still captured on the way out. The backoff and
        // single-flight still apply -- if the periodic probe is mid-flight or
        // WSL is known not to be answering, WSL panes keep their cached
        // command instead of this starting another wsl.exe.
        std::thread([shared, probes, policy]() {
            std::optional<ResumeCapture> capture;
            try
            {
                capture = _plansFor(probes, policy, true);
            }
            CATCH_LOG();

            {
                std::lock_guard guard{ shared->Mutex };
                shared->Capture = std::move(capture);
                shared->Done = true;
            }
            shared->Ready.notify_one();
        }).detach();

        std::unique_lock guard{ shared->Mutex };
        if (!shared->Ready.wait_for(guard, std::chrono::milliseconds{ timeoutMs }, [&] { return shared->Done; }))
        {
            // Keep what the periodic pass cached rather than clearing it.
            return;
        }

        auto capture = std::move(shared->Capture);
        guard.unlock();
        if (capture)
        {
            _applyResumeCommands(capture->Commands, capture->Unknown);
        }
    }
    CATCH_LOG()

    // Collected during the layout restore and flushed once, shortly after, so
    // every restored pane is known before anything is decided. "Ask first"
    // depends on that: one dialog listing everything beats one prompt per pane.
    void TerminalPage::_queueResumeCommand(const Microsoft::Terminal::Control::TermControl& control, const hstring& command)
    {
        _pendingResumes.emplace_back(control, command);

        if (!_resumeFlushQueued)
        {
            _resumeFlushQueued = true;
            _flushPendingResumes();
        }
    }

    safe_void_coroutine TerminalPage::_flushPendingResumes()
    {
        const auto dispatcher = Dispatcher();
        auto strongThis{ get_strong() };

        try
        {
            // Let the rest of the restore finish creating its panes. Every one
            // of them queues through here, and the first to arrive is the one
            // that scheduled this.
            co_await winrt::resume_after(std::chrono::milliseconds{ 500 });
            co_await wil::resume_foreground(dispatcher);

            auto pending = std::exchange(_pendingResumes, {});
            _resumeFlushQueued = false;
            if (pending.empty())
            {
                co_return;
            }

            std::wstring listing;
            for (const auto& item : pending)
            {
                listing.append(L"•  ");
                listing.append(item.second);
                listing.push_back(L'\n');
            }

            const auto mode = _settings.GlobalSettings().ResumeSessionNotification();

            if (mode == ResumeSessionNotification::Confirm)
            {
                const auto dialog = FindName(L"ResumeSessionsDialog").as<WUX::Controls::ContentDialog>();
                // BODGY: see _ShowConfirmCloseDialog -- once a ContentDialog has
                // been dismissed, FindName can no longer reach inside it, so the
                // body is fetched through Content() instead.
                if (const auto body = dialog.Content().try_as<WUX::Controls::TextBlock>())
                {
                    body.Text(winrt::hstring{ listing });
                }

                auto result = WUX::Controls::ContentDialogResult::None;
                if (const auto presenter{ _dialogPresenter.get() })
                {
                    result = co_await presenter.ShowDialog(dialog);
                }
                if (result != WUX::Controls::ContentDialogResult::Primary)
                {
                    // Declined. The panes keep the buffers they were restored
                    // with; nothing is typed into them.
                    co_return;
                }
            }

            for (const auto& item : pending)
            {
                _runResumeCommand(item.first, item.second);
            }

            if (mode == ResumeSessionNotification::Toast)
            {
                if (const auto infoBar = FindName(L"ResumedSessionsInfoBar").try_as<MUX::Controls::InfoBar>())
                {
                    infoBar.Message(winrt::hstring{ listing });
                    infoBar.IsOpen(true);
                }
            }
        }
        CATCH_LOG();
    }

    // Types the recorded command into a restored pane once its shell is up.
    //
    // Deliberately typed rather than launched: putting the command in
    // NewTerminalArgs::Commandline would make it the pane's ROOT process, so
    // the profile's own shell would never run and the pane would close the
    // moment the agent exited. Sending it as input runs the profile exactly as
    // configured and then the command inside it -- which is where it was when
    // we found it, and leaves a working shell behind when it ends.
    safe_void_coroutine TerminalPage::_runResumeCommand(Microsoft::Terminal::Control::TermControl control, hstring command)
    {
        const auto dispatcher = Dispatcher();
        auto strongThis{ get_strong() };

        try
        {
            // The connection doesn't start until the control has been sized,
            // and for a pane that also replays a saved buffer, not until that
            // replay finishes. Poll rather than guess how long that takes.
            for (auto attempt = 0; attempt < 100; ++attempt)
            {
                const auto connection = control.Connection();
                if (connection && connection.State() == ConnectionState::Connected)
                {
                    break;
                }
                co_await winrt::resume_after(std::chrono::milliseconds{ 100 });
                co_await wil::resume_foreground(dispatcher);
            }

            // Let the shell get through its startup files and draw a prompt.
            // Input arriving earlier is not lost -- the pty queues it -- but a
            // shell that is still running its rc files can echo it in pieces,
            // and a TUI launched into a half-initialized terminal redraws
            // badly.
            co_await winrt::resume_after(std::chrono::milliseconds{ 750 });
            co_await wil::resume_foreground(dispatcher);

            const auto connection = control.Connection();
            if (!connection || connection.State() != ConnectionState::Connected)
            {
                co_return;
            }

            // Enter goes as its own write: a lot of TUIs treat a single write
            // containing both the text and the newline as a bulk paste and
            // never submit it. The control pipe learned this the same way.
            control.SendInput(command);
            control.SendInput(L"\r");
        }
        CATCH_LOG();
    }
}
