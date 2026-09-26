// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlPipeServer.h"

#include <sddl.h>

// A pane's screen is at most a few hundred KB of text; a client that wants the
// scrollback is asking the wrong tool.
static constexpr DWORD PipeBufferSize = 64 * 1024;

ControlPipeServer::ControlPipeServer(HWND messageWindow, UINT requestMessage) noexcept :
    _messageWindow{ messageWindow },
    _requestMessage{ requestMessage }
{
}

ControlPipeServer::~ControlPipeServer()
{
    Stop();
}

std::wstring ControlPipeServer::PipeNameForProcess(DWORD pid)
{
    // One pipe per Terminal process. A client enumerates \\.\pipe\wt-control-*
    // and talks to each one it finds, so several Terminals - stable, preview,
    // elevated - coexist without needing to know about each other.
    return L"\\\\.\\pipe\\wt-control-" + std::to_wstring(pid);
}

// Only this user, and only locally. Not the PIPE_ACCESS_* defaults, which would
// hand write access to anyone who can look up the name: the pipe can type into
// the user's shells, so the DACL is the whole security story.
static wil::unique_hlocal_security_descriptor _buildSecurityDescriptor()
{
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));

    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    THROW_LAST_ERROR_IF(size == 0);

    std::vector<std::byte> buffer(size);
    THROW_IF_WIN32_BOOL_FALSE(GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size));

    wil::unique_hlocal_string sid;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid));

    // D: - discretionary ACL
    // P  - protected, so nothing is inherited in behind our back
    // A;;GA;;;<sid> - allow all access, to this user and nobody else
    const auto sddl = std::wstring{ L"D:P(A;;GA;;;" } + sid.get() + L")";

    wil::unique_hlocal_security_descriptor descriptor;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
    return descriptor;
}

bool ControlPipeServer::Start()
try
{
    if (!_threads.empty())
    {
        return true;
    }

    _name = PipeNameForProcess(GetCurrentProcessId());
    _securityDescriptor = _buildSecurityDescriptor();
    _stop.ResetEvent();

    // The first instance is created here, with FILE_FLAG_FIRST_PIPE_INSTANCE,
    // so that a name someone else already owns fails loudly instead of quietly
    // making us the second instance of somebody else's pipe.
    auto first = _createInstance(true);
    if (!first)
    {
        return false;
    }

    // Worker 0 inherits the instance we just made; the rest create their own on
    // their first time round, so all MaxInstances of them are listening at once
    // and a second client never has to wait for the first to finish.
    _threads.reserve(MaxInstances);
    for (DWORD i = 0; i < MaxInstances; i++)
    {
        _threads.emplace_back([this, instance = std::move(first)]() mutable {
            _worker(std::move(instance));
        });
    }

    TraceLoggingWrite(
        g_hWindowsTerminalProvider,
        "ControlPipeStarted",
        TraceLoggingDescription("The control pipe is listening"),
        TraceLoggingValue(static_cast<uint32_t>(GetCurrentProcessId()), "Pid"),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
        TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));

    return true;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    _threads.clear();
    return false;
}

void ControlPipeServer::Stop() noexcept
{
    if (_threads.empty())
    {
        return;
    }

    _stop.SetEvent();

    // Cancel whatever every worker is blocked on - a ConnectNamedPipe waiting
    // for a client that may never come, or a ReadFile waiting on a client that
    // has gone quiet. Without this, joining below would wait for a stranger.
    {
        std::scoped_lock lock{ _activeLock };
        for (const auto handle : _active)
        {
            CancelIoEx(handle, nullptr);
        }
    }

    // A worker may be sitting inside SendMessage to the very thread that is
    // calling Stop(). join() doesn't pump, so joining first would leave that
    // SendMessage unanswered and deadlock the app. Wait with a pump instead:
    // PeekMessage dispatches pending *sent* messages, which is exactly the set
    // we're blocked behind, and PM_NOREMOVE leaves posted messages alone.
    std::vector<HANDLE> pending;
    pending.reserve(_threads.size());
    for (auto& thread : _threads)
    {
        if (thread.joinable())
        {
            pending.push_back(thread.native_handle());
        }
    }

    while (!pending.empty())
    {
        const auto count = static_cast<DWORD>(pending.size());
        const auto result = MsgWaitForMultipleObjectsEx(count, pending.data(), INFINITE, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);

        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
        {
            pending.erase(pending.begin() + (result - WAIT_OBJECT_0));
        }
        else if (result == WAIT_OBJECT_0 + count)
        {
            MSG msg;
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        }
        else
        {
            LOG_LAST_ERROR();
            break;
        }
    }

    for (auto& thread : _threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    _threads.clear();
    _securityDescriptor.reset();
}

wil::unique_hfile ControlPipeServer::_createInstance(bool first) const noexcept
{
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = _securityDescriptor.get();
    attributes.bInheritHandle = FALSE;

    DWORD openMode = PIPE_ACCESS_DUPLEX;
    WI_SetFlagIf(openMode, FILE_FLAG_FIRST_PIPE_INSTANCE, first);

    wil::unique_hfile pipe{ CreateNamedPipeW(
        _name.c_str(),
        openMode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        MaxInstances,
        PipeBufferSize,
        PipeBufferSize,
        0,
        &attributes) };

    if (!pipe)
    {
        LOG_LAST_ERROR();
    }
    return pipe;
}

bool ControlPipeServer::_track(HANDLE pipe) noexcept
{
    std::scoped_lock lock{ _activeLock };
    if (_stop.is_signaled())
    {
        return false;
    }
    _active.push_back(pipe);
    return true;
}

void ControlPipeServer::_untrack(HANDLE pipe) noexcept
{
    std::scoped_lock lock{ _activeLock };
    _active.erase(std::remove(_active.begin(), _active.end(), pipe), _active.end());
}

void ControlPipeServer::_worker(wil::unique_hfile firstInstance) noexcept
{
    auto instance = std::move(firstInstance);

    while (!_stop.is_signaled())
    {
        if (!instance)
        {
            instance = _createInstance(false);
            if (!instance)
            {
                // Out of instances or the name went away. Either way there is
                // nothing useful to retry against.
                return;
            }
        }

        if (!_track(instance.get()))
        {
            return;
        }

        // A client that connected between CreateNamedPipe and here shows up as
        // ERROR_PIPE_CONNECTED, which is success wearing a failure's clothes.
        const auto connected = ConnectNamedPipe(instance.get(), nullptr) ?
                                   true :
                                   GetLastError() == ERROR_PIPE_CONNECTED;

        if (connected && !_stop.is_signaled())
        {
            _serve(instance.get());
        }

        // Untrack before the handle closes: Stop() may be cancelling I/O on
        // whatever is in that list right now.
        _untrack(instance.get());

        FlushFileBuffers(instance.get());
        DisconnectNamedPipe(instance.get());
        instance.reset();
    }
}

// Read one request, answer it, repeat until the client goes away. A client may
// keep the connection for many requests or open one per request; both are fine.
void ControlPipeServer::_serve(HANDLE pipe) noexcept
try
{
    TraceLoggingWrite(
        g_hWindowsTerminalProvider,
        "ControlPipeClientConnected",
        TraceLoggingDescription("A control pipe client connected"),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
        TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));

    std::string buffer;
    std::string line;

    while (!_stop.is_signaled())
    {
        if (!_readLine(pipe, buffer, line))
        {
            return;
        }

        // The response never contains what the client asked us to type, and we
        // never trace it either: that text is session titles and command lines.
        std::string response;
        if (line.size() > ControlPipe::MaxRequestBytes)
        {
            response = ControlPipe::ErrorResponse(ControlPipe::Error::BadRequest);
        }
        else if (const auto request = ControlPipe::ParseRequest(line))
        {
            response = _dispatch(*request);
        }
        else
        {
            response = ControlPipe::ErrorResponse(ControlPipe::Error::BadRequest);
        }

        response.push_back('\n');
        if (!_write(pipe, response))
        {
            return;
        }

        // An over-long line means we've lost sync with whatever the client
        // thinks it is sending; answering and hanging up beats guessing.
        if (line.size() > ControlPipe::MaxRequestBytes)
        {
            return;
        }
    }
}
CATCH_LOG()

// Hand the request to the UI thread and turn what comes back into a response
// line. Everything expensive - JSON, UTF-8, process names - stays out here.
std::string ControlPipeServer::_dispatch(const ControlPipe::Request& request) const
{
    ControlPipeExchange exchange;
    exchange.request = &request;

    // Deliberately SendMessage and not SendMessageTimeout. A timeout here would
    // be a lie in the one case that matters: the message stays queued after the
    // timeout expires, so we'd tell the client nothing was written while the UI
    // thread went on to write it - and it would be writing through a pointer to
    // a stack frame we'd already left. Waiting is the honest answer; Stop()
    // pumps, so this can't deadlock against our own shutdown, and the only way
    // to wait forever is a UI thread that is already gone.
    SetLastError(ERROR_SUCCESS);
    SendMessageW(_messageWindow, _requestMessage, 0, reinterpret_cast<LPARAM>(&exchange));
    if (const auto error = GetLastError(); error != ERROR_SUCCESS)
    {
        // An invalid window: we're shutting down. Nothing was done.
        LOG_WIN32(error);
        return ControlPipe::ErrorResponse(ControlPipe::Error::NoSuchPane);
    }

    if (exchange.error)
    {
        return ControlPipe::ErrorResponse(*exchange.error);
    }

    switch (request.op)
    {
    case ControlPipe::Op::Ping:
        return ControlPipe::PingResponse(GetCurrentProcessId(), exchange.windows);

    case ControlPipe::Op::ListPanes:
    {
        // Resolving an image name is a couple of syscalls per pane, so it
        // happens here rather than on the UI thread the user is typing into.
        for (auto& pane : exchange.panes)
        {
            if (pane.pid == 0)
            {
                continue;
            }

            wil::unique_handle process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pane.pid) };
            if (!process)
            {
                continue;
            }

            wchar_t path[MAX_PATH]{};
            DWORD length = ARRAYSIZE(path);
            if (QueryFullProcessImageNameW(process.get(), 0, path, &length))
            {
                const std::wstring_view full{ path, length };
                const auto slash = full.find_last_of(L'\\');
                pane.process = slash == std::wstring_view::npos ? std::wstring{ full } : std::wstring{ full.substr(slash + 1) };
            }
        }
        return ControlPipe::ListPanesResponse(exchange.panes);
    }

    case ControlPipe::Op::CapturePane:
        return ControlPipe::CapturePaneResponse(exchange.text);

    case ControlPipe::Op::FocusPane:
        return ControlPipe::FocusPaneResponse(exchange.focused);

    case ControlPipe::Op::SendInput:
    default:
        return ControlPipe::OkResponse();
    }
}

// Pull bytes until we have a whole line. Once a line is over the limit we stop
// accumulating and just drain to the newline, so a client can't make us hold a
// gigabyte of its mistake in memory.
bool ControlPipeServer::_readLine(HANDLE pipe, std::string& buffer, std::string& line) const
{
    for (;;)
    {
        if (const auto newline = buffer.find('\n'); newline != std::string::npos)
        {
            line.assign(buffer, 0, newline);
            buffer.erase(0, newline + 1);
            // Be forgiving about CRLF; a client on the Windows side of things
            // is very likely to send one.
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            return true;
        }

        if (buffer.size() > ControlPipe::MaxRequestBytes)
        {
            // Keep the oversize marker but stop growing.
            buffer.resize(ControlPipe::MaxRequestBytes + 1);
        }

        char chunk[4096];
        DWORD read = 0;
        if (!ReadFile(pipe, chunk, ARRAYSIZE(chunk), &read, nullptr) || read == 0)
        {
            const auto error = GetLastError();
            if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED && error != ERROR_NO_DATA && error != ERROR_OPERATION_ABORTED)
            {
                LOG_WIN32(error);
            }
            return false;
        }

        if (buffer.size() <= ControlPipe::MaxRequestBytes)
        {
            buffer.append(chunk, read);
        }
        else
        {
            // Already oversize: only keep looking for the newline.
            const std::string_view view{ chunk, read };
            if (const auto newline = view.find('\n'); newline != std::string_view::npos)
            {
                buffer.append(chunk, newline + 1);
            }
        }
    }
}

bool ControlPipeServer::_write(HANDLE pipe, std::string_view bytes) const noexcept
{
    while (!bytes.empty())
    {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written == 0)
        {
            // The client hung up mid-response. Perfectly normal; not our
            // problem, and definitely not the Terminal's.
            return false;
        }
        bytes.remove_prefix(written);
    }
    return true;
}
