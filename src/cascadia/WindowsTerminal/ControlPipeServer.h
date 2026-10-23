/*++
Copyright (c) Microsoft Corporation Licensed under the MIT license.

Class Name:
- ControlPipeServer.h

Abstract:
- Serves the control pipe, \\.\pipe\wt-control-<pid>, so that a local process
  running as this user can enumerate this Terminal's panes, read what is on a
  pane's screen, and write text into a pane's connection.
- All of the pipe I/O happens on the threads owned by this class. Anything that
  needs to touch a pane is handed to WindowEmperor's message window with
  SendMessageTimeout and runs on the UI thread; a client that stops reading, or
  dies mid-request, blocks nothing but its own thread here.
- See doc/control-pipe.md for the wire format and the reasoning.

--*/

#pragma once

#include "ControlPipeProtocol.h"

#include <functional>

// One request as it is recorded: who asked, what they asked for, and how it
// ended. Built on the pipe thread once the response is known, and handed to the
// observer the server was constructed with. Every request produces one,
// including a line that would not parse, because a client sending garbage into
// the pipe is exactly the one worth knowing about.
struct ControlPipeCallRecord
{
    ControlPipe::CallDescription description;
    bool ok{};
    // "ok", or the error code the client was answered with.
    std::wstring result;
    // What came back, where that says something: "3 panes", "812 chars",
    // the pane focus-pane landed on.
    std::wstring note;

    // The process on the other end of the pipe, and its parent - the client is
    // often a short-lived powershell.exe, and its parent is what started it.
    DWORD clientPid{};
    std::wstring clientExe;
    DWORD clientParentPid{};
    std::wstring clientParentExe;
    // The parent and its ancestors, nearest first: "wslhost.exe 20440 <
    // wsl.exe 1234". What the Windows side can trace on its own; past
    // wslhost.exe only the request's own `caller` can say.
    std::wstring clientAncestry;
};

// One of these crosses to the UI thread per request. The pipe thread owns it
// for the duration of the SendMessage and reads the results afterwards; the
// emperor only writes the out-fields.
struct ControlPipeExchange
{
    // In.
    const ControlPipe::Request* request{ nullptr };

    // Out. When set, everything else is ignored and this becomes the response.
    std::optional<ControlPipe::Error> error;
    // ping
    std::vector<uint64_t> windows;
    // list-panes. `process` is left empty here and filled in off the UI thread.
    std::vector<ControlPipe::PaneEntry> panes;
    // capture-pane
    std::wstring text;
    // focus-pane: the pane that was focused. run-action: the pane it ran in.
    ControlPipe::PaneAddress focused;
    // run-action: whether anything handled the action.
    bool handled{};
};

class ControlPipeServer
{
public:
    // Called on a pipe thread after every request, response already sent. It
    // must not block for long and must not touch XAML; post to the UI thread.
    using Observer = std::function<void(const ControlPipeCallRecord&)>;

    ControlPipeServer(HWND messageWindow, UINT requestMessage, Observer observer = {}) noexcept;
    ~ControlPipeServer();

    ControlPipeServer(const ControlPipeServer&) = delete;
    ControlPipeServer& operator=(const ControlPipeServer&) = delete;

    // Returns false (and logs) if the pipe couldn't be created. The Terminal
    // carries on regardless: a control pipe that won't open is a missing
    // convenience, not a reason to fail startup.
    bool Start();
    void Stop() noexcept;

    static std::wstring PipeNameForProcess(DWORD pid);

private:
    static constexpr DWORD MaxInstances = 4;

    struct Client
    {
        DWORD pid{};
        std::wstring exe;
        DWORD parentPid{};
        std::wstring parentExe;
        std::wstring ancestry;
    };

    // How a dispatched request ended, for the record.
    struct Outcome
    {
        std::optional<ControlPipe::Error> error;
        std::wstring note;
    };

    void _worker(wil::unique_hfile firstInstance) noexcept;
    wil::unique_hfile _createInstance(bool first) const noexcept;
    void _serve(HANDLE pipe) noexcept;
    std::string _dispatch(const ControlPipe::Request& request, Outcome& outcome) const;
    static Client _identifyClient(HANDLE pipe) noexcept;
    void _report(const Client& client, ControlPipe::CallDescription description, const Outcome& outcome) const noexcept;

    bool _readLine(HANDLE pipe, std::string& buffer, std::string& line) const;
    bool _write(HANDLE pipe, std::string_view bytes) const noexcept;

    // A handle has to be in _active before its thread blocks on it, or Stop()
    // has nothing to cancel and the join never completes. _track answers false
    // if we're already stopping, which is the other half of that race.
    bool _track(HANDLE pipe) noexcept;
    void _untrack(HANDLE pipe) noexcept;

    HWND _messageWindow{ nullptr };
    UINT _requestMessage{ 0 };
    Observer _observer;
    std::wstring _name;
    wil::unique_hlocal_security_descriptor _securityDescriptor;
    wil::unique_event _stop{ wil::EventOptions::ManualReset };
    std::vector<std::thread> _threads;
    std::mutex _activeLock;
    std::vector<HANDLE> _active;
};
