/*++
Copyright (c) Microsoft Corporation Licensed under the MIT license.

Class Name:
- WindowEmperor.h

Abstract:
- The WindowEmperor is our class for managing the single Terminal process
  with all our windows. It will be responsible for handling the commandline
  arguments. It will initially try to find another terminal process to
  communicate with. If it does, it'll hand off to the existing process.
- If it determines that it should create a window, it will set up a new thread
  for that window, and a message loop on the main thread for handling global
  state, such as hotkeys and the notification icon.

--*/

#pragma once

#include "ControlPipeServer.h"

class AppHost;

class WindowEmperor
{
public:
    enum UserMessages : UINT
    {
        WM_CLOSE_TERMINAL_WINDOW = WM_USER,
        WM_MESSAGE_BOX_CLOSED,
        WM_IDENTIFY_ALL_WINDOWS,
        WM_NOTIFY_FROM_NOTIFICATION_AREA,
        WM_GET_WINDOW_LIST,
        // Sent (not posted) by a control pipe thread with a ControlPipeExchange*
        // in LPARAM. Handled synchronously so the pane work happens on this
        // thread and the answer is ready when SendMessage returns.
        WM_CONTROL_PIPE_REQUEST,
        // Posted by a control pipe thread after every request, with a
        // heap-allocated ControlPipeCallRecord* in LPARAM that the handler
        // takes ownership of. Posted, not sent: the client already has its
        // answer, and the tab row's badge can wait for the UI thread.
        WM_CONTROL_PIPE_ACTIVITY,
    };

    // Used by WM_GET_WINDOW_LIST.  Callers allocate a vector on their
    // stack and pass a pointer through LPARAM; the emperor fills it in
    // synchronously via SendMessage.
    struct WindowListEntry
    {
        uint64_t Id;
        std::wstring Name;
    };

    HWND GetMainWindow() const noexcept;
    AppHost* GetWindowById(uint64_t id) const noexcept;
    AppHost* GetWindowByName(std::wstring_view name) const noexcept;
    // CreateNewWindow is used for creating a new window from existing Content
    void CreateNewWindow(winrt::TerminalApp::WindowRequestedArgs args);
    void HandleCommandlineArgs(int nCmdShow);
    void FocusTabInAnyWindow(const winrt::TerminalApp::Tab& tab) const;
    // OpenWindow is used for opening a new window or summoning an existing window by name.
    void OpenWindow(const winrt::hstring& name);

private:
    struct SummonWindowSelectionArgs
    {
        uint64_t WindowID = 0;
        std::wstring_view WindowName;
        bool OnCurrentDesktop = false;
        winrt::TerminalApp::SummonWindowBehavior SummonBehavior;
    };

    [[nodiscard]] static LRESULT __stdcall _wndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;

    AppHost* _mostRecentWindow() const noexcept;
    void _createWindowMaybeRestoringWorkspace(uint64_t windowId, const winrt::hstring& windowName, winrt::TerminalApp::CommandlineArgs args);
    bool _summonWindow(const SummonWindowSelectionArgs& args) const;
    void _summonAllWindows() const;
    void _dispatchSpecialKey(const MSG& msg) const;
    void _dispatchCommandline(winrt::TerminalApp::CommandlineArgs args);
    void _dispatchCommandlineCommon(winrt::array_view<const winrt::hstring> args, wil::zwstring_view currentDirectory, wil::zwstring_view envString, uint32_t showWindowCommand);
    safe_void_coroutine _dispatchCommandlineCurrentDesktop(winrt::TerminalApp::CommandlineArgs args);
    LRESULT _messageHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    void _createMessageWindow(const wchar_t* className);
    void _postQuitMessageIfNeeded() const;
    void _armNoWindowWatchdog();
    safe_void_coroutine _showMessageBox(winrt::hstring message, bool error);
    void _notificationAreaMenuRequested(WPARAM wParam);
    void _notificationAreaMenuClicked(WPARAM wParam, LPARAM lParam) const;
    void _hotkeyPressed(long hotkeyIndex);
    void _registerHotKey(int index, const winrt::Microsoft::Terminal::Control::KeyChord& hotkey) noexcept;
    void _unregisterHotKey(int index) noexcept;
    void _setupGlobalHotkeys();
    static bool _shouldPersistPeriodically(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);
    void _setupSessionPersistence(bool enabled);
    void _persistState(const winrt::Microsoft::Terminal::Settings::Model::ApplicationState& state) const;
    void _finalizeSessionPersistence() const;
    void _persistBuffers() const;
    int64_t _bufferBytesOnDisk(const std::filesystem::path& settingsDirectory, std::wstring_view filenamePrefix) const;
    void _recordPersistCost(uint32_t panes, int64_t bytes, int64_t elapsedMicros) const;
    void _checkWindowsForNotificationIcon();
    void _setupAumid(const std::wstring& aumid);
    void _setupControlPipe(bool enabled);
    void _handleControlPipeRequest(ControlPipeExchange& exchange) const;
    void _broadcastControlPipeCall(const ControlPipeCallRecord& record) const;

    wil::unique_hwnd _window;
    winrt::TerminalApp::App _app{ nullptr };
    std::vector<std::shared_ptr<::AppHost>> _windows;
    std::vector<winrt::Microsoft::Terminal::Settings::Model::GlobalSummonArgs> _hotkeys;
    NOTIFYICONDATA _notificationIcon{};
    UINT WM_TASKBARCREATED = 0;
    HMENU _currentWindowMenu = nullptr;
    bool _notificationIconShown = false;
    bool _skipPersistence = false;
    bool _needsPersistenceCleanup = false;
    SafeDispatcherTimer _persistStateTimer;
    SafeDispatcherTimer _handoffTimeoutTimer;
    SafeDispatcherTimer _noWindowWatchdogTimer;
    std::optional<bool> _currentSystemThemeIsDark;
    int32_t _windowCount = 0;
    int32_t _messageBoxCount = 0;
    std::wstring _pendingAumidLnkPath;
    std::wstring _pendingAumid;
    std::unique_ptr<ControlPipeServer> _controlPipe;

#if 0 // #ifdef NDEBUG
    static constexpr void _assertIsMainThread() noexcept
    {
    }
#else
    void _assertIsMainThread() const noexcept
    {
        WI_ASSERT_MSG(_mainThreadId == GetCurrentThreadId(), "This part of WindowEmperor must be accessed from the UI thread");
    }
    DWORD _mainThreadId = GetCurrentThreadId();
#endif
};