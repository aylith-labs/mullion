// Shared from Lintel paths/. Run conformance/sync-paths.mjs to update.
#pragma once
#include <string_view>
namespace Lintel {
inline constexpr std::wstring_view windowsPathPattern = LR"lintel((?<![\w/])(?:[A-Za-z]:[\\/]|\\\\[^\s\\/]+[\\/])[^\s<>"\x27`|;,()\[\]{}]+)lintel";
inline constexpr std::wstring_view posixPathPattern = LR"lintel((?<![\w:/\\~])~?/(?!/)[^\s<>"\x27`|;,()\[\]{}]+)lintel";
}
