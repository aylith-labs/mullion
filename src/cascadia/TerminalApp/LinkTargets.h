// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Where a link a preview was asked for really leads.
//
// Windows follows its own symbolic links and junctions, so std::filesystem::status on
// one already answers for its target and nothing here is needed. It cannot follow a
// Linux symlink seen through \\wsl.localhost: the Plan 9 server reports it as a
// reparse point tagged IO_REPARSE_TAG_LX_SYMLINK, opening it fails with "path not
// found", and FSCTL_GET_REPARSE_POINT is refused, so from Windows there is no way to
// read where it points. The same goes for any path that passes through one, such as
// /lib/os-release on a distribution where /lib is a link to usr/lib. Only the
// distribution can resolve those, so it is asked, once, and only after Windows has
// failed and a reparse point has been seen on the way.
#pragma once
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include "../../inc/LintelPaths.h"

namespace Microsoft::Terminal::FilePreview
{
    struct WslUncPath
    {
        std::wstring distro;
        std::wstring posixPath;
    };

    // \\wsl.localhost\Ubuntu\etc\os-release -> { Ubuntu, /etc/os-release }. \\wsl$ is the
    // older spelling of the same thing. Forward slashes are accepted as well.
    inline std::optional<WslUncPath> SplitWslUncPath(std::wstring_view path)
    {
        const auto separator = [](wchar_t ch) { return ch == L'\\' || ch == L'/'; };
        if (path.size() < 3 || !separator(path[0]) || !separator(path[1]))
        {
            return std::nullopt;
        }
        auto rest = path.substr(2);
        const auto hostEnd = std::find_if(rest.begin(), rest.end(), separator);
        const std::wstring_view host{ rest.data(), static_cast<size_t>(hostEnd - rest.begin()) };
        if (Lintel::ClassifyFileHost(host) != Lintel::FileHostKind::Wsl || host.size() == rest.size())
        {
            return std::nullopt;
        }
        rest = rest.substr(host.size() + 1);
        const auto distroEnd = std::find_if(rest.begin(), rest.end(), separator);
        WslUncPath result;
        result.distro.assign(rest.data(), static_cast<size_t>(distroEnd - rest.begin()));
        if (result.distro.empty())
        {
            return std::nullopt;
        }
        rest = rest.substr(result.distro.size());
        result.posixPath = rest.empty() ? std::wstring{ L"/" } : std::wstring{ rest };
        std::replace(result.posixPath.begin(), result.posixPath.end(), L'\\', L'/');
        return result;
    }

    // Run inside the distribution as `sh -c <script> sh <path>`. It prints one of
    //   found\n<canonical path>   the path, links and all, leads to something that exists
    //   broken\n<link text>       the path is itself a link, to something that does not
    //   absent\n                  anything else: the path is simply not there
    // readlink -f rather than -e or realpath, because BusyBox, which Alpine ships, has only -f.
    inline constexpr std::wstring_view WslLinkScript =
        LR"sh(r=$(readlink -f -- "$1" 2>/dev/null) && [ -e "$r" ] && { printf 'found\n%s\n' "$r"; exit 0; }; [ -L "$1" ] && { printf 'broken\n%s\n' "$(readlink -- "$1")"; exit 0; }; printf 'absent\n')sh";

    enum class WslLinkStatus
    {
        Found,
        Broken,
        Absent,
    };

    struct WslLinkReply
    {
        WslLinkStatus status{ WslLinkStatus::Absent };
        // The canonical POSIX path when found; the link's own text when broken.
        std::wstring path;
    };

    inline std::wstring Utf8ToWide(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
        if (length > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        }
        return wide;
    }

    // Anything that is not exactly the reply WslLinkScript prints -- wsl.exe failing to
    // start the distribution, an error page, nothing at all -- is no answer.
    inline std::optional<WslLinkReply> ParseWslLinkReply(std::string_view output)
    {
        const auto firstEnd = output.find('\n');
        if (firstEnd == std::string_view::npos)
        {
            return std::nullopt;
        }
        auto status = output.substr(0, firstEnd);
        if (!status.empty() && status.back() == '\r')
        {
            status.remove_suffix(1);
        }
        auto rest = output.substr(firstEnd + 1);
        if (const auto end = rest.find('\n'); end != std::string_view::npos)
        {
            rest = rest.substr(0, end);
        }
        if (!rest.empty() && rest.back() == '\r')
        {
            rest.remove_suffix(1);
        }
        WslLinkReply reply;
        if (status == "found")
        {
            if (rest.empty() || rest.front() != '/')
            {
                return std::nullopt;
            }
            reply.status = WslLinkStatus::Found;
        }
        else if (status == "broken")
        {
            reply.status = WslLinkStatus::Broken;
        }
        else if (status == "absent")
        {
            return reply;
        }
        else
        {
            return std::nullopt;
        }
        reply.path = Utf8ToWide(rest);
        return reply;
    }

    // What the card says about a link that leads nowhere. The link's own text is the
    // useful part -- it is what to fix -- so it is quoted when there is one.
    inline std::wstring BrokenLinkMessage(std::wstring_view linkText)
    {
        if (linkText.empty())
        {
            return L"A link on this path points to something that does not exist.";
        }
        return L"This link points to " + std::wstring{ linkText } + L", which does not exist.";
    }

    struct LinkedPath
    {
        // Where the link leads, when it leads somewhere.
        std::filesystem::path target;
        // What to say instead, when it does not.
        std::wstring broken;
    };

    // Only for a path Windows has already said is neither a file nor a folder: a working
    // Windows link never gets here. runWsl(distro, posixPath) runs WslLinkScript in that
    // distribution and returns what it printed. Nothing is returned when no link is
    // involved, or when the distribution gives no answer; the caller's own "not a file or
    // a folder" then stands.
    template<typename RunWsl>
    std::optional<LinkedPath> ResolveLinkedPath(const std::filesystem::path& path, const RunWsl& runWsl)
    {
        // The deepest part of the path Windows can see at all. GetFileAttributesW does not
        // follow a link, so a link -- the path itself, or a folder on the way to it -- shows
        // up here as a reparse point. Anything else is simply missing, and asking the
        // distribution about a path that is not there would only cost a process.
        auto existing = path;
        auto attributes = GetFileAttributesW(existing.c_str());
        while (attributes == INVALID_FILE_ATTRIBUTES && existing.has_relative_path())
        {
            existing = existing.parent_path();
            attributes = GetFileAttributesW(existing.c_str());
        }
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        {
            return std::nullopt;
        }

        if (const auto wsl = SplitWslUncPath(path.native()))
        {
            const auto reply = ParseWslLinkReply(runWsl(wsl->distro, wsl->posixPath));
            if (!reply || reply->status == WslLinkStatus::Absent)
            {
                return std::nullopt;
            }
            if (reply->status == WslLinkStatus::Broken)
            {
                return LinkedPath{ {}, BrokenLinkMessage(reply->path) };
            }
            // A link into /mnt/c comes back as C:\..., not as the distribution's view of it.
            const auto candidates = Lintel::PathCandidates(reply->path, true, wsl->distro);
            if (candidates.empty())
            {
                return std::nullopt;
            }
            return LinkedPath{ std::filesystem::path{ candidates.front().path }, {} };
        }

        // A Windows symbolic link or junction Windows could not follow. Only call it
        // broken when what it names is really not there: one Windows could not follow for
        // some other reason (access, an app execution alias) is not this message's business.
        std::error_code error;
        const auto kind = std::filesystem::symlink_status(existing, error).type();
        if (error || (kind != std::filesystem::file_type::symlink && kind != std::filesystem::file_type::junction))
        {
            return std::nullopt;
        }
        const auto text = std::filesystem::read_symlink(existing, error);
        if (error)
        {
            return LinkedPath{ {}, BrokenLinkMessage({}) };
        }
        const auto named = text.is_absolute() ? text : existing.parent_path() / text;
        if (std::error_code existsError; std::filesystem::exists(named, existsError))
        {
            return std::nullopt;
        }
        // A junction's own text is in the NT namespace, \??\C:\...; nobody wrote that prefix.
        auto shown = text.wstring();
        if (shown.starts_with(LR"(\??\)"))
        {
            shown.erase(0, 4);
        }
        return LinkedPath{ {}, BrokenLinkMessage(existing == path ? std::wstring_view{ shown } : std::wstring_view{}) };
    }
}
