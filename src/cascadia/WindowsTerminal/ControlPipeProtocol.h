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

#include <charconv>
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
}
