// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Narrows the settings window to one provenance: the settings upstream ships, the
// ones this fork added, or the ones marked experimental. Cards and expanders already
// carry the marks that decide it (IsForkFeature, IsExperimental); this hides the ones
// that do not match, and the section headings left with nothing under them.
//
// View state, like the descriptions switch: MainPage keeps it in state.json, never in
// settings.json, so choosing a filter is not an unsaved edit.

#pragma once

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation::ViewFilter
{
    enum class Mode : int32_t
    {
        All = 0,
        Upstream,
        Fork,
        Experimental,
    };

    Mode Current() noexcept;

    // Applies the mode to every card and expander built so far. A no-op when the mode
    // is already the current one.
    void Set(Mode mode);

    // The state.json spelling. An unknown or empty token reads as All, so a state.json
    // written by a newer build never strands the window on a filter this one lacks.
    Mode FromToken(std::wstring_view token) noexcept;
    std::wstring_view ToToken(Mode mode) noexcept;

    // Whether a setting with these marks is shown. Search uses it, because it has index
    // entries to go on rather than live cards.
    bool Admits(Mode mode, bool isForkFeature, bool isExperimental) noexcept;

    // Pages this fork added outright. Their rows carry no per-row mark -- a diamond on
    // every row of a page that is all ours would say nothing -- so the page is what
    // makes them the fork's. Keep in step with $ForkPages in
    // tools/GenerateSettingsIndex.ps1, which marks the same pages' index entries.
    bool IsForkPageTag(std::wstring_view navigationTag) noexcept;

    // Whether the nav entry with this tag leads to anything the mode shows, judged by
    // the search index: a page is kept if any setting on it is admitted. A page the
    // index has nothing for (Actions, say, whose rows are built at run time) is judged
    // as a whole, as the fork's if it is one of IsForkPageTag's and upstream's if not.
    bool PageAdmits(Mode mode, std::wstring_view navigationTag);

    // Filters a settings page, and remembers it so a later change of mode reaches it.
    // MainPage calls it from the page's Loading: before layout, which is the point. A
    // row collapsed after its panel has measured it keeps the space it was measured
    // at -- the gaps the first version of this left were exactly that, rows hiding
    // themselves from their own OnApplyTemplate in the middle of a measure pass.
    void ApplyToPage(const Windows::UI::Xaml::FrameworkElement& page);

    // A SettingsExpander's rows, from its OnApplyTemplate. They are not in the visual
    // tree yet at that point, nor measured, so this is still ahead of layout.
    void ApplyToExpanderItems(const Windows::UI::Xaml::UIElement& expander);
}
