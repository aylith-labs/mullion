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
