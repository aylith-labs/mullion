// GitHub credentials follow the same CLI-first order for settings and previews.
#pragma once
#include "ProcessCaptureImpl.h"
#include <string>
#include <algorithm>
#include <set>
#include <utility>
#include <vector>
#include <cwctype>
namespace Microsoft::Terminal
{
    inline bool ValidGitHubOwner(std::wstring_view value)
    {
        if (value.empty() || value.size() > 39 || value.front() == L'-' || value.back() == L'-') return false;
        return std::all_of(value.begin(), value.end(), [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-'; });
    }
    inline std::vector<std::wstring> GitHubOwners(std::wstring_view text)
    {
        std::vector<std::wstring> result;
        std::set<std::wstring> seen;
        std::wstring owner;
        const auto add = [&] {
            auto lower = owner; std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
            if (ValidGitHubOwner(owner) && seen.insert(lower).second) result.push_back(owner);
            owner.clear();
        };
        for (const auto ch : text) { if (ch == L',' || iswspace(ch)) add(); else owner += ch; } add();
        return result;
    }
    inline bool ValidGitHubRepo(std::wstring_view value)
    {
        if (value.empty() || value.size() > 100 || value == L"." || value == L"..") return false;
        return std::all_of(value.begin(), value.end(), [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.'; });
    }
    inline bool SameGitHubName(std::wstring_view a, std::wstring_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return towlower(x) == towlower(y); });
    }
    // `repoOwners` (Lintel SPEC, "Resolving repo#number"): the owner the user
    // picked for a repository name, stored as "repo=owner,repo=owner". Invalid
    // pairs are dropped, and a later entry for the same repository loses.
    inline std::vector<std::pair<std::wstring, std::wstring>> GitHubRepoOwners(std::wstring_view text)
    {
        std::vector<std::pair<std::wstring, std::wstring>> result;
        std::wstring entry;
        const auto add = [&] {
            const auto equals = entry.find(L'=');
            if (equals != std::wstring::npos)
            {
                auto repo = entry.substr(0, equals), owner = entry.substr(equals + 1);
                const auto known = std::any_of(result.begin(), result.end(), [&](const auto& pair) { return SameGitHubName(pair.first, repo); });
                if (!known && ValidGitHubRepo(repo) && ValidGitHubOwner(owner)) result.emplace_back(std::move(repo), std::move(owner));
            }
            entry.clear();
        };
        for (const auto ch : text) { if (ch == L',' || iswspace(ch)) add(); else entry += ch; } add();
        return result;
    }
    inline std::wstring FormatGitHubRepoOwners(const std::vector<std::pair<std::wstring, std::wstring>>& pairs)
    {
        std::wstring text;
        for (const auto& [repo, owner] : pairs) { if (!text.empty()) text += L','; text += repo + L'=' + owner; }
        return text;
    }
    // Records one pick, replacing any earlier owner for the same repository.
    inline std::wstring WithGitHubRepoOwner(std::wstring_view text, const std::wstring& repo, const std::wstring& owner)
    {
        auto pairs = GitHubRepoOwners(text);
        std::erase_if(pairs, [&](const auto& pair) { return SameGitHubName(pair.first, repo); });
        if (ValidGitHubRepo(repo) && ValidGitHubOwner(owner)) pairs.emplace_back(repo, owner);
        return FormatGitHubRepoOwners(pairs);
    }
    inline std::wstring GitHubToken(const std::wstring& fallback)
    {
        const auto output = TerminalUtils::CaptureProcess(L"gh.exe auth token --hostname github.com", {}, 4000);
        auto token = til::u8u16(output);
        while (!token.empty() && (token.back() == L'\r' || token.back() == L'\n' || token.back() == L' ')) token.pop_back();
        const auto valid = !token.empty() && std::all_of(token.begin(), token.end(), [](wchar_t ch) { return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'_'; });
        return valid ? token : fallback;
    }
}
