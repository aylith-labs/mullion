// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Turning a font glyph or emoji into a file the shell can actually draw.
//
// A profile icon is allowed to be a Segoe MDL2 glyph or an emoji, and the
// settings model deliberately resolves those to themselves (see
// MediaResourceSupport.h and the ValidateResolverNotCalledForEmojiIcons test).
// XAML renders such a string directly, but a jump list entry cannot: it takes
// either a file containing an icon or a logo URI, and a bare glyph is neither,
// so the entry draws blank. That is not a rare corner -- the DEFAULT profile
// icon is the glyph U+E756 (MTSMSettings.h), so every profile without an
// explicit icon is affected, as is every SSH profile (SshHostGenerator uses
// U+E977).
//
// So rasterize the glyph ourselves and hand the shell a real .ico. This is the
// approach the maintainers suggested in microsoft/terminal#10552 ("we could
// actually download the image locally and set the path for the jumplist entry
// to that file") applied to glyphs rather than to remote images.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace TerminalApp
{
    class JumplistIconCache
    {
    public:
        // `folder` is the directory beside settings.json the files go in. The
        // taskbar badge (TerminalPage::TaskbarOverlayIconPath) uses its own, so
        // the jump list's PruneUnused never deletes a badge it did not hand out.
        explicit JumplistIconCache(std::wstring_view folder = L"JumplistIcons");

        // Returns the path to an .ico rendering `glyph`, or nullopt if it could
        // not be produced. Never throws: a missing icon is worth degrading over,
        // not worth failing the whole jump list for.
        std::optional<std::wstring> Ensure(std::wstring_view glyph) noexcept;

        // Deletes every cached file that Ensure() did not hand out since this
        // object was constructed. Bounded growth matters here: the cache is keyed
        // partly on the system foreground colour, so a user who changes theme
        // repeatedly would otherwise accumulate a file per theme per glyph.
        void PruneUnused() const noexcept;

    private:
        std::filesystem::path _dir;
        std::unordered_set<std::wstring> _used;
        // The colour glyphs are drawn in, and part of the cache key. The jump
        // list follows the system theme, so a monochrome glyph baked in the
        // wrong colour would be invisible against it.
        uint32_t _foreground{};
        bool _ok{ false };
    };
}
