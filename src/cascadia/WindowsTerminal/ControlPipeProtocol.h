// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The wire format of the control pipe (see doc/control-pipe.md): newline
// delimited UTF-8 JSON, one request object per line, one response object per
// line, in order.
//
// This header is deliberately free of Terminal types and of anything that
// touches a window, so that the parsing and formatting can be tested on their
// own - see UnitTests_Control/ControlPipeProtocolTests.cpp. ControlPipeServer
// does the I/O, WindowEmperor does the pane work.

#pragma once

#include <json/json.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ControlPipe
{
    // Bumped when the shape of a response changes in a way an existing client
    // could not have coped with. Reported by `ping`.
    inline constexpr uint32_t ProtocolVersion = 1;

    // A client that sends us more than this on one line is not speaking the
    // protocol; there is nothing legitimate in it that comes close.
    inline constexpr size_t MaxRequestBytes = 1024 * 1024;

    enum class Op
    {
        Ping,
        ListPanes,
        CapturePane,
        SendInput,
        FocusPane,
    };

    enum class Error
    {
        BadRequest,
        NoSuchPane,
        NeedleGone,
        Disconnected,
    };

    inline std::string_view ErrorCode(Error error) noexcept
    {
        switch (error)
        {
        case Error::NoSuchPane:
            return "no-such-pane";
        case Error::NeedleGone:
            return "needle-gone";
        case Error::Disconnected:
            return "disconnected";
        case Error::BadRequest:
        default:
            return "bad-request";
        }
    }

    // "<window>.<tabIndex>.<paneId>" - the id the client gets back from
    // list-panes and hands to capture-pane and send-input.
    struct PaneAddress
    {
        uint64_t window{};
        uint32_t tab{};
        uint32_t pane{};

        bool operator==(const PaneAddress&) const noexcept = default;
    };

    namespace details
    {
        template<typename T>
        std::optional<T> ParseUnsigned(std::string_view text) noexcept
        {
            // from_chars would happily accept a leading '+', and would stop at
            // the first bad character rather than rejecting the whole field.
            if (text.empty() || text.size() > 20)
            {
                return std::nullopt;
            }
            for (const auto ch : text)
            {
                if (ch < '0' || ch > '9')
                {
                    return std::nullopt;
                }
            }

            T value{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        inline int HexDigit(char ch) noexcept
        {
            if (ch >= '0' && ch <= '9')
            {
                return ch - '0';
            }
            if (ch >= 'a' && ch <= 'f')
            {
                return ch - 'a' + 10;
            }
            if (ch >= 'A' && ch <= 'F')
            {
                return ch - 'A' + 10;
            }
            return -1;
        }

        inline std::optional<std::wstring> Widen(std::string_view utf8)
        {
            if (utf8.empty())
            {
                return std::wstring{};
            }
            if (utf8.size() > INT_MAX)
            {
                return std::nullopt;
            }

            const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (length <= 0)
            {
                return std::nullopt;
            }

            std::wstring out;
            out.resize(static_cast<size_t>(length));
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), length) != length)
            {
                return std::nullopt;
            }
            return out;
        }

        inline std::string Narrow(std::wstring_view utf16)
        {
            if (utf16.empty() || utf16.size() > INT_MAX)
            {
                return {};
            }

            const auto length = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
            if (length <= 0)
            {
                return {};
            }

            std::string out;
            out.resize(static_cast<size_t>(length));
            WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), length, nullptr, nullptr);
            return out;
        }

        inline std::string Serialize(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            builder["commentStyle"] = "None";
            auto out = Json::writeString(builder, value);
            // We add the framing newline ourselves; jsoncpp must not sneak one in.
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            {
                out.pop_back();
            }
            return out;
        }
    }

    inline std::optional<PaneAddress> ParsePaneAddress(std::string_view text) noexcept
    {
        const auto firstDot = text.find('.');
        if (firstDot == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto secondDot = text.find('.', firstDot + 1);
        if (secondDot == std::string_view::npos || text.find('.', secondDot + 1) != std::string_view::npos)
        {
            return std::nullopt;
        }

        const auto window = details::ParseUnsigned<uint64_t>(text.substr(0, firstDot));
        const auto tab = details::ParseUnsigned<uint32_t>(text.substr(firstDot + 1, secondDot - firstDot - 1));
        const auto pane = details::ParseUnsigned<uint32_t>(text.substr(secondDot + 1));
        if (!window || !tab || !pane)
        {
            return std::nullopt;
        }

        return PaneAddress{ *window, *tab, *pane };
    }

    inline std::string FormatPaneAddress(const PaneAddress& address)
    {
        return std::to_string(address.window) + '.' + std::to_string(address.tab) + '.' + std::to_string(address.pane);
    }

    // A connection session id, the GUID a shell sees in WT_SESSION. Braced (the
    // form list-panes reports) or bare, either case. Anything else is rejected
    // whole rather than half-parsed.
    inline std::optional<GUID> ParseSessionId(std::string_view text) noexcept
    {
        if (text.size() == 38 && text.front() == '{' && text.back() == '}')
        {
            text = text.substr(1, 36);
        }
        if (text.size() != 36)
        {
            return std::nullopt;
        }

        uint8_t bytes[16]{};
        size_t count = 0;
        for (size_t i = 0; i < text.size();)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (text[i] != '-')
                {
                    return std::nullopt;
                }
                i++;
                continue;
            }

            const auto high = details::HexDigit(text[i]);
            const auto low = details::HexDigit(text[i + 1]);
            if (high < 0 || low < 0)
            {
                return std::nullopt;
            }
            bytes[count++] = static_cast<uint8_t>((high << 4) | low);
            i += 2;
        }

        GUID guid{};
        guid.Data1 = (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | uint32_t{ bytes[3] };
        guid.Data2 = static_cast<uint16_t>((bytes[4] << 8) | bytes[5]);
        guid.Data3 = static_cast<uint16_t>((bytes[6] << 8) | bytes[7]);
        for (size_t i = 0; i < 8; i++)
        {
            guid.Data4[i] = bytes[8 + i];
        }
        return guid;
    }

    struct Request
    {
        Op op{ Op::Ping };
        // Set for capture-pane and send-input, and for focus-pane when it
        // names its pane by address.
        std::optional<PaneAddress> pane;
        // focus-pane only: the pane's connection session id (WT_SESSION).
        std::optional<GUID> session;
        // list-panes filter. Empty means "no filter", which is also what an
        // absent `containing` means.
        std::wstring containing;
        // send-input payload, verbatim.
        std::wstring text;
        std::wstring requireContains;
        // capture-pane row count. 0 means "the whole viewport".
        int32_t lines{};
    };

    // Returns nullopt for anything we would answer with `bad-request`: not JSON,
    // not an object, an op we don't have, a malformed pane id, a field of the
    // wrong type, or text that isn't valid UTF-8.
    //
    // Unknown members are ignored on purpose, so that adding a field later
    // doesn't break a client that is already sending one we don't know yet.
    inline std::optional<Request> ParseRequest(std::string_view line)
    {
        if (line.size() > MaxRequestBytes)
        {
            return std::nullopt;
        }

        Json::Value root;
        {
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["failIfExtra"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string errors;
            if (!reader->parse(line.data(), line.data() + line.size(), &root, &errors))
            {
                return std::nullopt;
            }
        }

        if (!root.isObject())
        {
            return std::nullopt;
        }

        const auto& opValue = root["op"];
        if (!opValue.isString())
        {
            return std::nullopt;
        }

        Request request;
        const auto op = opValue.asString();
        if (op == "ping")
        {
            request.op = Op::Ping;
        }
        else if (op == "list-panes")
        {
            request.op = Op::ListPanes;
        }
        else if (op == "capture-pane")
        {
            request.op = Op::CapturePane;
        }
        else if (op == "send-input")
        {
            request.op = Op::SendInput;
        }
        else if (op == "focus-pane")
        {
            request.op = Op::FocusPane;
        }
        else
        {
            return std::nullopt;
        }

        const auto readText = [&](const char* key, std::wstring& out, bool& present) -> bool {
            present = false;
            if (!root.isMember(key))
            {
                return true;
            }
            const auto& value = root[key];
            if (!value.isString())
            {
                return false;
            }
            auto widened = details::Widen(value.asString());
            if (!widened)
            {
                return false;
            }
            out = std::move(*widened);
            present = true;
            return true;
        };

        auto present = false;
        if (!readText("containing", request.containing, present) ||
            !readText("requireContains", request.requireContains, present))
        {
            return std::nullopt;
        }

        auto hasText = false;
        if (!readText("text", request.text, hasText))
        {
            return std::nullopt;
        }

        if (root.isMember("pane"))
        {
            const auto& value = root["pane"];
            if (!value.isString())
            {
                return std::nullopt;
            }
            request.pane = ParsePaneAddress(value.asString());
            if (!request.pane)
            {
                return std::nullopt;
            }
        }

        if (root.isMember("lines"))
        {
            const auto& value = root["lines"];
            // isIntegral() is true for a bool as well, and `"lines": true` is
            // a mistake we would rather report than guess at.
            if (!value.isInt() || value.isBool())
            {
                return std::nullopt;
            }
            request.lines = std::max(0, value.asInt());
        }

        // `session` is read for focus-pane only; every other op ignores it
        // like any unknown member.
        if (request.op == Op::FocusPane && root.isMember("session"))
        {
            const auto& value = root["session"];
            if (!value.isString())
            {
                return std::nullopt;
            }
            request.session = ParseSessionId(value.asString());
            if (!request.session)
            {
                return std::nullopt;
            }
        }

        // A pane op without a pane, or a send-input without text, is a client
        // bug rather than a missing pane - say so.
        if ((request.op == Op::CapturePane || request.op == Op::SendInput) && !request.pane)
        {
            return std::nullopt;
        }
        // focus-pane names its pane exactly one way: both at once could disagree.
        if (request.op == Op::FocusPane && request.pane.has_value() == request.session.has_value())
        {
            return std::nullopt;
        }
        if (request.op == Op::SendInput && !hasText)
        {
            return std::nullopt;
        }

        return request;
    }

    struct PaneEntry
    {
        PaneAddress address;
        std::wstring title;
        std::wstring process;
        // Focused within its tab, and its tab is the active tab.
        bool focused{};
        // That pane's window is the OS foreground window.
        bool windowFocused{};
        // The pane's process is still running. A dead pane can still be
        // captured; writing to it answers `disconnected`.
        bool alive{};
        uint32_t pid{};
        std::wstring session;
    };

    inline std::string ErrorResponse(Error error)
    {
        Json::Value root{ Json::objectValue };
        root["ok"] = false;
        root["error"] = std::string{ ErrorCode(error) };
        return details::Serialize(root);
    }

    inline std::string PingResponse(uint32_t pid, const std::vector<uint64_t>& windows)
    {
        Json::Value root{ Json::objectValue };
        root["ok"] = true;
        root["version"] = ProtocolVersion;
        root["pid"] = pid;

        Json::Value ids{ Json::arrayValue };
        for (const auto id : windows)
        {
            ids.append(Json::Value::UInt64{ id });
        }
        root["windows"] = std::move(ids);

        return details::Serialize(root);
    }

    inline std::string ListPanesResponse(const std::vector<PaneEntry>& panes)
    {
        Json::Value root{ Json::objectValue };
        root["ok"] = true;

        Json::Value list{ Json::arrayValue };
        for (const auto& pane : panes)
        {
            Json::Value entry{ Json::objectValue };
            entry["id"] = FormatPaneAddress(pane.address);
            entry["window"] = Json::Value::UInt64{ pane.address.window };
            entry["tab"] = pane.address.tab;
            entry["pane"] = pane.address.pane;
            entry["title"] = details::Narrow(pane.title);
            entry["focused"] = pane.focused;
            entry["windowFocused"] = pane.windowFocused;
            entry["alive"] = pane.alive;
            entry["pid"] = pane.pid;
            entry["process"] = details::Narrow(pane.process);
            // Not part of the original contract; additive, and it lets a client
            // correlate a pane with the WT_SESSION its shell already knows.
            entry["session"] = details::Narrow(pane.session);
            list.append(std::move(entry));
        }
        root["panes"] = std::move(list);

        return details::Serialize(root);
    }

    inline std::string CapturePaneResponse(std::wstring_view text)
    {
        Json::Value root{ Json::objectValue };
        root["ok"] = true;
        root["text"] = details::Narrow(text);
        return details::Serialize(root);
    }

    inline std::string OkResponse()
    {
        Json::Value root{ Json::objectValue };
        root["ok"] = true;
        return details::Serialize(root);
    }

    // The pane focus-pane landed on, so a client that asked by session id
    // learns its address without a second list-panes.
    inline std::string FocusPaneResponse(const PaneAddress& address)
    {
        Json::Value root{ Json::objectValue };
        root["ok"] = true;
        root["id"] = FormatPaneAddress(address);
        return details::Serialize(root);
    }

    // ---------------------------------------------------------------------
    // Recording. Every request is written to the activity log and listed on
    // the tab row's pipe badge; these put a request into the words both of
    // them show. They live here, away from the pipe, so they can be tested.
    // ---------------------------------------------------------------------

    // The longest send-input text a record keeps. The log is this user's own,
    // but one pasted megabyte would push everything else out of a 4 MiB log.
    inline constexpr size_t MaxRecordedTextChars = 4096;
    // The most of a filter or a needle a record keeps.
    inline constexpr size_t MaxRecordedNeedleChars = 256;
    // How much of a line that would not parse is kept: enough to tell which
    // client sent it and what it got wrong.
    inline constexpr size_t MaxRecordedRawBytes = 512;

    inline std::wstring_view OpName(Op op) noexcept
    {
        switch (op)
        {
        case Op::ListPanes:
            return L"list-panes";
        case Op::CapturePane:
            return L"capture-pane";
        case Op::SendInput:
            return L"send-input";
        case Op::FocusPane:
            return L"focus-pane";
        case Op::Ping:
        default:
            return L"ping";
        }
    }

    // What a request was. `op` is the wire name, or "unparsed" for a line that
    // never became a request; `target` is the pane address or the braced
    // session id it named; `detail` is what it asked for.
    struct CallDescription
    {
        std::wstring op;
        std::wstring target;
        std::wstring detail;
    };

    namespace details
    {
        // Quoted, with control characters spelled out. A record that shows \r
        // says exactly what reached the pane, where a raw CR would end the line
        // it is printed on - and "did the client send the Enter" is the usual
        // question.
        inline std::wstring Quote(std::wstring_view text, size_t maxChars)
        {
            static constexpr wchar_t hex[] = L"0123456789abcdef";

            auto kept = std::min(text.size(), maxChars);
            // Never cut between the halves of a surrogate pair.
            if (kept < text.size() && kept > 0 && IS_HIGH_SURROGATE(text[kept - 1]))
            {
                kept--;
            }

            std::wstring out;
            out.reserve(kept + 2);
            out.push_back(L'"');
            for (size_t i = 0; i < kept; i++)
            {
                const auto ch = text[i];
                switch (ch)
                {
                case L'"':
                    out.append(L"\\\"");
                    break;
                case L'\\':
                    out.append(L"\\\\");
                    break;
                case L'\r':
                    out.append(L"\\r");
                    break;
                case L'\n':
                    out.append(L"\\n");
                    break;
                case L'\t':
                    out.append(L"\\t");
                    break;
                default:
                    if (ch < 0x20 || ch == 0x7f)
                    {
                        out.append(L"\\x");
                        out.push_back(hex[(ch >> 4) & 0xf]);
                        out.push_back(hex[ch & 0xf]);
                    }
                    else
                    {
                        out.push_back(ch);
                    }
                    break;
                }
            }
            out.push_back(L'"');

            if (kept < text.size())
            {
                out.append(L" ...+");
                out.append(std::to_wstring(text.size() - kept));
                out.append(L" chars");
            }
            return out;
        }

        inline std::wstring Ascii(std::string_view text)
        {
            return { text.begin(), text.end() };
        }

        inline std::wstring FormatSessionId(const GUID& guid)
        {
            wchar_t buffer[64]{};
            const auto data4 = [&](size_t i) { return static_cast<unsigned int>(guid.Data4[i]); };
            swprintf_s(buffer,
                       L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                       static_cast<unsigned long>(guid.Data1),
                       static_cast<unsigned int>(guid.Data2),
                       static_cast<unsigned int>(guid.Data3),
                       data4(0),
                       data4(1),
                       data4(2),
                       data4(3),
                       data4(4),
                       data4(5),
                       data4(6),
                       data4(7));
            return buffer;
        }
    }

    inline CallDescription DescribeRequest(const Request& request)
    {
        CallDescription description;
        description.op = OpName(request.op);

        if (request.pane)
        {
            description.target = details::Ascii(FormatPaneAddress(*request.pane));
        }
        else if (request.session)
        {
            description.target = details::FormatSessionId(*request.session);
        }

        switch (request.op)
        {
        case Op::ListPanes:
            if (!request.containing.empty())
            {
                description.detail = L"containing " + details::Quote(request.containing, MaxRecordedNeedleChars);
            }
            break;
        case Op::CapturePane:
            description.detail = request.lines > 0 ? L"lines " + std::to_wstring(request.lines) : std::wstring{ L"viewport" };
            break;
        case Op::SendInput:
            description.detail = details::Quote(request.text, MaxRecordedTextChars);
            if (!request.requireContains.empty())
            {
                description.detail += L" requireContains " + details::Quote(request.requireContains, MaxRecordedNeedleChars);
            }
            break;
        case Op::Ping:
        case Op::FocusPane:
        default:
            break;
        }
        return description;
    }

    // A line that never became a request has no op to name, so keep the start
    // of what was sent instead.
    inline CallDescription DescribeUnparsed(std::string_view line)
    {
        CallDescription description;
        description.op = L"unparsed";

        auto kept = std::min(line.size(), MaxRecordedRawBytes);
        // Back off to a character boundary, so a cut through a multi-byte
        // character does not turn the whole prefix into a conversion failure.
        while (kept > 0 && kept < line.size() && (static_cast<unsigned char>(line[kept]) & 0xC0) == 0x80)
        {
            kept--;
        }
        const auto prefix = line.substr(0, kept);

        auto widened = details::Widen(prefix);
        if (!widened)
        {
            // Not UTF-8 at all. Keep the ASCII and mark the rest, rather than
            // keeping nothing.
            widened.emplace();
            for (const auto ch : prefix)
            {
                widened->push_back(static_cast<unsigned char>(ch) < 0x80 ? static_cast<wchar_t>(ch) : L'?');
            }
        }

        description.detail = details::Quote(*widened, MaxRecordedRawBytes);
        if (kept < line.size())
        {
            description.detail.append(L" ...+");
            description.detail.append(std::to_wstring(line.size() - kept));
            description.detail.append(L" bytes");
        }
        return description;
    }

    // The one-line form, as the activity log's command column shows it:
    //   send-input 1.0.3 "echo hi\r"
    inline std::wstring SummarizeCall(const CallDescription& description)
    {
        std::wstring out{ description.op };
        for (const auto* part : { &description.target, &description.detail })
        {
            if (!part->empty())
            {
                out.push_back(L' ');
                out.append(*part);
            }
        }
        return out;
    }
}
