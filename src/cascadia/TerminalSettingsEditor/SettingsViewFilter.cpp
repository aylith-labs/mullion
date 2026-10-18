// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SettingsViewFilter.h"
#include "SettingsCard.h"
#include "SettingsExpander.h"
#include "GeneratedSettingsIndex.g.h"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation::ViewFilter
{
    namespace
    {
        // Read by search off the UI thread, written on it.
        std::atomic<Mode> g_mode{ Mode::All };

        // An element this filter collapsed, and the Visibility it had of its own.
        //
        // Visibility is not ours to take. About twenty cards bind it in markup
        // (x:Bind to a view model, "only when this font is missing" and the like), and
        // overwriting a bound value for good would leave the card in whatever state it
        // was in when the filter came on. So the filter remembers what the element
        // wanted, keeps listening while it is hidden -- a binding that fires then
        // updates what to restore rather than winning -- and hands that value back.
        struct HiddenByFilter
        {
            winrt::weak_ref<UIElement> element;
            int64_t token{ 0 };
            Visibility own{ Visibility::Visible };
            bool writing{ false };
        };
        std::vector<std::unique_ptr<HiddenByFilter>> g_collapsed;

        struct ForkPage
        {
            std::wstring_view tag;
            std::wstring_view className;
        };
        constexpr std::array ForkPages{
            ForkPage{ L"Activity_Nav", L"Microsoft.Terminal.Settings.Editor.Activity" },
            ForkPage{ L"Documentation_Nav", L"Microsoft.Terminal.Settings.Editor.Documentation" },
            ForkPage{ L"Integrations_Nav", L"Microsoft.Terminal.Settings.Editor.Integrations" },
            ForkPage{ L"LinkTooltip_Nav", L"Microsoft.Terminal.Settings.Editor.LinkTooltip" },
            ForkPage{ L"SessionRestore_Nav", L"Microsoft.Terminal.Settings.Editor.SessionRestore" },
        };

        // Pages that are lists of other pages rather than settings: every profile, every
        // color scheme. Nothing on them is a setting to filter, and their rows are built
        // from data (Profiles keeps its in a ListView inside an expander's ItemsHeader),
        // so hiding by marks only ever emptied them. Whether to show them at all is the
        // nav entry's business (PageAdmits); once open, they are left whole.
        constexpr std::array HubPageClasses{
            std::wstring_view{ L"Microsoft.Terminal.Settings.Editor.Profiles" },
            std::wstring_view{ L"Microsoft.Terminal.Settings.Editor.ColorSchemes" },
        };

        // The header row of an expander is itself a SettingsCard, named in the
        // expander's template. It is the expander's to show or hide, never its own.
        constexpr std::wstring_view ExpanderHeaderCardName{ L"PART_HeaderCard" };

        bool _same(const winrt::Windows::Foundation::IInspectable& a, const winrt::Windows::Foundation::IInspectable& b) noexcept
        {
            return winrt::get_abi(a) == winrt::get_abi(b);
        }

        struct Marks
        {
            bool fork{ false };
            bool experimental{ false };
        };

        // A row's own marks, plus -- for an expander, and only when asked -- whether
        // any row inside it has them. Asked for when deciding whether to show an
        // expander under the fork or experimental filter: a group of upstream
        // settings that has gained one of ours stays, showing only that row.
        Marks _marksOf(const winrt::Windows::Foundation::IInspectable& item, const bool includeItems)
        {
            if (const auto card{ item.try_as<Editor::SettingsCard>() })
            {
                return { card.IsForkFeature(), card.IsExperimental() };
            }
            if (const auto expander{ item.try_as<Editor::SettingsExpander>() })
            {
                Marks marks{ expander.IsForkFeature(), expander.IsExperimental() };
                if (includeItems)
                {
                    if (const auto items{ expander.Items() })
                    {
                        for (const auto& child : items)
                        {
                            const auto inner = _marksOf(child, true);
                            marks.fork |= inner.fork;
                            marks.experimental |= inner.experimental;
                        }
                    }
                }
                return marks;
            }
            return {};
        }

        // What a row inherits from where it sits: every expander it is inside, and the
        // page, which may be one this fork added outright.
        Marks _inheritedMarks(const DependencyObject& from)
        {
            Marks marks;
            for (auto current = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(from); current; current = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(current))
            {
                if (const auto expander{ current.try_as<Editor::SettingsExpander>() })
                {
                    marks.fork |= expander.IsForkFeature();
                    marks.experimental |= expander.IsExperimental();
                }
                else if (current.try_as<Page>())
                {
                    const auto name = winrt::get_class_name(current);
                    for (const auto& page : ForkPages)
                    {
                        marks.fork |= std::wstring_view{ name } == page.className;
                    }
                    break;
                }
            }
            return marks;
        }

        bool _isFilterTarget(const UIElement& element)
        {
            if (const auto card{ element.try_as<Editor::SettingsCard>() })
            {
                return std::wstring_view{ card.Name() } != ExpanderHeaderCardName;
            }
            return static_cast<bool>(element.try_as<Editor::SettingsExpander>());
        }

        // A clickable card opens another page -- the Profiles list, a profile's
        // Appearance / Terminal / Advanced -- rather than being a setting itself, and
        // what it leads to carries the marks, not the card. Hiding it would strand
        // the settings it leads to: the Profiles page, all navigators, came up blank
        // under the fork filter although a profile's Terminal page has one of ours.
        // So under the fork and experimental filters it stays. Under the upstream
        // filter it is judged like any row, because every navigator here is
        // upstream's unless marked.
        bool _isNavigator(const UIElement& element)
        {
            const auto card{ element.try_as<Editor::SettingsCard>() };
            return card && card.IsClickEnabled();
        }

        bool _admitsElement(const Mode mode, const UIElement& element, const Marks& inherited)
        {
            if (mode == Mode::All)
            {
                return true;
            }
            if (mode != Mode::Upstream && _isNavigator(element))
            {
                return true;
            }
            const auto own = _marksOf(element, mode != Mode::Upstream);
            return Admits(mode, own.fork || inherited.fork, own.experimental || inherited.experimental);
        }

        bool _admitsElement(const Mode mode, const UIElement& element)
        {
            return mode == Mode::All || _admitsElement(mode, element, _inheritedMarks(element));
        }

        HiddenByFilter* _find(const UIElement& element)
        {
            for (const auto& entry : g_collapsed)
            {
                if (const auto alive = entry->element.get(); alive && _same(alive, element))
                {
                    return entry.get();
                }
            }
            return nullptr;
        }

        void _setFilteredOut(const UIElement& element, const bool hide)
        {
            auto entry = _find(element);
            if (hide == (entry != nullptr))
            {
                return;
            }

            if (hide)
            {
                auto hidden = std::make_unique<HiddenByFilter>();
                const auto raw = hidden.get();
                raw->element = winrt::make_weak(element);
                raw->own = element.Visibility();
                // raw stays valid for as long as the callback can fire: the entry is
                // only erased after unregistering, and a dead element raises nothing.
                raw->token = element.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [raw](const DependencyObject& sender, const DependencyProperty&) {
                    if (raw->writing)
                    {
                        return;
                    }
                    const auto target = sender.as<UIElement>();
                    raw->own = target.Visibility();
                    if (raw->own == Visibility::Visible)
                    {
                        raw->writing = true;
                        target.Visibility(Visibility::Collapsed);
                        raw->writing = false;
                    }
                });
                raw->writing = true;
                element.Visibility(Visibility::Collapsed);
                raw->writing = false;
                g_collapsed.emplace_back(std::move(hidden));
                return;
            }

            element.UnregisterPropertyChangedCallback(UIElement::VisibilityProperty(), entry->token);
            const auto own = entry->own;
            std::erase_if(g_collapsed, [entry](const auto& candidate) { return candidate.get() == entry; });
            element.Visibility(own);
        }

        // Filters one panel's rows, then its section headings, then -- if it sits in
        // another StackPanel -- the panel itself. Run children first (_applyBelow), so
        // a nested section that ends up empty is already collapsed by the time the
        // panel holding its heading is looked at.
        //
        // A heading is a TextBlock sitting directly in the panel, heading everything
        // after it up to the next one -- which is how every page here lays out its
        // TextBlockSubHeaderStyle sections. It is hidden when the filter hid at least
        // one row in its section and nothing in the section is left showing. Anything
        // else that is visible there (an InfoBar, a button row) keeps it, because the
        // filter has no way to know what that belongs to.
        //
        // Only a StackPanel reads as a run of sections. In a Grid, document order says
        // nothing about which TextBlock heads what, so there the rows are filtered and
        // nothing else is touched.
        void _applyToPanel(const Mode mode, const Panel& panel)
        {
            const auto children = panel.Children();

            for (const auto& child : children)
            {
                if (_isFilterTarget(child))
                {
                    _setFilteredOut(child, !_admitsElement(mode, child));
                }
            }

            if (!panel.try_as<StackPanel>())
            {
                return;
            }

            TextBlock heading{ nullptr };
            auto filtered = false;
            auto shown = false;
            const auto closeSection = [&]() {
                if (heading)
                {
                    _setFilteredOut(heading, mode != Mode::All && filtered && !shown);
                }
            };

            auto panelFiltered = false;
            auto panelShown = false;
            for (const auto& child : children)
            {
                if (const auto text{ child.try_as<TextBlock>() })
                {
                    closeSection();
                    heading = text;
                    filtered = false;
                    shown = false;
                    continue;
                }

                if (_find(child))
                {
                    filtered = true;
                    panelFiltered = true;
                }
                else if (child.Visibility() == Visibility::Visible)
                {
                    shown = true;
                    panelShown = true;
                }
            }
            closeSection();

            // A nested section panel (Appearance groups its sections this way) whose
            // every row is gone collapses too, so it leaves no gap and the panel above
            // sees it as filtered.
            if (winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(panel).try_as<StackPanel>())
            {
                _setFilteredOut(panel, mode != Mode::All && panelFiltered && !panelShown);
            }
        }

        // An expander's rows, decided from the expander rather than from the visual
        // tree: they are not in it yet when the expander applies its template, and
        // that is the moment to decide, before anything has measured them. A row
        // hidden after its panel measured it keeps the space it was measured at.
        void _applyToItems(const Mode mode, const Editor::SettingsExpander& expander)
        {
            const auto items = expander.Items();
            if (!items)
            {
                return;
            }
            auto inherited = _inheritedMarks(expander);
            inherited.fork |= expander.IsForkFeature();
            inherited.experimental |= expander.IsExperimental();
            for (const auto& item : items)
            {
                if (const auto element{ item.try_as<UIElement>() }; element && _isFilterTarget(element))
                {
                    _setFilteredOut(element, !_admitsElement(mode, element, inherited));
                }
            }
        }

        // Everything under a node, children first. Walks the visual tree, so it sees
        // what has been built: a page's own panels as soon as it is constructed, and
        // an expander's rows once its template is applied. Rows of an expander whose
        // template is not applied yet are reached through its Items instead.
        void _applyBelow(const Mode mode, const DependencyObject& node)
        {
            const auto count = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(node);
            for (int32_t i = 0; i < count; ++i)
            {
                _applyBelow(mode, winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(node, i));
            }
            if (const auto expander{ node.try_as<Editor::SettingsExpander>() })
            {
                _applyToItems(mode, expander);
            }
            if (const auto panel{ node.try_as<Panel>() })
            {
                _applyToPanel(mode, panel);
            }
        }

        // Pages the filter has been applied to, so a change of mode reaches the one on
        // screen. Weak, and compacted on each walk: XAML owns them, and nothing
        // reliably says when the settings window lets one go.
        std::vector<winrt::weak_ref<FrameworkElement>> g_pages;
    }

    Mode Current() noexcept
    {
        return g_mode.load(std::memory_order_relaxed);
    }

    void Set(const Mode mode)
    {
        if (g_mode.exchange(mode, std::memory_order_relaxed) == mode)
        {
            return;
        }

        // Outside layout (a menu click), so hiding a row here gives its space back.
        std::erase_if(g_pages, [](const auto& weak) { return !weak.get(); });
        std::erase_if(g_collapsed, [](const auto& entry) { return !entry->element.get(); });
        for (const auto& weak : g_pages)
        {
            if (const auto page = weak.get())
            {
                _applyBelow(mode, page);
            }
        }
    }

    Mode FromToken(const std::wstring_view token) noexcept
    {
        if (token == L"upstream")
        {
            return Mode::Upstream;
        }
        if (token == L"fork")
        {
            return Mode::Fork;
        }
        if (token == L"experimental")
        {
            return Mode::Experimental;
        }
        return Mode::All;
    }

    std::wstring_view ToToken(const Mode mode) noexcept
    {
        switch (mode)
        {
        case Mode::Upstream:
            return L"upstream";
        case Mode::Fork:
            return L"fork";
        case Mode::Experimental:
            return L"experimental";
        default:
            return L"";
        }
    }

    // Upstream means everything the fork did not add, experimental or not: an
    // experimental upstream setting is still upstream's.
    bool Admits(const Mode mode, const bool isForkFeature, const bool isExperimental) noexcept
    {
        switch (mode)
        {
        case Mode::Upstream:
            return !isForkFeature;
        case Mode::Fork:
            return isForkFeature;
        case Mode::Experimental:
            return isExperimental;
        default:
            return true;
        }
    }

    bool IsForkPageTag(const std::wstring_view navigationTag) noexcept
    {
        return std::any_of(ForkPages.begin(), ForkPages.end(), [&](const auto& page) { return page.tag == navigationTag; });
    }

    bool PageAdmits(const Mode mode, const std::wstring_view navigationTag)
    {
        if (mode == Mode::All)
        {
            return true;
        }

        // The Profiles entry leads to the defaults, every profile and the color
        // schemes, so everything indexed under any of them counts for it. The partial
        // indices (per profile, per scheme, per folder) have no tag of their own --
        // their page is a view model at run time -- so they are attributed here.
        const auto attributedTag = [](const IndexEntry& entry) -> std::wstring_view {
            const std::wstring_view tag{ entry.NavigationArgTag };
            if (tag == L"GlobalProfile_Nav" || tag == L"ColorSchemes_Nav")
            {
                return L"Profiles_Nav";
            }
            return tag;
        };

        auto indexed = false;
        const auto scan = [&](const auto& entries, const std::wstring_view impliedTag) {
            for (const auto& entry : entries)
            {
                if ((impliedTag.empty() ? attributedTag(entry) : impliedTag) != navigationTag)
                {
                    continue;
                }
                indexed = true;
                if (Admits(mode, entry.IsForkFeature, entry.IsExperimental))
                {
                    return true;
                }
            }
            return false;
        };

        if (scan(LoadBuildTimeIndex(), {}) ||
            scan(LoadProfileIndex(), L"Profiles_Nav") ||
            scan(LoadColorSchemeIndex(), L"Profiles_Nav") ||
            scan(LoadNTMFolderIndex(), L"NewTabMenu_Nav"))
        {
            return true;
        }
        return !indexed && Admits(mode, IsForkPageTag(navigationTag), false);
    }

    void ApplyToPage(const FrameworkElement& page)
    {
        const auto className = winrt::get_class_name(page);
        if (std::find(HubPageClasses.begin(), HubPageClasses.end(), std::wstring_view{ className }) != HubPageClasses.end())
        {
            return;
        }

        if (std::none_of(g_pages.begin(), g_pages.end(), [&](const auto& weak) {
                const auto alive = weak.get();
                return alive && _same(alive, page);
            }))
        {
            g_pages.emplace_back(winrt::make_weak(page));
        }

        // The common case, and the one every page load pays for: nothing filtered,
        // nothing to restore. Entries for elements XAML has since released do not
        // count, or a filter used once would make every later page pay the walk.
        std::erase_if(g_collapsed, [](const auto& entry) { return !entry->element.get(); });
        const auto mode = Current();
        if (mode == Mode::All && g_collapsed.empty())
        {
            return;
        }
        _applyBelow(mode, page);
    }

    void ApplyToExpanderItems(const UIElement& expander)
    {
        const auto mode = Current();
        if (mode == Mode::All && g_collapsed.empty())
        {
            return;
        }
        if (const auto settingsExpander{ expander.try_as<Editor::SettingsExpander>() })
        {
            _applyToItems(mode, settingsExpander);
        }
    }
}
