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
    // focus-pane: the pane that was focused.
    ControlPipe::PaneAddress focused;
};

class ControlPipeServer
{
public:
    ControlPipeServer(HWND messageWindow, UINT requestMessage) noexcept;
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

    void _worker(wil::unique_hfile firstInstance) noexcept;
    wil::unique_hfile _createInstance(bool first) const noexcept;
    void _serve(HANDLE pipe) noexcept;
    std::string _dispatch(const ControlPipe::Request& request) const;

    bool _readLine(HANDLE pipe, std::string& buffer, std::string& line) const;
    bool _write(HANDLE pipe, std::string_view bytes) const noexcept;

    // A handle has to be in _active before its thread blocks on it, or Stop()
    // has nothing to cancel and the join never completes. _track answers false
    // if we're already stopping, which is the other half of that race.
    bool _track(HANDLE pipe) noexcept;
    void _untrack(HANDLE pipe) noexcept;

    HWND _messageWindow{ nullptr };
    UINT _requestMessage{ 0 };
    std::wstring _name;
    wil::unique_hlocal_security_descriptor _securityDescriptor;
    wil::unique_event _stop{ wil::EventOptions::ManualReset };
    std::vector<std::thread> _threads;
    std::mutex _activeLock;
    std::vector<HANDLE> _active;
};
