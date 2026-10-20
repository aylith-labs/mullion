// Shared from Lintel paths/. Run conformance/sync-paths.mjs to update.
// Lintel path policy. Hosts supply distribution names and home directories, and perform I/O.
#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace Lintel
{
    enum class PathKind { None, Windows, Unc, Posix, Home };
    struct PathCandidate { std::wstring path; std::wstring distro; };
    // A home directory the host knows. An empty distro is the host's own home: the
    // Windows profile directory on Windows, the local POSIX home elsewhere.
    struct PathHome { std::wstring distro; std::wstring home; };
    inline bool DriveLetter(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
    inline PathKind ClassifyPath(std::wstring_view text)
    {
        if (text.size() >= 3 && DriveLetter(text[0]) && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/')) return PathKind::Windows;
        if (text.starts_with(L"\\\\") || text.starts_with(L"//")) return PathKind::Unc;
        if (text.starts_with(L"~/")) return PathKind::Home;
        return text.starts_with(L"/") ? PathKind::Posix : PathKind::None;
    }
    inline std::wstring PathBackslashes(std::wstring_view text)
    {
        std::wstring result{ text };
        std::replace(result.begin(), result.end(), L'/', L'\\');
        return result;
    }
    inline std::optional<std::wstring> FindPathHome(const std::vector<PathHome>& homes, std::wstring_view distro)
    {
        for (const auto& item : homes)
        {
            if (item.distro != distro || item.home.empty()) continue;
            auto home = item.home;
            while (home.size() > 1 && (home.back() == L'/' || home.back() == L'\\')) home.pop_back();
            return home;
        }
        return std::nullopt;
    }
    inline std::vector<PathCandidate> PathCandidates(std::wstring_view text, bool onWindows, std::wstring_view sourceDistro, const std::vector<std::wstring>& distros = {}, const std::vector<PathHome>& homes = {})
    {
        const auto kind = ClassifyPath(text);
        if (kind == PathKind::None) return {};
        if (kind == PathKind::Home)
        {
            // "~" means the home of whoever printed it, which the host has to know.
            // Never probe other distributions for it, and never guess a home.
            const auto rest = text.substr(1);
            if (!onWindows || sourceDistro.empty())
            {
                const auto home = FindPathHome(homes, L"");
                if (!home) return {};
                return { { *home + (onWindows ? PathBackslashes(rest) : std::wstring{ rest }), {} } };
            }
            const auto home = FindPathHome(homes, sourceDistro);
            if (!home || ClassifyPath(*home) != PathKind::Posix) return {};
            const auto expanded = *home == L"/" ? std::wstring{ rest } : *home + std::wstring{ rest };
            return PathCandidates(expanded, true, sourceDistro);
        }
        if (!onWindows || kind == PathKind::Windows || kind == PathKind::Unc) return { { std::wstring{ text }, {} } };
        if (text.starts_with(L"/mnt/") && text.size() >= 6 && DriveLetter(text[5]) && (text.size() == 6 || text[6] == L'/'))
        {
            const auto drive = static_cast<wchar_t>(text[5] >= L'a' ? text[5] - L'a' + L'A' : text[5]);
            return { { std::wstring{ drive } + L":\\" + PathBackslashes(text.substr(std::min(size_t{ 7 }, text.size()))), {} } };
        }
        std::vector<PathCandidate> result;
        const auto add = [&](std::wstring_view distro) {
            if (distro.empty() || std::any_of(result.begin(), result.end(), [&](const auto& item) { return item.distro == distro; })) return;
            result.push_back({ L"\\\\wsl.localhost\\" + std::wstring{ distro } + PathBackslashes(text), std::wstring{ distro } });
        };
        if (!sourceDistro.empty()) add(sourceDistro);
        else for (const auto& distro : distros) add(distro);
        return result;
    }
    // Whose file a file:// host, or a UNC path's server, names. The host supplies its own
    // machine names; empty and localhost are always local.
    enum class FileHostKind { Local, Wsl, Remote };
    inline bool EqualsAsciiInsensitive(std::wstring_view a, std::wstring_view b)
    {
        const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c; };
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](wchar_t x, wchar_t y) { return lower(x) == lower(y); });
    }
    inline FileHostKind ClassifyFileHost(std::wstring_view host, const std::vector<std::wstring>& localNames = {})
    {
        if (host.empty() || EqualsAsciiInsensitive(host, L"localhost") || host == L"127.0.0.1" || host == L"::1") return FileHostKind::Local;
        if (std::any_of(localNames.begin(), localNames.end(), [&](const auto& name) { return EqualsAsciiInsensitive(host, name); })) return FileHostKind::Local;
        return EqualsAsciiInsensitive(host, L"wsl$") || EqualsAsciiInsensitive(host, L"wsl.localhost") ? FileHostKind::Wsl : FileHostKind::Remote;
    }
    // A \\server\... or //server/... path on another machine, the \\.\ device namespace
    // included. A host never touches one without a click: it would be an SMB connection.
    inline bool IsRemoteUncPath(std::wstring_view path, const std::vector<std::wstring>& localNames = {})
    {
        const auto separator = [](wchar_t ch) { return ch == L'\\' || ch == L'/'; };
        if (path.size() < 2 || !separator(path[0]) || !separator(path[1])) return false;
        auto rest = path.substr(2);
        if (rest.size() > 1 && rest[0] == L'?' && separator(rest[1]))
        {
            rest = rest.substr(2);
            if (rest.size() >= 2 && DriveLetter(rest[0]) && rest[1] == L':') return false;
            if (rest.size() < 4 || !EqualsAsciiInsensitive(rest.substr(0, 3), L"UNC") || !separator(rest[3])) return true;
            rest = rest.substr(4);
        }
        const auto end = std::find_if(rest.begin(), rest.end(), separator);
        const std::wstring_view server{ rest.data(), static_cast<size_t>(end - rest.begin()) };
        return server.empty() || server == L"." || ClassifyFileHost(server, localNames) == FileHostKind::Remote;
    }
    inline std::optional<PathCandidate> SelectPathCandidate(const std::vector<PathCandidate>& candidates, const std::vector<bool>& exists)
    {
        std::optional<PathCandidate> found;
        for (size_t i = 0; i < candidates.size() && i < exists.size(); ++i)
        {
            if (!exists[i]) continue;
            if (found) return std::nullopt;
            found = candidates[i];
        }
        return found;
    }
}
