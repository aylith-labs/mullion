// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The reply of `shefrd machine read <machine> <path> --max-bytes <n>`: one JSON line, then,
// on success, exactly "bytes" raw bytes. The contract lives in Lintel's SPEC.md, "Host
// file-preview capability". Anything that is not such a reply is Unavailable, which the
// card shows quietly: the reader may simply not exist yet, or that machine may be asleep.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::FilePreview
{
    enum class RemoteReadStatus
    {
        Ok,
        NotFound,
        NotRegular,
        TooLarge,
        Denied,
        NotConnected,
        Unsupported,
        Unavailable,
    };

    struct RemoteReadReply
    {
        RemoteReadStatus status{ RemoteReadStatus::Unavailable };
        uint64_t size{ 0 };
        bool truncated{ false };
        std::string bytes;
    };

    // Value of "key" in a flat JSON object: a string's contents (no escapes are
    // expected in the fields read here), or a bare literal such as 123 or true.
    inline std::optional<std::string_view> RemoteReadField(std::string_view json, std::string_view key)
    {
        const auto quoted = "\"" + std::string{ key } + "\"";
        auto at = json.find(quoted);
        if (at == std::string_view::npos)
        {
            return std::nullopt;
        }
        at = json.find_first_not_of(" \t", at + quoted.size());
        if (at == std::string_view::npos || json[at] != ':')
        {
            return std::nullopt;
        }
        at = json.find_first_not_of(" \t", at + 1);
        if (at == std::string_view::npos)
        {
            return std::nullopt;
        }
        if (json[at] == '"')
        {
            const auto end = json.find('"', at + 1);
            return end == std::string_view::npos ? std::nullopt : std::optional{ json.substr(at + 1, end - at - 1) };
        }
        const auto end = json.find_first_of(",} \t", at);
        return json.substr(at, (end == std::string_view::npos ? json.size() : end) - at);
    }

    inline std::optional<uint64_t> RemoteReadNumber(std::string_view json, std::string_view key)
    {
        const auto value = RemoteReadField(json, key);
        if (!value || value->empty() || value->size() > 19 || value->find_first_not_of("0123456789") != std::string_view::npos)
        {
            return std::nullopt;
        }
        return std::stoull(std::string{ *value });
    }

    inline RemoteReadReply ParseRemoteReadReply(std::string_view output, size_t byteLimit)
    {
        RemoteReadReply reply;
        // A login shell may print before the reply, so the header is the first line that starts like one.
        size_t start = 0;
        while (!output.substr(start).starts_with("{\"ok\""))
        {
            start = output.find('\n', start);
            if (start == std::string_view::npos)
            {
                return reply;
            }
            ++start;
        }
        const auto lineEnd = output.find('\n', start);
        const auto header = output.substr(start, (lineEnd == std::string_view::npos ? output.size() : lineEnd) - start);
        const auto ok = RemoteReadField(header, "ok");
        if (ok == "false")
        {
            const auto error = RemoteReadField(header, "error").value_or("");
            reply.status = error == "not_found"     ? RemoteReadStatus::NotFound :
                           error == "not_regular"   ? RemoteReadStatus::NotRegular :
                           error == "too_large"     ? RemoteReadStatus::TooLarge :
                           error == "denied"        ? RemoteReadStatus::Denied :
                           error == "not_connected" ? RemoteReadStatus::NotConnected :
                           error == "unsupported"   ? RemoteReadStatus::Unsupported :
                                                      RemoteReadStatus::Unavailable;
            return reply;
        }
        const auto size = RemoteReadNumber(header, "size");
        const auto count = RemoteReadNumber(header, "bytes");
        const auto truncated = RemoteReadField(header, "truncated");
        if (ok != "true" || RemoteReadField(header, "kind") != "file" || !size || !count ||
            (truncated != "true" && truncated != "false") || *count > byteLimit || *count > *size || lineEnd == std::string_view::npos)
        {
            return reply;
        }
        const auto payload = output.substr(lineEnd + 1);
        if (payload.size() < *count)
        {
            return reply;
        }
        reply.status = RemoteReadStatus::Ok;
        reply.size = *size;
        reply.truncated = truncated == "true" || *count < *size;
        reply.bytes.assign(payload.substr(0, static_cast<size_t>(*count)));
        return reply;
    }
}
