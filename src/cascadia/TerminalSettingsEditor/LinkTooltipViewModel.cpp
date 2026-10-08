// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "LinkTooltipViewModel.h"
#include "LinkTooltipViewModel.g.cpp"
#include "RuleGroupViewModel.g.cpp"
#include "HyperlinkTooltipRuleViewModel.g.cpp"
#include "HyperlinkTooltipActionViewModel.g.cpp"
#include "IntegrationChoiceViewModel.g.cpp"
#include "ButtonChoiceViewModel.g.cpp"
#include "EnumEntry.h"
#include "LinkTooltipPresets.h"

// Last, and deliberately: <icu.h> is a large C header full of macros, and the
// rule preview has to compile patterns the way the control does at runtime --
// ICU, not std::wregex, which has no named groups at all. See
// TermControl::_ruleTextMatches for what that difference already cost once.
#include <til/regex.h>

using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // Splits a comma-separated line into trimmed, non-empty tokens -- same convention
    // LinkTooltipViewModel::SafeUriSchemes already uses (moved here verbatim from
    // InteractionViewModel, since it's now needed by both that field and Schemes/
    // CustomExtensions below).
    static std::vector<winrt::hstring> _splitCommaList(const winrt::hstring& value)
    {
        std::vector<winrt::hstring> parts;
        for (const auto& part : til::split_iterator{ std::wstring_view{ value }, L',' })
        {
            if (const auto trimmed = til::trim(part, L' '); !trimmed.empty())
            {
                parts.emplace_back(trimmed);
            }
        }
        return parts;
    }

    static winrt::hstring _joinCommaList(const IVector<winrt::hstring>& values)
    {
        if (!values || values.Size() == 0)
        {
            return {};
        }

        std::wstring joined;
        for (const auto& value : values)
        {
            if (!joined.empty())
            {
                joined.append(L", ");
            }
            joined.append(value);
        }
        return winrt::hstring{ joined };
    }

    // The built-in card buttons, in the order the card draws them. The ids are
    // the ones stored in "hyperlink.tooltipButtons" and in a rule's "buttons".
    static constexpr std::wstring_view KnownButtonIds[]{
        L"open",
        L"copyLink",
        L"copyPath",
        L"reveal",
        L"showInPane",
    };

    static winrt::hstring _buttonLabel(const std::wstring_view id)
    {
        if (id == L"open")
        {
            return RS_(L"LinkTooltip_ButtonOpen");
        }
        if (id == L"copyLink")
        {
            return RS_(L"LinkTooltip_ButtonCopyLink");
        }
        if (id == L"copyPath")
        {
            return RS_(L"LinkTooltip_ButtonCopyPath");
        }
        if (id == L"reveal")
        {
            return RS_(L"LinkTooltip_ButtonReveal");
        }
        if (id == L"showInPane")
        {
            return RS_(L"LinkTooltip_ButtonShowInPane");
        }
        return winrt::hstring{ id };
    }

    // The link kinds hyperlink.clickableKinds can name, in the order the
    // checkboxes are drawn.
    static constexpr std::wstring_view KnownClickableKindIds[]{
        L"detected",
        L"rules",
        L"osc8",
    };

    static winrt::hstring _clickableKindLabel(const std::wstring_view id)
    {
        if (id == L"detected")
        {
            return RS_(L"LinkTooltip_ClickableKindDetected");
        }
        if (id == L"rules")
        {
            return RS_(L"LinkTooltip_ClickableKindRules");
        }
        if (id == L"osc8")
        {
            return RS_(L"LinkTooltip_ClickableKindOsc8");
        }
        return winrt::hstring{ id };
    }

    // What a click chord can be bound to: the same ids the card's buttons use,
    // plus "none". `includeInherit` adds a leading "" entry, which only makes
    // sense on a rule -- the global setting has nothing to inherit from.
    static std::vector<Editor::IntegrationChoiceViewModel> _buildActionChoices(const bool includeInherit)
    {
        std::vector<Editor::IntegrationChoiceViewModel> choices;
        if (includeInherit)
        {
            choices.push_back(winrt::make<IntegrationChoiceViewModel>(winrt::hstring{}, RS_(L"LinkTooltip_ActionInherit")));
        }
        choices.push_back(winrt::make<IntegrationChoiceViewModel>(winrt::hstring{ L"none" }, RS_(L"LinkTooltip_ActionNone")));
        for (const auto& id : KnownButtonIds)
        {
            choices.push_back(winrt::make<IntegrationChoiceViewModel>(winrt::hstring{ id }, _buttonLabel(id)));
        }
        return choices;
    }

    // An id already in use that isn't one of the built-ins -- an entry under
    // "actions", say -- needs an entry of its own, or the picker would show
    // nothing selected and the first edit would silently rewrite the setting.
    // Done while building the list rather than lazily on lookup, so the entry is
    // already there the first time the picker asks what is selected.
    static void _ensureActionChoice(const IVector<Editor::IntegrationChoiceViewModel>& choices, const winrt::hstring& id)
    {
        if (!choices || id.empty())
        {
            return;
        }
        for (const auto& choice : choices)
        {
            if (choice.Id() == id)
            {
                return;
            }
        }
        choices.Append(winrt::make<IntegrationChoiceViewModel>(id, id));
    }

    // IMap::Lookup throws hresult_out_of_bounds when the key is absent, and these
    // maps are read from x:Bind getters that XAML calls while dispatching
    // PropertyChanged. An exception there does not surface as a failed binding -- it
    // escapes the callback and fails the process fast (0xC000041D,
    // STATUS_FATAL_USER_CALLBACK_EXCEPTION), taking every window with it. A combo box
    // showing nothing selected is the right failure for a value the list does not
    // contain.
    template<typename K>
    static Windows::Foundation::IInspectable _lookupEnumEntry(const IMap<K, Editor::EnumEntry>& map, const K& key)
    {
        if (!map || !map.HasKey(key))
        {
            return nullptr;
        }
        return map.Lookup(key);
    }

    // Shared by the four action pickers: pure lookup, since a binding getter is
    // no place to be mutating the collection the binding reads.
    static Windows::Foundation::IInspectable _actionChoiceFor(const IVector<Editor::IntegrationChoiceViewModel>& choices,
                                                              const winrt::hstring& current)
    {
        if (!choices)
        {
            return nullptr;
        }
        for (const auto& choice : choices)
        {
            if (choice.Id() == current)
            {
                return choice;
            }
        }
        return nullptr;
    }

    static bool _listContains(const IVector<winrt::hstring>& list, const winrt::hstring& id)
    {
        if (!list)
        {
            return false;
        }
        for (const auto& value : list)
        {
            if (value == id)
            {
                return true;
            }
        }
        return false;
    }

    static bool _isKnownButtonId(const winrt::hstring& id)
    {
        return std::find(std::begin(KnownButtonIds), std::end(KnownButtonIds), std::wstring_view{ id }) != std::end(KnownButtonIds);
    }

    // Rebuilds an id list with one button turned on or off. The result is always
    // in KnownButtonIds order -- so the card draws them the same way however the
    // boxes were ticked -- and keeps any id we don't know about, which is how a
    // hand-written settings.json survives a round trip through this page.
    static IVector<winrt::hstring> _withButton(const IVector<winrt::hstring>& list, const winrt::hstring& id, bool selected)
    {
        std::vector<winrt::hstring> ordered;
        for (const auto& known : KnownButtonIds)
        {
            const winrt::hstring knownId{ known };
            const auto wanted = knownId == id ? selected : _listContains(list, knownId);
            if (wanted)
            {
                ordered.push_back(knownId);
            }
        }
        if (list)
        {
            for (const auto& value : list)
            {
                if (!_isKnownButtonId(value))
                {
                    ordered.push_back(value);
                }
            }
        }
        return single_threaded_vector<winrt::hstring>(std::move(ordered));
    }

    // Toggles one id in a list, rewriting it in `known` order. The same shape as
    // _withButton, but over whichever id table is passed in -- the clickable-kind
    // checkboxes need this behaviour over a different set of ids, and unlike the
    // button list they have no unknown ids to preserve.
    template<typename KnownIds>
    static IVector<winrt::hstring> _withIdToggled(const IVector<winrt::hstring>& list,
                                                  const KnownIds& known,
                                                  const winrt::hstring& id,
                                                  bool selected)
    {
        std::vector<winrt::hstring> ordered;
        for (const auto& entry : known)
        {
            const winrt::hstring knownId{ entry };
            const auto wanted = knownId == id ? selected : _listContains(list, knownId);
            if (wanted)
            {
                ordered.push_back(knownId);
            }
        }
        return single_threaded_vector<winrt::hstring>(std::move(ordered));
    }

    // What to show in a rule's "Integration" picker for a given id. "" and "none"
    // are not integration ids at all -- they're the two ways of saying "no specific
    // integration" -- so they get their own localized labels.
    static winrt::hstring _integrationDisplayName(const winrt::hstring& id)
    {
        if (id.empty())
        {
            return RS_(L"LinkTooltip_IntegrationAutomatic");
        }
        if (id == L"none")
        {
            return RS_(L"LinkTooltip_IntegrationNone");
        }
        return id;
    }

    static std::vector<Editor::IntegrationChoiceViewModel> _buildIntegrationChoices(const IVector<Model::HyperlinkTooltipRule>& rules)
    {
        std::vector<Editor::IntegrationChoiceViewModel> choices;
        choices.push_back(make<IntegrationChoiceViewModel>(winrt::hstring{}, _integrationDisplayName({})));
        choices.push_back(make<IntegrationChoiceViewModel>(winrt::hstring{ L"none" }, _integrationDisplayName(winrt::hstring{ L"none" })));
        if (const auto manifests = Model::IntegrationRegistry::All())
        {
            for (const auto& manifest : manifests)
            {
                const auto id = manifest.Id();
                if (id.empty())
                {
                    continue;
                }
                const auto name = manifest.Name();
                choices.push_back(make<IntegrationChoiceViewModel>(id, name.empty() ? id : name));
            }
        }
        for (const auto& preset : GetLinkTooltipPresets())
        {
            if (!preset.integration.empty())
            {
                const winrt::hstring presetId{ preset.integration };
                const auto known = std::any_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.Id() == presetId; });
                if (!known)
                {
                    choices.push_back(make<IntegrationChoiceViewModel>(presetId, _integrationDisplayName(presetId)));
                }
            }
        }
        if (rules)
        {
            for (const auto& rule : rules)
            {
                if (const auto current = rule.Integration(); !current.empty())
                {
                    const auto known = std::any_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.Id() == current; });
                    if (!known)
                    {
                        choices.push_back(make<IntegrationChoiceViewModel>(current, current));
                    }
                }
            }
        }
        return choices;
    }

    // ---- Rule preview ---------------------------------------------------
    //
    // What the card at the top of the rule editor shows: the sample line, with
    // the run this rule would actually pick out of it highlighted, and the named
    // captures the preview pipeline would read back from it. It used to show the
    // rule's own summary -- the pattern and the integration -- which is the same
    // regex the Pattern field two rows below already shows, so it demonstrated
    // nothing.

    // The named capture groups a pattern declares, in the order it declares them.
    // Same scan (and same rules about what ICU will accept as a name) as
    // HyperlinkPreviewService::CollectGroupNames, because these are the names that
    // service will look for when it builds a card from this rule.
    static std::vector<std::wstring> _namedGroups(const std::wstring_view pattern)
    {
        std::vector<std::wstring> names;
        for (size_t i = 0; i + 3 < pattern.size(); ++i)
        {
            if (pattern[i] != L'(' || pattern[i + 1] != L'?' || pattern[i + 2] != L'<')
            {
                continue;
            }
            // A backslash before the '(' makes it a literal.
            if (i > 0 && pattern[i - 1] == L'\\')
            {
                continue;
            }
            // "(?<=" and "(?<!" are lookbehind, not a group name.
            if (pattern[i + 3] == L'=' || pattern[i + 3] == L'!')
            {
                continue;
            }
            const auto close = pattern.find(L'>', i + 3);
            if (close == std::wstring_view::npos)
            {
                continue;
            }
            const auto name = pattern.substr(i + 3, close - i - 3);
            // ICU's rule: a letter followed by letters and digits. An underscore
            // makes uregex_open reject the whole pattern, so a name carrying one
            // could never come back anyway.
            const auto usable = !name.empty() &&
                                ((name.front() >= L'a' && name.front() <= L'z') || (name.front() >= L'A' && name.front() <= L'Z')) &&
                                std::all_of(name.begin(), name.end(), [](wchar_t ch) {
                                    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
                                });
            if (usable)
            {
                names.emplace_back(name);
            }
        }
        return names;
    }

    // The scheme of a sample line, by the same reading TermControl applies to a
    // hovered link: a bare POSIX path has no scheme of its own and counts as file.
    static std::wstring _sampleScheme(const std::wstring_view sample)
    {
        if (sample.empty())
        {
            return {};
        }
        if (sample.front() == L'/')
        {
            return L"file";
        }
        const auto colon = sample.find(L':');
        if (colon == std::wstring_view::npos || colon == 0)
        {
            return {};
        }
        std::wstring scheme{ sample.substr(0, colon) };
        for (auto& ch : scheme)
        {
            if (ch >= L'A' && ch <= L'Z')
            {
                ch = static_cast<wchar_t>(ch - L'A' + L'a');
            }
            else if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'+' || ch == L'-' || ch == L'.'))
            {
                return {};
            }
        }
        return scheme;
    }

    // The sample line to seed a rule's preview with. Rules do not record the preset
    // they came from, so this identifies one the same way IsPresetInUse does: by
    // name first (what the list calls the rule), then by pattern. A rule that is
    // nobody's preset starts with an empty box and the invitation to type in it.
    static std::wstring_view _presetSampleFor(const Model::HyperlinkTooltipRule& rule)
    {
        if (!rule)
        {
            return {};
        }

        const std::wstring_view name{ rule.Name() };
        const std::wstring_view pattern{ rule.Pattern() };
        for (const auto& preset : GetLinkTooltipPresets())
        {
            if (preset.name == name || (!pattern.empty() && preset.pattern == pattern))
            {
                return GetLinkTooltipPresetSample(preset.id);
            }
        }
        return {};
    }

    struct RulePreview
    {
        bool matched{ false };
        std::wstring before;
        std::wstring match;
        std::wstring after;
        std::wstring captures;
        std::wstring status;
    };

    static RulePreview _computeRulePreview(const Model::HyperlinkMatchKind kind,
                                           const std::wstring_view pattern,
                                           const IVector<winrt::hstring>& schemes,
                                           const std::wstring_view sample)
    {
        RulePreview result;

        if (sample.empty())
        {
            result.status = L"Type a line above to see what this rule picks out of it.";
            return result;
        }

        if (pattern.empty())
        {
            result.status = kind == Model::HyperlinkMatchKind::Text ?
                                L"A text rule with no pattern has nothing to look for, so it never matches." :
                                L"No pattern: this rule applies to every link that passes its scheme and file-type criteria.";
            return result;
        }

        UErrorCode status = U_ZERO_ERROR;
        const auto re = til::ICU::CreateRegex(pattern, UREGEX_CASE_INSENSITIVE, &status);
        if (U_FAILURE(status) || !re)
        {
            // Exactly what happens at runtime: an uncompilable pattern is not an
            // error anywhere, it simply never matches. Saying so here is the whole
            // point of the card -- this is where a bad pattern becomes visible
            // instead of silently costing you every card the rule was meant to draw.
            result.status = L"This pattern is not valid, so the rule can never match. ICU syntax, not JavaScript: capture group names must be letters and digits only.";
            return result;
        }

#pragma warning(suppress : 26490) // Don't use reinterpret_cast (type.1).
        uregex_setText(re.get(), reinterpret_cast<const UChar*>(sample.data()), gsl::narrow_cast<int32_t>(sample.size()), &status);
        if (U_FAILURE(status))
        {
            result.status = L"No match.";
            return result;
        }

        // uregex_find, not uregex_matches, for both kinds. A link rule is matched
        // against the whole URI that way at runtime, and a text rule's pattern is
        // first what the buffer scanner uses to find the run in the first place --
        // so "what would this find in this line" is the honest question here.
        if (!uregex_find(re.get(), 0, &status) || U_FAILURE(status))
        {
            result.status = L"No match.";
            return result;
        }

        status = U_ZERO_ERROR;
        // Not `start`/`end`: winrt::begin/end are what iterate the scheme list
        // further down, and a local called `end` hides the free function.
        const auto matchStart = uregex_start(re.get(), 0, &status);
        const auto matchEnd = uregex_end(re.get(), 0, &status);
        if (U_FAILURE(status) || matchStart < 0 || matchEnd < matchStart || gsl::narrow_cast<size_t>(matchEnd) > sample.size())
        {
            result.status = L"No match.";
            return result;
        }

        result.matched = true;
        result.before = sample.substr(0, gsl::narrow_cast<size_t>(matchStart));
        result.match = sample.substr(gsl::narrow_cast<size_t>(matchStart), gsl::narrow_cast<size_t>(matchEnd - matchStart));
        result.after = sample.substr(gsl::narrow_cast<size_t>(matchEnd));
        result.status = L"Matches.";

        for (const auto& name : _namedGroups(pattern))
        {
            status = U_ZERO_ERROR;
#pragma warning(suppress : 26490) // Don't use reinterpret_cast (type.1).
            const auto number = uregex_groupNumberFromName(re.get(), reinterpret_cast<const UChar*>(name.data()), gsl::narrow_cast<int32_t>(name.size()), &status);
            if (U_FAILURE(status) || number <= 0)
            {
                continue;
            }

            status = U_ZERO_ERROR;
            const auto groupStart = uregex_start(re.get(), number, &status);
            const auto groupEnd = uregex_end(re.get(), number, &status);
            if (U_FAILURE(status) || groupStart < 0 || groupEnd < groupStart || gsl::narrow_cast<size_t>(groupEnd) > sample.size())
            {
                continue;
            }

            if (!result.captures.empty())
            {
                result.captures += L"  ·  ";
            }
            result.captures += name;
            result.captures += L" = ";
            result.captures += sample.substr(gsl::narrow_cast<size_t>(groupStart), gsl::narrow_cast<size_t>(groupEnd - groupStart));
        }

        // A link rule's scheme list is a criterion in its own right, so a pattern
        // that matches is only half the answer. Say so rather than showing a green
        // match for a rule that would never be consulted for this link.
        if (kind == Model::HyperlinkMatchKind::Link && schemes && schemes.Size() > 0)
        {
            const auto scheme = _sampleScheme(sample);
            const auto allowed = std::any_of(begin(schemes), end(schemes), [&](const auto& s) {
                return til::equals_insensitive_ascii(std::wstring_view{ s }, scheme);
            });
            if (!allowed)
            {
                result.status = L"The pattern matches, but this rule only applies to ";
                result.status += std::wstring_view{ _joinCommaList(schemes) };
                result.status += L" links.";
            }
        }

        return result;
    }

    HyperlinkTooltipRuleViewModel::HyperlinkTooltipRuleViewModel(Model::HyperlinkTooltipRule rule,
                                                                 Model::WindowSettings windowSettings,
                                                                 IObservableVector<Editor::EnumEntry> kindList,
                                                                 IMap<Model::HyperlinkMatchKind, Editor::EnumEntry> kindMap,
                                                                 IObservableVector<Editor::EnumEntry> fileTypeGroupList,
                                                                 IMap<Model::HyperlinkFileTypeGroup, Editor::EnumEntry> fileTypeGroupMap,
                                                                 IObservableVector<Editor::IntegrationChoiceViewModel> integrationChoices,
                                                                 IObservableVector<Editor::IntegrationChoiceViewModel> actionChoices) :
        _Rule{ rule },
        _WindowSettings{ windowSettings },
        _KindList{ kindList },
        _KindMap{ kindMap },
        _FileTypeGroupList{ fileTypeGroupList },
        _FileTypeGroupMap{ fileTypeGroupMap },
        _IntegrationChoices{ integrationChoices },
        _ActionChoices{ actionChoices }
    {
        if (!_Rule.CustomActions())
        {
            _Rule.CustomActions(winrt::single_threaded_vector<Model::HyperlinkTooltipAction>());
        }

        // A rule with no list of its own inherits the global one, so the boxes
        // show what the card would actually do -- greyed out until the override
        // is switched on.
        std::vector<Editor::ButtonChoiceViewModel> buttonVMs;
        for (const auto& id : KnownButtonIds)
        {
            const winrt::hstring buttonId{ id };
            buttonVMs.push_back(make<ButtonChoiceViewModel>(
                buttonId,
                _buttonLabel(id),
                // Weak, not [this]: these live inside ButtonChoiceViewModels that
                // XAML holds through its ItemsSource, so they outlive this view
                // model whenever the page is torn down or rebuilt while a box is
                // still bound. A raw this would be dangling by then, and the
                // read happens during teardown.
                [weakThis = winrt::weak_ref<HyperlinkTooltipRuleViewModel>{ get_weak() }](const winrt::hstring& which) {
                    const auto self = weakThis.get();
                    if (!self)
                    {
                        return false;
                    }
                    const auto own = self->_Rule.Buttons();
                    return _listContains(own && own.Size() > 0 ? own : self->_WindowSettings.HyperlinkTooltipButtons(), which);
                },
                [weakThis = winrt::weak_ref<HyperlinkTooltipRuleViewModel>{ get_weak() }](const winrt::hstring& which, bool selected) {
                    const auto self = weakThis.get();
                    if (!self)
                    {
                        return;
                    }
                    if (!self->OverrideButtons())
                    {
                        // The boxes are disabled while the rule inherits; don't
                        // let a stray write turn the override on by accident.
                        return;
                    }
                    const auto updated = _withButton(self->_Rule.Buttons(), which, selected);
                    if (updated.Size() == 0)
                    {
                        // An empty list is how a rule says "inherit", so it can't
                        // also mean "show nothing": refuse to clear the last box.
                        // ButtonChoiceViewModel re-reads Selected afterwards, so
                        // the checkbox snaps back on its own.
                        return;
                    }
                    self->_Rule.Buttons(updated);
                }));
        }
        _ButtonChoices = single_threaded_observable_vector<Editor::ButtonChoiceViewModel>(std::move(buttonVMs));

        // A rule's action pickers carry an extra leading "inherit" entry the global
        // ones don't have, so this list can't be the page's own ActionChoices -- but
        // it is still ONE list shared by every rule, handed in by the page. It must
        // not be per-rule: the two pickers bind their ItemsSource to it, and
        // replacing a ComboBox's ItemsSource while it still holds a selection from
        // the previous list throws, which silently abandoned the rest of the
        // "current rule changed" binding update and left the whole editor blank.
        // Only the fallback constructor below, which has no page to share with,
        // builds one of its own.
        if (!_ActionChoices)
        {
            _ActionChoices = single_threaded_observable_vector<Editor::IntegrationChoiceViewModel>(_buildActionChoices(true));
        }
        _ensureActionChoice(_ActionChoices, _Rule.PrimaryAction());
        _ensureActionChoice(_ActionChoices, _Rule.AlternativeAction());

        // Seed the preview with the line the rule's preset is meant to match, so
        // the card demonstrates something the moment it opens.
        _PreviewSample = winrt::hstring{ _presetSampleFor(_Rule) };
        _storePreview();

        std::vector<Editor::HyperlinkTooltipActionViewModel> actionVMs;
        for (const auto& action : _Rule.CustomActions())
        {
            actionVMs.push_back(make<HyperlinkTooltipActionViewModel>(action));
        }
        _CustomActions = single_threaded_observable_vector<Editor::HyperlinkTooltipActionViewModel>(std::move(actionVMs));

        _customActionsChangedRevoker = _CustomActions.VectorChanged(winrt::auto_revoke, [this](auto&&, const IVectorChangedEventArgs& args) {
            switch (args.CollectionChange())
            {
            case CollectionChange::Reset:
            {
                std::vector<Model::HyperlinkTooltipAction> modelActions;
                for (const auto& vm : _CustomActions)
                {
                    modelActions.push_back(get_self<HyperlinkTooltipActionViewModel>(vm)->Action());
                }
                _Rule.CustomActions(single_threaded_vector<Model::HyperlinkTooltipAction>(std::move(modelActions)));
                return;
            }
            case CollectionChange::ItemInserted:
            {
                const auto& vm = _CustomActions.GetAt(args.Index());
                _Rule.CustomActions().InsertAt(args.Index(), get_self<HyperlinkTooltipActionViewModel>(vm)->Action());
                return;
            }
            case CollectionChange::ItemRemoved:
                _Rule.CustomActions().RemoveAt(args.Index());
                return;
            case CollectionChange::ItemChanged:
            {
                const auto& vm = _CustomActions.GetAt(args.Index());
                _Rule.CustomActions().SetAt(args.Index(), get_self<HyperlinkTooltipActionViewModel>(vm)->Action());
                return;
            }
            }
        });
    }

    HyperlinkTooltipRuleViewModel::HyperlinkTooltipRuleViewModel(Model::HyperlinkTooltipRule rule, Model::WindowSettings windowSettings) :
        HyperlinkTooltipRuleViewModel(rule, windowSettings, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)
    {
        INITIALIZE_BINDABLE_ENUM_SETTING(FileTypeGroup, HyperlinkFileTypeGroup, Model::HyperlinkFileTypeGroup, L"LinkTooltip_FileTypeGroup", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(Kind, HyperlinkMatchKind, Model::HyperlinkMatchKind, L"LinkTooltip_MatchKind", L"Content");
        auto choices = _buildIntegrationChoices(nullptr);
        if (const auto current = _Rule.Integration(); !current.empty())
        {
            const auto known = std::any_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.Id() == current; });
            if (!known)
            {
                choices.push_back(make<IntegrationChoiceViewModel>(current, current));
            }
        }
        _IntegrationChoices = single_threaded_observable_vector<Editor::IntegrationChoiceViewModel>(std::move(choices));
    }

    Windows::Foundation::IInspectable HyperlinkTooltipRuleViewModel::CurrentPrimaryAction() const
    {
        return _actionChoiceFor(_ActionChoices, _Rule.PrimaryAction());
    }

    void HyperlinkTooltipRuleViewModel::CurrentPrimaryAction(const Windows::Foundation::IInspectable& value)
    {
        if (const auto choice = value.try_as<Editor::IntegrationChoiceViewModel>(); choice && _Rule.PrimaryAction() != choice.Id())
        {
            _Rule.PrimaryAction(choice.Id());

        }
    }

    Windows::Foundation::IInspectable HyperlinkTooltipRuleViewModel::CurrentAlternativeAction() const
    {
        return _actionChoiceFor(_ActionChoices, _Rule.AlternativeAction());
    }

    void HyperlinkTooltipRuleViewModel::CurrentAlternativeAction(const Windows::Foundation::IInspectable& value)
    {
        if (const auto choice = value.try_as<Editor::IntegrationChoiceViewModel>(); choice && _Rule.AlternativeAction() != choice.Id())
        {
            _Rule.AlternativeAction(choice.Id());

        }
    }

    void HyperlinkTooltipRuleViewModel::Name(const hstring& value)
    {
        if (_Rule.Name() != value)
        {
            _Rule.Name(value);
            _NotifyChanges(L"Name", L"DisplayName");
        }
    }

    hstring HyperlinkTooltipRuleViewModel::DisplayName() const
    {
        const auto name = _Rule.Name();
        return !name.empty() ? name : RS_(L"LinkTooltip_NewRulePlaceholder");
    }

    winrt::Windows::Foundation::IInspectable HyperlinkTooltipRuleViewModel::CurrentKind() const
    {
        if (_KindMap && _KindMap.HasKey(_Rule.Kind()))
        {
            return winrt::box_value<Editor::EnumEntry>(_KindMap.Lookup(_Rule.Kind()));
        }
        return nullptr;
    }

    void HyperlinkTooltipRuleViewModel::CurrentKind(const winrt::Windows::Foundation::IInspectable& enumEntry)
    {
        if (auto ee = enumEntry.try_as<Editor::EnumEntry>())
        {
            auto setting = winrt::unbox_value<Model::HyperlinkMatchKind>(ee.EnumValue());
            if (_Rule.Kind() != setting)
            {
                _Rule.Kind(setting);
                _recomputePreview();
                _NotifyChanges(L"IsLinkKind", L"IsTextKind", L"SummaryText");
            }
        }
    }

    winrt::Windows::Foundation::IInspectable HyperlinkTooltipRuleViewModel::CurrentFileTypeGroup() const
    {
        if (_FileTypeGroupMap && _FileTypeGroupMap.HasKey(_Rule.FileTypeGroup()))
        {
            return winrt::box_value<Editor::EnumEntry>(_FileTypeGroupMap.Lookup(_Rule.FileTypeGroup()));
        }
        return nullptr;
    }

    void HyperlinkTooltipRuleViewModel::CurrentFileTypeGroup(const winrt::Windows::Foundation::IInspectable& enumEntry)
    {
        if (auto ee = enumEntry.try_as<Editor::EnumEntry>())
        {
            auto setting = winrt::unbox_value<Model::HyperlinkFileTypeGroup>(ee.EnumValue());
            if (_Rule.FileTypeGroup() != setting)
            {
                _Rule.FileTypeGroup(setting);
                _recomputePreview();
                _NotifyChanges(L"SummaryText");
            }
        }
    }

    winrt::Windows::Foundation::IInspectable HyperlinkTooltipRuleViewModel::CurrentIntegrationChoice() const
    {
        const auto current = _Rule.Integration();
        if (_IntegrationChoices)
        {
            for (const auto& choice : _IntegrationChoices)
            {
                if (choice.Id() == current)
                {
                    return choice;
                }
            }
            if (_IntegrationChoices.Size() > 0)
            {
                return _IntegrationChoices.GetAt(0);
            }
        }
        return nullptr;
    }

    void HyperlinkTooltipRuleViewModel::CurrentIntegrationChoice(const winrt::Windows::Foundation::IInspectable& value)
    {
        if (!value)
        {
            return;
        }
        const auto choice = value.try_as<Editor::IntegrationChoiceViewModel>();
        if (!choice)
        {
            return;
        }
        if (_Rule.Integration() != choice.Id())
        {
            _Rule.Integration(choice.Id());
            _NotifyChanges(L"Integration", L"SummaryText");
        }
    }

    hstring HyperlinkTooltipRuleViewModel::SummaryText() const
    {
        // A text rule matches terminal output rather than links, so none of the
        // link criteria below apply to it -- what matters is the pattern and who
        // previews what it finds.
        if (_Rule.Kind() == Model::HyperlinkMatchKind::Text)
        {
            std::wstring summary{ L"text: " };
            summary += std::wstring_view{ _Rule.Pattern() };
            summary += L"  →  ";
            summary += std::wstring_view{ _integrationDisplayName(_Rule.Integration()) };
            return winrt::hstring{ summary };
        }

        std::wstring parts;
        if (const auto schemes = _Rule.Schemes(); schemes && schemes.Size() > 0)
        {
            parts += L"scheme: ";
            parts += std::wstring_view{ _joinCommaList(schemes) };
        }
        if (const auto pattern = _Rule.Pattern(); !pattern.empty())
        {
            if (!parts.empty())
            {
                parts += L"  ·  ";
            }
            parts += L"pattern: ";
            parts += std::wstring_view{ pattern };
        }
        if (_Rule.FileTypeGroup() != Model::HyperlinkFileTypeGroup::None)
        {
            if (!parts.empty())
            {
                parts += L"  ·  ";
            }
            parts += L"file type";
        }
        return winrt::hstring{ parts };
    }

    void HyperlinkTooltipRuleViewModel::PreviewSample(const hstring& value)
    {
        if (_PreviewSample == value)
        {
            return;
        }
        _PreviewSample = value;
        _recomputePreview();
        _NotifyChanges(L"PreviewSample");
    }

    // Storing without announcing, so the constructor can seed the card without
    // raising an event from a half-built object.
    void HyperlinkTooltipRuleViewModel::_storePreview()
    {
        const auto preview = _computeRulePreview(_Rule.Kind(),
                                                 std::wstring_view{ _Rule.Pattern() },
                                                 _Rule.Schemes(),
                                                 std::wstring_view{ _PreviewSample });

        _HasPreviewMatch = preview.matched;
        _PreviewBefore = winrt::hstring{ preview.before };
        _PreviewMatch = winrt::hstring{ preview.match };
        _PreviewAfter = winrt::hstring{ preview.after };
        _PreviewCaptures = winrt::hstring{ preview.captures };
        _PreviewStatus = winrt::hstring{ preview.status };
    }

    void HyperlinkTooltipRuleViewModel::_recomputePreview()
    {
        _storePreview();
        _NotifyChanges(L"HasPreviewMatch",
                       L"PreviewBefore",
                       L"PreviewMatch",
                       L"PreviewAfter",
                       L"PreviewCaptures",
                       L"HasPreviewCaptures",
                       L"PreviewStatus");
    }

    // Opening a rule announces one property -- CurrentRule -- and XAML answers it
    // by walking every ViewModel.CurrentRule.* binding on the page in a single
    // generated function. That function is straight-line code with no error
    // handling, and it runs inside a PropertyChanged delegate, which
    // winrt::impl::invoke wraps in a catch-all: the first target that throws
    // silently abandons every binding after it, with no crash and nothing logged.
    // That is exactly what happened here -- a ComboBox whose ItemsSource was being
    // replaced while it still held a selection from the previous rule -- and the
    // symptom was an editor that filled in the first time and came up completely
    // blank on every visit afterwards.
    //
    // The ItemsSource swap is gone (see the shared RuleActionChoices list), but the
    // page should not depend on that one batch surviving in the first place. Each
    // name here is a separate event raise, so one bad target can only ever cost its
    // own field.
    //
    // Safe to do now in a way it was not before: a Current* setter behind a
    // ComboBox no longer announces its own property, so re-announcing one cannot
    // start the write-read-write loop that used to exhaust the stack.
    void HyperlinkTooltipRuleViewModel::NotifyAllProperties()
    {
        _NotifyChanges(L"Name",
                       L"DisplayName",
                       L"Enabled",
                       L"SummaryText",
                       L"CurrentKind",
                       L"IsLinkKind",
                       L"IsTextKind",
                       L"Schemes",
                       L"Pattern",
                       L"CurrentFileTypeGroup",
                       L"CustomExtensions",
                       L"Integration",
                       L"CurrentIntegrationChoice",
                       L"ShowPreview",
                       L"OverrideShowDelay",
                       L"ShowDelay",
                       L"OverrideHideDelay",
                       L"HideDelay",
                       L"OverrideMaxWidth",
                       L"MaxWidth",
                       L"OverrideButtons",
                       L"ButtonChoices",
                       L"OverrideShowInPane",
                       L"ShowInPane",
                       L"ActionChoices",
                       L"CurrentPrimaryAction",
                       L"CurrentAlternativeAction",
                       L"CustomActions",
                       L"PreviewSample",
                       L"HasPreviewMatch",
                       L"PreviewBefore",
                       L"PreviewMatch",
                       L"PreviewAfter",
                       L"PreviewCaptures",
                       L"HasPreviewCaptures",
                       L"PreviewStatus");
    }

    hstring HyperlinkTooltipRuleViewModel::Schemes() const
    {
        return _joinCommaList(_Rule.Schemes());
    }

    // Guarded, because the two-way binding writes back whatever it was just handed:
    // without this, every refresh of the box rewrote the rule's scheme list with an
    // identical copy of itself and announced it as a change.
    void HyperlinkTooltipRuleViewModel::Schemes(const hstring& value)
    {
        if (Schemes() == value)
        {
            return;
        }
        auto parts = _splitCommaList(value);
        _Rule.Schemes(parts.empty() ? nullptr : single_threaded_vector<hstring>(std::move(parts)));
        _recomputePreview();
        _NotifyChanges(L"Schemes", L"SummaryText");
    }

    hstring HyperlinkTooltipRuleViewModel::CustomExtensions() const
    {
        return _joinCommaList(_Rule.CustomExtensions());
    }

    void HyperlinkTooltipRuleViewModel::CustomExtensions(const hstring& value)
    {
        if (CustomExtensions() == value)
        {
            return;
        }
        auto parts = _splitCommaList(value);
        _Rule.CustomExtensions(parts.empty() ? nullptr : single_threaded_vector<hstring>(std::move(parts)));
        _NotifyChanges(L"CustomExtensions");
    }

    void HyperlinkTooltipRuleViewModel::OverrideShowDelay(bool value)
    {
        if (OverrideShowDelay() == value)
        {
            return;
        }
        if (value)
        {
            _Rule.TooltipShowDelay(winrt::Windows::Foundation::IReference<int32_t>{ _ShadowShowDelay });
        }
        else
        {
            _ShadowShowDelay = ShowDelay();
            _Rule.TooltipShowDelay(nullptr);
        }
        _NotifyChanges(L"OverrideShowDelay", L"ShowDelay");
    }

    int32_t HyperlinkTooltipRuleViewModel::ShowDelay() const noexcept
    {
        if (const auto value = _Rule.TooltipShowDelay())
        {
            return value.Value();
        }
        return _ShadowShowDelay;
    }

    // The number box is disabled until the override checkbox is ticked, so the only
    // writes that can arrive while the override is off are the binding pushing the
    // displayed default straight back at us -- and writing that to the rule would
    // turn the override on and save a delay nobody chose, just for opening the rule.
    // (Every one of those checkboxes came up ticked on a rule that had never been
    // touched.) Keep the number for the next time the box is ticked, the way the
    // pair is documented to behave, but leave the rule alone. Same instinct as the
    // button checkboxes, which already refuse a stray write for the same reason.
    void HyperlinkTooltipRuleViewModel::ShowDelay(int32_t value)
    {
        _ShadowShowDelay = value;
        if (!OverrideShowDelay())
        {
            return;
        }
        _Rule.TooltipShowDelay(winrt::Windows::Foundation::IReference<int32_t>{ value });
        _NotifyChanges(L"ShowDelay", L"OverrideShowDelay");
    }

    void HyperlinkTooltipRuleViewModel::OverrideHideDelay(bool value)
    {
        if (OverrideHideDelay() == value)
        {
            return;
        }
        if (value)
        {
            _Rule.TooltipHideDelay(winrt::Windows::Foundation::IReference<int32_t>{ _ShadowHideDelay });
        }
        else
        {
            _ShadowHideDelay = HideDelay();
            _Rule.TooltipHideDelay(nullptr);
        }
        _NotifyChanges(L"OverrideHideDelay", L"HideDelay");
    }

    int32_t HyperlinkTooltipRuleViewModel::HideDelay() const noexcept
    {
        if (const auto value = _Rule.TooltipHideDelay())
        {
            return value.Value();
        }
        return _ShadowHideDelay;
    }

    void HyperlinkTooltipRuleViewModel::HideDelay(int32_t value)
    {
        _ShadowHideDelay = value;
        if (!OverrideHideDelay())
        {
            return;
        }
        _Rule.TooltipHideDelay(winrt::Windows::Foundation::IReference<int32_t>{ value });
        _NotifyChanges(L"HideDelay", L"OverrideHideDelay");
    }

    void HyperlinkTooltipRuleViewModel::OverrideMaxWidth(bool value)
    {
        if (OverrideMaxWidth() == value)
        {
            return;
        }
        if (value)
        {
            _Rule.TooltipMaxWidth(winrt::Windows::Foundation::IReference<int32_t>{ _ShadowMaxWidth });
        }
        else
        {
            _ShadowMaxWidth = MaxWidth();
            _Rule.TooltipMaxWidth(nullptr);
        }
        _NotifyChanges(L"OverrideMaxWidth", L"MaxWidth");
    }

    int32_t HyperlinkTooltipRuleViewModel::MaxWidth() const noexcept
    {
        if (const auto value = _Rule.TooltipMaxWidth())
        {
            return value.Value();
        }
        return _ShadowMaxWidth;
    }

    void HyperlinkTooltipRuleViewModel::MaxWidth(int32_t value)
    {
        _ShadowMaxWidth = value;
        if (!OverrideMaxWidth())
        {
            return;
        }
        _Rule.TooltipMaxWidth(winrt::Windows::Foundation::IReference<int32_t>{ value });
        _NotifyChanges(L"MaxWidth", L"OverrideMaxWidth");
    }

    bool HyperlinkTooltipRuleViewModel::OverrideButtons() const noexcept
    {
        const auto buttons = _Rule.Buttons();
        return buttons && buttons.Size() > 0;
    }

    void HyperlinkTooltipRuleViewModel::OverrideButtons(bool value)
    {
        if (OverrideButtons() == value)
        {
            return;
        }

        if (value)
        {
            // Start from what the rule was already inheriting, so switching the
            // override on doesn't silently change which buttons the card shows.
            std::vector<winrt::hstring> inherited;
            if (const auto defaults = _WindowSettings.HyperlinkTooltipButtons())
            {
                for (const auto& id : defaults)
                {
                    inherited.push_back(id);
                }
            }
            if (inherited.empty())
            {
                // There is nothing to copy, and an empty list would read as
                // "inherit" again -- so seed it with the one button that is
                // useful for every link.
                inherited.emplace_back(L"copyLink");
            }
            _Rule.Buttons(single_threaded_vector<winrt::hstring>(std::move(inherited)));
        }
        else
        {
            _Rule.Buttons(nullptr);
        }

        _NotifyChanges(L"OverrideButtons");
        _raiseButtonChoicesChanged();
    }

    void HyperlinkTooltipRuleViewModel::_raiseButtonChoicesChanged()
    {
        for (const auto& choice : _ButtonChoices)
        {
            get_self<ButtonChoiceViewModel>(choice)->RaiseSelectedChanged();
        }
    }

    void HyperlinkTooltipRuleViewModel::OverrideShowInPane(bool value)
    {
        if (OverrideShowInPane() == value)
        {
            return;
        }
        if (value)
        {
            _Rule.ShowInPane(winrt::Windows::Foundation::IReference<bool>{ _ShadowShowInPane });
        }
        else
        {
            _ShadowShowInPane = ShowInPane();
            _Rule.ShowInPane(nullptr);
        }
        _NotifyChanges(L"OverrideShowInPane", L"ShowInPane");
    }

    bool HyperlinkTooltipRuleViewModel::ShowInPane() const noexcept
    {
        if (const auto value = _Rule.ShowInPane())
        {
            return value.Value();
        }
        return _ShadowShowInPane;
    }

    void HyperlinkTooltipRuleViewModel::ShowInPane(bool value)
    {
        _ShadowShowInPane = value;
        if (!OverrideShowInPane())
        {
            return;
        }
        _Rule.ShowInPane(winrt::Windows::Foundation::IReference<bool>{ value });
        _NotifyChanges(L"ShowInPane", L"OverrideShowInPane");
    }

    Editor::HyperlinkTooltipActionViewModel HyperlinkTooltipRuleViewModel::RequestAddCustomAction()
    {
        Model::HyperlinkTooltipAction action{};
        const auto vm = make<HyperlinkTooltipActionViewModel>(action);
        _CustomActions.Append(vm);
        return vm;
    }

    void HyperlinkTooltipRuleViewModel::RequestDeleteCustomAction(const Editor::HyperlinkTooltipActionViewModel& vm)
    {
        uint32_t idx;
        if (_CustomActions.IndexOf(vm, idx))
        {
            _CustomActions.RemoveAt(idx);
        }
    }

    void HyperlinkTooltipRuleViewModel::ApplyPreset(const winrt::hstring& presetId)
    {
        const auto preset = FindLinkTooltipPreset(presetId);
        if (!preset)
        {
            return;
        }

        _Rule.Name(winrt::hstring{ preset->name });
        _Rule.Enabled(true);
        _Rule.Kind(preset->kind);
        _Rule.Pattern(winrt::hstring{ preset->pattern });
        if (!preset->schemes.empty())
        {
            std::vector<winrt::hstring> schemes;
            for (const auto& s : preset->schemes)
            {
                schemes.emplace_back(s);
            }
            _Rule.Schemes(winrt::single_threaded_vector<winrt::hstring>(std::move(schemes)));
        }
        else
        {
            _Rule.Schemes(nullptr);
        }
        _Rule.FileTypeGroup(preset->fileTypeGroup);
        if (!preset->customExtensions.empty())
        {
            std::vector<winrt::hstring> exts;
            for (const auto& e : preset->customExtensions)
            {
                exts.emplace_back(e);
            }
            _Rule.CustomExtensions(winrt::single_threaded_vector<winrt::hstring>(std::move(exts)));
        }
        else
        {
            _Rule.CustomExtensions(nullptr);
        }
        _Rule.Integration(winrt::hstring{ preset->integration });
        if (!preset->integration.empty() && _IntegrationChoices)
        {
            const winrt::hstring intId{ preset->integration };
            const auto known = std::any_of(begin(_IntegrationChoices), end(_IntegrationChoices), [&](const auto& choice) {
                return choice.Id() == intId;
            });
            if (!known)
            {
                _IntegrationChoices.Append(make<IntegrationChoiceViewModel>(intId, _integrationDisplayName(intId)));
            }
        }
        _Rule.ShowPreview(preset->showPreview);
        _Rule.TooltipShowDelay(nullptr);
        _Rule.TooltipHideDelay(nullptr);
        _Rule.TooltipMaxWidth(nullptr);
        _Rule.Buttons(nullptr);
        _Rule.ShowInPane(nullptr);

        // The preset knows what it is meant to match, so the card can demonstrate
        // it straight away. An edited sample is left alone -- it is the user's test
        // input, not the preset's.
        if (_PreviewSample.empty())
        {
            _PreviewSample = winrt::hstring{ GetLinkTooltipPresetSample(presetId) };
            _NotifyChanges(L"PreviewSample");
        }
        _recomputePreview();

        _NotifyChanges(L"Name",
                       L"DisplayName",
                       L"Enabled",
                       L"Pattern",
                       L"CurrentKind",
                       L"IsLinkKind",
                       L"IsTextKind",
                       L"Schemes",
                       L"CurrentFileTypeGroup",
                       L"CustomExtensions",
                       L"Integration",
                       L"CurrentIntegrationChoice",
                       L"ShowPreview",
                       L"SummaryText",
                       L"OverrideShowDelay",
                       L"ShowDelay",
                       L"OverrideHideDelay",
                       L"HideDelay",
                       L"OverrideMaxWidth",
                       L"MaxWidth",
                       L"OverrideButtons",
                       L"OverrideShowInPane",
                       L"ShowInPane");
    }

    RuleGroupViewModel::RuleGroupViewModel(winrt::hstring platformId,
                                           winrt::hstring platformName,
                                           winrt::hstring platformIcon,
                                           std::vector<Editor::HyperlinkTooltipRuleViewModel> rules) :
        _platformId{ std::move(platformId) },
        _platformName{ std::move(platformName) },
        _platformIcon{ std::move(platformIcon) },
        _rules{ winrt::single_threaded_observable_vector<Editor::HyperlinkTooltipRuleViewModel>(std::move(rules)) }
    {
        for (const auto& r : _rules)
        {
            winrt::weak_ref<RuleGroupViewModel> weakThis{ get_weak() };
            r.PropertyChanged([weakThis](auto&&, const Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
                if (auto strong = weakThis.get())
                {
                    if (args.PropertyName() == L"Enabled")
                    {
                        strong->RefreshState();
                    }
                }
            });
        }
    }

    winrt::hstring RuleGroupViewModel::CountBadge() const
    {
        if (!_rules)
        {
            return winrt::hstring{ L"0/0" };
        }
        uint32_t enabled = 0;
        const auto total = _rules.Size();
        for (const auto& r : _rules)
        {
            if (r.Enabled())
            {
                enabled++;
            }
        }
        return winrt::hstring{ fmt::format(L"{}/{}", enabled, total) };
    }

    bool RuleGroupViewModel::IsEnabled() const
    {
        if (!_rules || _rules.Size() == 0)
        {
            return false;
        }
        for (const auto& r : _rules)
        {
            if (r.Enabled())
            {
                return true;
            }
        }
        return false;
    }

    void RuleGroupViewModel::IsEnabled(bool value)
    {
        if (!_rules)
        {
            return;
        }
        for (const auto& r : _rules)
        {
            r.Enabled(value);
        }
        RefreshState();
    }

    void RuleGroupViewModel::RefreshState()
    {
        _NotifyChanges(L"IsEnabled", L"CountBadge");
    }

    static IVector<Model::HyperlinkTooltipRule> _ensureRules(const Model::WindowSettings& windowSettings)
    {
        auto rules = windowSettings.HyperlinkTooltipRules();
        if (!rules)
        {
            rules = winrt::single_threaded_vector<Model::HyperlinkTooltipRule>();
            windowSettings.HyperlinkTooltipRules(rules);
        }
        return rules;
    }

    static std::wstring _toLower(std::wstring_view s)
    {
        std::wstring res{ s };
        std::transform(res.begin(), res.end(), res.begin(), ::towlower);
        return res;
    }

    static winrt::hstring _classifyRulePlatform(const Editor::HyperlinkTooltipRuleViewModel& vm)
    {
        const auto name = _toLower(vm.DisplayName());
        const auto pattern = _toLower(vm.Pattern());
        const auto integration = _toLower(vm.Integration());
        const auto schemes = _toLower(vm.Schemes());

        if (integration == L"unblocked") return L"unblocked";

        if (integration.find(L"github") != std::wstring::npos ||
            name.find(L"github") != std::wstring::npos ||
            pattern.find(L"github") != std::wstring::npos)
        {
            return L"github";
        }
        if (integration.find(L"jira") != std::wstring::npos ||
            name.find(L"jira") != std::wstring::npos ||
            pattern.find(L"jira") != std::wstring::npos ||
            pattern.find(L"browse/") != std::wstring::npos)
        {
            return L"jira";
        }
        if (integration.find(L"slack") != std::wstring::npos ||
            name.find(L"slack") != std::wstring::npos ||
            pattern.find(L"slack") != std::wstring::npos)
        {
            return L"slack";
        }
        if (integration.find(L"stith") != std::wstring::npos ||
            name.find(L"stith") != std::wstring::npos)
        {
            return L"stith";
        }
        // Before the "git" test below, which would otherwise claim anything
        // whose name merely starts with those three letters.
        if (integration.find(L"shefrd") != std::wstring::npos ||
            name.find(L"shefrd") != std::wstring::npos ||
            name.find(L"herdr") != std::wstring::npos ||
            name.find(L"pane") != std::wstring::npos)
        {
            return L"shefrd";
        }
        if (name.find(L"git") != std::wstring::npos ||
            name.find(L"commit") != std::wstring::npos ||
            pattern.find(L"commit") != std::wstring::npos ||
            schemes.find(L"git") != std::wstring::npos)
        {
            return L"git";
        }
        if (schemes.find(L"file") != std::wstring::npos ||
            name.find(L"file") != std::wstring::npos ||
            name.find(L"markdown") != std::wstring::npos ||
            name.find(L"media") != std::wstring::npos ||
            name.find(L"path") != std::wstring::npos)
        {
            return L"files";
        }
        return L"custom";
    }

    // The order _updateRuleGroups draws the groups in, and therefore the order
    // automatic sorting puts the rules in. "custom" is last, so ungrouped rules
    // sit below the grouped ones -- and, since order is precedence, lose to them.
    static constexpr std::wstring_view GroupOrder[]{
        L"github",
        // Specific task IDs must precede Jira's broad PROJECT-number matcher.
        L"unblocked",
        L"jira",
        L"slack",
        L"stith",
        L"shefrd",
        L"git",
        L"files",
        L"custom",
    };

    static size_t _groupRank(const winrt::hstring& platform)
    {
        for (size_t i = 0; i < std::size(GroupOrder); ++i)
        {
            if (GroupOrder[i] == platform)
            {
                return i;
            }
        }
        return std::size(GroupOrder);
    }

    LinkTooltipViewModel::LinkTooltipViewModel(Model::GlobalAppSettings globalSettings, Model::WindowSettings windowSettings) :
        _GlobalSettings{ globalSettings },
        _WindowSettings{ windowSettings }
    {
        auto rules = _ensureRules(_WindowSettings);

        std::vector<Editor::ButtonChoiceViewModel> buttonVMs;
        for (const auto& id : KnownButtonIds)
        {
            const winrt::hstring buttonId{ id };
            buttonVMs.push_back(make<ButtonChoiceViewModel>(
                buttonId,
                _buttonLabel(id),
                // The settings object, not [this]: these outlive the view model
                // whenever XAML still holds the ButtonChoiceViewModels through its
                // ItemsSource. _WindowSettings is set once in the constructor and
                // never reassigned, so a captured copy is the same object -- and it
                // keeps itself alive.
                [windowSettings = _WindowSettings](const winrt::hstring& which) {
                    return _listContains(windowSettings.HyperlinkTooltipButtons(), which);
                },
                [windowSettings = _WindowSettings](const winrt::hstring& which, bool selected) {
                    windowSettings.HyperlinkTooltipButtons(_withButton(windowSettings.HyperlinkTooltipButtons(), which, selected));
                }));
        }
        _ButtonChoices = single_threaded_observable_vector<Editor::ButtonChoiceViewModel>(std::move(buttonVMs));

        INITIALIZE_BINDABLE_ENUM_SETTING(FileTypeGroup, HyperlinkFileTypeGroup, Model::HyperlinkFileTypeGroup, L"LinkTooltip_FileTypeGroup", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(Kind, HyperlinkMatchKind, Model::HyperlinkMatchKind, L"LinkTooltip_MatchKind", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(IntegrationDisplayMode, HyperlinkIntegrationDisplayMode, Model::HyperlinkIntegrationDisplayMode, L"LinkTooltip_IntegrationDisplayMode", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(ActionPlacement, HyperlinkActionPlacement, Model::HyperlinkActionPlacement, L"LinkTooltip_ActionPlacement", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(ClickModifier, HyperlinkClickModifier, Model::HyperlinkClickModifier, L"LinkTooltip_ClickModifier", L"Content");
        INITIALIZE_BINDABLE_ENUM_SETTING(ClickGesture, HyperlinkClickGesture, Model::HyperlinkClickGesture, L"LinkTooltip_ClickGesture", L"Content");
        _IntegrationChoices = single_threaded_observable_vector<Editor::IntegrationChoiceViewModel>(_buildIntegrationChoices(rules));
        _ActionChoices = single_threaded_observable_vector<Editor::IntegrationChoiceViewModel>(_buildActionChoices(false));
        _ensureActionChoice(_ActionChoices, _WindowSettings.HyperlinkPrimaryAction());
        _ensureActionChoice(_ActionChoices, _WindowSettings.HyperlinkAlternativeAction());

        // The same list plus a leading "inherit" entry, built once here and handed
        // to every rule -- not built per rule. The rule editor's two action pickers
        // bind their ItemsSource to it, and a ComboBox that is handed a different
        // collection while it still holds a selection from the old one throws; that
        // throw took the rest of the "current rule changed" binding update with it
        // and left the whole editor blank on every visit after the first. One list
        // means the ItemsSource never changes, and every choice a rule can select
        // is by construction an item of the collection the picker is showing.
        _RuleActionChoices = single_threaded_observable_vector<Editor::IntegrationChoiceViewModel>(_buildActionChoices(true));

        std::vector<Editor::ButtonChoiceViewModel> kindVMs;
        for (const auto& id : KnownClickableKindIds)
        {
            const winrt::hstring kindId{ id };
            kindVMs.push_back(make<ButtonChoiceViewModel>(
                kindId,
                _clickableKindLabel(id),
                [windowSettings = _WindowSettings](const winrt::hstring& which) {
                    // An unset list is the shipped default -- every kind -- rather
                    // than "no kinds", matching how the control resolves it.
                    const auto kinds = windowSettings.HyperlinkClickableKinds();
                    return !kinds ? true : _listContains(kinds, which);
                },
                [windowSettings = _WindowSettings](const winrt::hstring& which, bool selected) {
                    auto kinds = windowSettings.HyperlinkClickableKinds();
                    if (!kinds)
                    {
                        kinds = single_threaded_vector<winrt::hstring>();
                        for (const auto& id : KnownClickableKindIds)
                        {
                            kinds.Append(winrt::hstring{ id });
                        }
                    }
                    windowSettings.HyperlinkClickableKinds(_withIdToggled(kinds, KnownClickableKindIds, which, selected));
                }));
        }
        _ClickableKindChoices = single_threaded_observable_vector<Editor::ButtonChoiceViewModel>(std::move(kindVMs));

        std::vector<Editor::HyperlinkTooltipRuleViewModel> ruleVMs;
        for (const auto& rule : rules)
        {
            ruleVMs.push_back(make<HyperlinkTooltipRuleViewModel>(rule, _WindowSettings, _KindList, _KindMap, _FileTypeGroupList, _FileTypeGroupMap, _IntegrationChoices, _RuleActionChoices));
        }
        _CurrentView = single_threaded_observable_vector<Editor::HyperlinkTooltipRuleViewModel>(std::move(ruleVMs));
        _RuleGroups = single_threaded_observable_vector<Editor::RuleGroupViewModel>();
        _updateRuleGroups();

        _rulesChangedRevoker = _CurrentView.VectorChanged(winrt::auto_revoke, [this](auto&&, const IVectorChangedEventArgs& args) {
            auto rules = _ensureRules(_WindowSettings);

            switch (args.CollectionChange())
            {
            case CollectionChange::Reset:
            {
                std::vector<Model::HyperlinkTooltipRule> modelRules;
                for (const auto& vm : _CurrentView)
                {
                    modelRules.push_back(get_self<HyperlinkTooltipRuleViewModel>(vm)->Rule());
                }
                _WindowSettings.HyperlinkTooltipRules(single_threaded_vector<Model::HyperlinkTooltipRule>(std::move(modelRules)));
                break;
            }
            case CollectionChange::ItemInserted:
            {
                const auto& vm = _CurrentView.GetAt(args.Index());
                rules.InsertAt(args.Index(), get_self<HyperlinkTooltipRuleViewModel>(vm)->Rule());
                break;
            }
            case CollectionChange::ItemRemoved:
                if (args.Index() < rules.Size())
                {
                    rules.RemoveAt(args.Index());
                }
                break;
            case CollectionChange::ItemChanged:
            {
                const auto& vm = _CurrentView.GetAt(args.Index());
                if (args.Index() < rules.Size())
                {
                    rules.SetAt(args.Index(), get_self<HyperlinkTooltipRuleViewModel>(vm)->Rule());
                }
                break;
            }
            }
            _updateRuleGroups();
        });

        // Only now that the write-through above is armed, so the sort reaches
        // hyperlink.tooltipRules rather than only the list on screen.
        _applyAutomaticOrder();
    }

    // With manual ordering off, the list you see IS the precedence order rather
    // than a display that lies about it: the model is kept sorted by group rank
    // (Custom Rules last) and then by name. Turning manual ordering on simply
    // stops this running, freezing the current order as the starting point.
    void LinkTooltipViewModel::_applyAutomaticOrder()
    {
        if (!_CurrentView || _reorderingInProgress || _WindowSettings.HyperlinkManualRuleOrder())
        {
            return;
        }

        std::vector<Editor::HyperlinkTooltipRuleViewModel> sorted{ begin(_CurrentView), end(_CurrentView) };
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& lhs, const auto& rhs) {
            const auto lhsRank = _groupRank(_classifyRulePlatform(lhs));
            const auto rhsRank = _groupRank(_classifyRulePlatform(rhs));
            if (lhsRank != rhsRank)
            {
                return lhsRank < rhsRank;
            }
            return _toLower(lhs.DisplayName()) < _toLower(rhs.DisplayName());
        });

        // Already in order. Returning without touching the vector is what stops
        // the VectorChanged handler below from calling back in here forever --
        // see the stack overflow this page has already had once.
        if (std::equal(sorted.begin(), sorted.end(), begin(_CurrentView), end(_CurrentView)))
        {
            return;
        }

        _reorderingInProgress = true;
        _CurrentView.ReplaceAll(sorted);
        _reorderingInProgress = false;
    }

    void LinkTooltipViewModel::_updateRuleGroups()
    {
        if (!_RuleGroups)
        {
            _RuleGroups = winrt::single_threaded_observable_vector<Editor::RuleGroupViewModel>();
        }

        struct GroupInfo
        {
            std::wstring id;
            std::wstring name;
            std::wstring icon;
            std::vector<Editor::HyperlinkTooltipRuleViewModel> rules;
        };

        std::vector<GroupInfo> groups = {
            { L"github", L"GitHub", L"\uE82D", {} },
            { L"unblocked", L"Unblocked Code", L"\uE943", {} },
            { L"jira", L"Jira", L"\uE943", {} },
            { L"slack", L"Slack", L"\uE8BD", {} },
            { L"stith", L"Stith", L"\uE774", {} },
            { L"shefrd", L"shefrd", L"\uF0E2", {} },
            { L"git", L"Git", L"\uE81D", {} },
            { L"files", L"Files & Media", L"\uE8A5", {} },
            { L"custom", L"Custom Rules", L"\uE713", {} }
        };

        if (_CurrentView)
        {
            for (const auto& vm : _CurrentView)
            {
                const auto platform = _classifyRulePlatform(vm);
                bool found = false;
                for (auto& g : groups)
                {
                    if (g.id == platform)
                    {
                        g.rules.push_back(vm);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    groups.back().rules.push_back(vm);
                }
            }
        }

        _RuleGroups.Clear();
        for (auto& g : groups)
        {
            if (!g.rules.empty())
            {
                _RuleGroups.Append(winrt::make<RuleGroupViewModel>(
                    winrt::hstring{ g.id },
                    winrt::hstring{ g.name },
                    winrt::hstring{ g.icon },
                    std::move(g.rules)));
            }
        }
        _NotifyChanges(L"RuleGroups");
    }

    // A Current* setter behind a ComboBox must NOT announce its own property.
    //
    // These are the write half of SelectedItem="{x:Bind ..., Mode=TwoWay}". The
    // binding calls the setter because the selection already changed; telling it
    // "this property changed" makes it re-read the getter and assign SelectedItem
    // again, the ComboBox raises its change, the binding calls the setter again --
    // and round it goes until the stack is gone. It is not a hang and not an
    // exception: the process simply dies about five seconds later with
    // STATUS_FATAL_USER_CALLBACK_EXCEPTION, one interaction after the click that
    // started it, which is why it reads as "any dropdown crashes at random".
    //
    // Caught with a debugger: the captured stack is CItemsControl::SetValue ->
    // NotifyPropertyChanged -> our callback -> SetValueByKnownIndex -> repeat.
    // GETSET_BINDABLE_ENUM_SETTING omits the notification for exactly this reason,
    // which is why every page using the macro is unaffected and only this page,
    // whose setters are hand-written, crashes.
    //
    // Announcing a *different* property is fine and is what the remaining calls do.
    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentIntegrationDisplayMode() const
    {
        return _lookupEnumEntry(_IntegrationDisplayModeMap, _WindowSettings.HyperlinkIntegrationDisplayMode());
    }

    void LinkTooltipViewModel::CurrentIntegrationDisplayMode(const Windows::Foundation::IInspectable& enumEntry)
    {
        if (const auto entry = enumEntry.try_as<Editor::EnumEntry>())
        {
            const auto value = winrt::unbox_value<Model::HyperlinkIntegrationDisplayMode>(entry.EnumValue());
            _WindowSettings.HyperlinkIntegrationDisplayMode(value);
            _NotifyChanges(L"HyperlinkIntegrationDisplayMode");
        }
    }

    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentActionPlacement() const
    {
        return _lookupEnumEntry(_ActionPlacementMap, _WindowSettings.HyperlinkActionPlacement());
    }

    void LinkTooltipViewModel::CurrentActionPlacement(const Windows::Foundation::IInspectable& enumEntry)
    {
        if (const auto entry = enumEntry.try_as<Editor::EnumEntry>())
        {
            const auto value = winrt::unbox_value<Model::HyperlinkActionPlacement>(entry.EnumValue());
            _WindowSettings.HyperlinkActionPlacement(value);
            _NotifyChanges(L"HyperlinkActionPlacement");
        }
    }

    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentPrimaryClickModifier() const
    {
        return _lookupEnumEntry(_ClickModifierMap, _WindowSettings.HyperlinkPrimaryClickModifier());
    }

    void LinkTooltipViewModel::CurrentPrimaryClickModifier(const Windows::Foundation::IInspectable& enumEntry)
    {
        if (const auto entry = enumEntry.try_as<Editor::EnumEntry>())
        {
            _WindowSettings.HyperlinkPrimaryClickModifier(winrt::unbox_value<Model::HyperlinkClickModifier>(entry.EnumValue()));

        }
    }

    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentPrimaryClickGesture() const
    {
        return _lookupEnumEntry(_ClickGestureMap, _WindowSettings.HyperlinkPrimaryClickGesture());
    }

    void LinkTooltipViewModel::CurrentPrimaryClickGesture(const Windows::Foundation::IInspectable& enumEntry)
    {
        if (const auto entry = enumEntry.try_as<Editor::EnumEntry>())
        {
            _WindowSettings.HyperlinkPrimaryClickGesture(winrt::unbox_value<Model::HyperlinkClickGesture>(entry.EnumValue()));

        }
    }

    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentAlternativeClickModifier() const
    {
        return _lookupEnumEntry(_ClickModifierMap, _WindowSettings.HyperlinkAlternativeClickModifier());
    }

    void LinkTooltipViewModel::CurrentAlternativeClickModifier(const Windows::Foundation::IInspectable& enumEntry)
    {
        if (const auto entry = enumEntry.try_as<Editor::EnumEntry>())
        {
            _WindowSettings.HyperlinkAlternativeClickModifier(winrt::unbox_value<Model::HyperlinkClickModifier>(entry.EnumValue()));

        }
    }

    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentAlternativeClickGesture() const
    {
        return _lookupEnumEntry(_ClickGestureMap, _WindowSettings.HyperlinkAlternativeClickGesture());
    }

    void LinkTooltipViewModel::CurrentAlternativeClickGesture(const Windows::Foundation::IInspectable& enumEntry)
    {
        if (const auto entry = enumEntry.try_as<Editor::EnumEntry>())
        {
            _WindowSettings.HyperlinkAlternativeClickGesture(winrt::unbox_value<Model::HyperlinkClickGesture>(entry.EnumValue()));

        }
    }

    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentPrimaryAction() const
    {
        return _actionChoiceFor(_ActionChoices, _WindowSettings.HyperlinkPrimaryAction());
    }

    void LinkTooltipViewModel::CurrentPrimaryAction(const Windows::Foundation::IInspectable& value)
    {
        if (const auto choice = value.try_as<Editor::IntegrationChoiceViewModel>())
        {
            _WindowSettings.HyperlinkPrimaryAction(choice.Id());

        }
    }

    Windows::Foundation::IInspectable LinkTooltipViewModel::CurrentAlternativeAction() const
    {
        return _actionChoiceFor(_ActionChoices, _WindowSettings.HyperlinkAlternativeAction());
    }

    void LinkTooltipViewModel::CurrentAlternativeAction(const Windows::Foundation::IInspectable& value)
    {
        if (const auto choice = value.try_as<Editor::IntegrationChoiceViewModel>())
        {
            _WindowSettings.HyperlinkAlternativeAction(choice.Id());

        }
    }

    winrt::hstring LinkTooltipViewModel::SafeUriSchemes() const
    {
        return _joinCommaList(_WindowSettings.SafeUriSchemes());
    }

    void LinkTooltipViewModel::SafeUriSchemes(const winrt::hstring& value)
    {
        // An empty box stores an empty list rather than clearing the setting. The default
        // is not empty (it lists "stith"), so clearing would put the default back and the
        // box would refill the moment it was emptied -- and the card promises that an
        // empty box confirms every scheme.
        _WindowSettings.SafeUriSchemes(winrt::single_threaded_vector<winrt::hstring>(_splitCommaList(value)));

        _NotifyChanges(L"SafeUriSchemes");
    }

    // Opening or closing a rule is what swaps the page between the list and the
    // editor, and it is also what MainPage watches to push (or pop) the
    // "Link Tooltip > <rule>" breadcrumb. Renaming the open rule has to reach
    // the breadcrumb too, hence the subscription.
    void LinkTooltipViewModel::CurrentRule(const Editor::HyperlinkTooltipRuleViewModel& vm)
    {
        _currentRuleChangedRevoker.revoke();
        const auto wasEditing = static_cast<bool>(_CurrentRule);
        _CurrentRule = vm;
        if (!_CurrentRule && wasEditing)
        {
            _updateRuleGroups();
        }
        _NotifyChanges(L"CurrentRule", L"IsEditingRule", L"IsNotEditingRule", L"CurrentRuleName");

        if (_CurrentRule)
        {
            // "CurrentRule changed" is one event, and XAML answers it by running
            // every ViewModel.CurrentRule.* binding on the page from a single
            // generated function -- so one target that throws costs you all the
            // ones after it, silently. Announce the rule's own properties as well,
            // which is a separate event each, so opening a rule fills the editor in
            // even if that batch does not survive. See NotifyAllProperties.
            //
            // Before the rename subscription below, not after: this raises Name and
            // DisplayName, and the crumb has just been written from the same rule.
            if (const auto self = get_self<HyperlinkTooltipRuleViewModel>(_CurrentRule))
            {
                self->NotifyAllProperties();
            }

            _currentRuleChangedRevoker = _CurrentRule.PropertyChanged(winrt::auto_revoke, [this](auto&&, const Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
                if (args.PropertyName() == L"Name" || args.PropertyName() == L"DisplayName")
                {
                    _NotifyChanges(L"CurrentRuleName");
                }
            });
        }
    }

    void LinkTooltipViewModel::RequestDeleteRule(const Editor::HyperlinkTooltipRuleViewModel& vm)
    {
        uint32_t idx;
        if (CurrentView().IndexOf(vm, idx))
        {
            CurrentView().RemoveAt(idx);
            if (_CurrentRule == vm)
            {
                CurrentRule(nullptr);
            }
        }
    }

    Editor::HyperlinkTooltipRuleViewModel LinkTooltipViewModel::RequestDuplicateRule(const Editor::HyperlinkTooltipRuleViewModel& source)
    {
        if (!source) return nullptr;
        const auto original = get_self<HyperlinkTooltipRuleViewModel>(source)->Rule();
        const auto rule = original.Copy();
        const std::wstring base = (original.Name().empty() ? std::wstring{ L"Rule" } : std::wstring{ original.Name() }) + L" (copy)";
        std::wstring name = base;
        size_t count = 2;
        while (std::any_of(CurrentView().begin(), CurrentView().end(), [&](const auto& vm) { return vm.Name() == name; })) name = base + L" " + std::to_wstring(count++);
        rule.Name(name);
        const auto vm = make<HyperlinkTooltipRuleViewModel>(rule, _WindowSettings, _KindList, _KindMap, _FileTypeGroupList, _FileTypeGroupMap, _IntegrationChoices, _RuleActionChoices);
        CurrentView().Append(vm);
        _applyAutomaticOrder();
        CurrentRule(vm);
        return vm;
    }

    Editor::HyperlinkTooltipRuleViewModel LinkTooltipViewModel::RequestAddRule()
    {
        _ensureRules(_WindowSettings);
        Model::HyperlinkTooltipRule rule{};
        rule.Enabled(true);
        rule.CustomActions(winrt::single_threaded_vector<Model::HyperlinkTooltipAction>());
        const auto vm = make<HyperlinkTooltipRuleViewModel>(rule, _WindowSettings, _KindList, _KindMap, _FileTypeGroupList, _FileTypeGroupMap, _IntegrationChoices, _RuleActionChoices);
        CurrentView().Append(vm);
        // A blank rule classifies as Custom, which sorts last anyway, so this
        // leaves it at the end -- where the user is about to start editing it.
        _applyAutomaticOrder();
        return vm;
    }

    // Whether the rules list already contains this preset, so the Add rule menu can
    // grey it out instead of silently making a duplicate that matches the same links
    // twice. Rules do not record the preset they came from, so this compares what a
    // preset actually writes: the name, and the pattern where there is one. A preset
    // whose pattern is empty (the file-type ones) is identified by name and file-type
    // group, which is the only thing that distinguishes those from each other.
    bool LinkTooltipViewModel::IsPresetInUse(const winrt::hstring& presetId) const
    {
        return _isPresetInUse(presetId, nullptr);
    }

    // The Apply preset menu's variant. Applying to the rule that already is that
    // preset re-syncs it -- which is a legitimate thing to want, and was in fact
    // the only way to get a rule's fields back while the editor was coming up
    // blank -- so it must not count as a duplicate.
    bool LinkTooltipViewModel::IsPresetInUseElsewhere(const winrt::hstring& presetId) const
    {
        Model::HyperlinkTooltipRule except{ nullptr };
        if (_CurrentRule)
        {
            if (const auto self = get_self<HyperlinkTooltipRuleViewModel>(_CurrentRule))
            {
                except = self->Rule();
            }
        }
        return _isPresetInUse(presetId, except);
    }

    bool LinkTooltipViewModel::_isPresetInUse(const winrt::hstring& presetId, const Model::HyperlinkTooltipRule& except) const
    {
        const auto preset = FindLinkTooltipPreset(presetId);
        if (!preset)
        {
            return false;
        }

        const auto rules = _WindowSettings.HyperlinkTooltipRules();
        if (!rules)
        {
            return false;
        }

        const std::wstring_view presetName{ preset->name };
        const std::wstring_view presetPattern{ preset->pattern };

        for (const auto& rule : rules)
        {
            if (!rule || (except && rule == except))
            {
                continue;
            }

            // Name first. It is what identifies the preset in the menu, and a rule
            // whose pattern has since been edited -- or which was added before the
            // preset's own pattern changed -- is still that preset to the person
            // reading the list. Observed live: the shipped repo#number preset now
            // matches \b(?<repo>[A-Za-z0-9_.-]+)#\d+\b while a rule added earlier
            // still stores (?<repo>[a-z-]+)#\d+, so a pattern-only test offered it
            // again as though it were missing.
            if (std::wstring_view{ rule.Name() } == presetName || std::find(preset->legacyNames.begin(), preset->legacyNames.end(), std::wstring_view{ rule.Name() }) != preset->legacyNames.end())
            {
                return true;
            }

            if (!presetPattern.empty())
            {
                if (std::wstring_view{ rule.Pattern() } == presetPattern)
                {
                    return true;
                }
                continue;
            }

            // A preset with no pattern of its own (the file-type ones) is only
            // distinguishable by its file-type group once the name has not matched.
            const auto extensions = rule.CustomExtensions();
            const auto schemes = rule.Schemes();
            const auto sameExtensions = (!extensions && preset->customExtensions.empty()) ||
                (extensions && std::equal(extensions.begin(), extensions.end(), preset->customExtensions.begin(), preset->customExtensions.end()));
            const auto sameSchemes = (!schemes && preset->schemes.empty()) ||
                (schemes && std::equal(schemes.begin(), schemes.end(), preset->schemes.begin(), preset->schemes.end()));
            if (rule.Pattern().empty() && rule.Kind() == preset->kind && rule.FileTypeGroup() == preset->fileTypeGroup && sameExtensions && sameSchemes)
            {
                return true;
            }
        }
        return false;
    }

    // Index first, name as the check. The index is what the tooltip actually had
    // and is exact -- the control's rule list is a 1:1 mirror of this one, in the
    // same order. But the settings are editable while a tooltip is on screen, so
    // by the time the click arrives the rule at that index may be a different one,
    // or gone. The name catches that: if it disagrees, fall back to searching by
    // name, and if nothing answers to either, say so and let the caller leave the
    // user on the rules list rather than opening some unrelated rule for editing.
    //
    // A rule has nothing better to be found by. There is no id, and the name is
    // user-editable, optional and not required to be unique -- so name alone would
    // be a guess, and index alone would be a stale one.
    bool LinkTooltipViewModel::SelectRule(int32_t ruleIndex, const winrt::hstring& ruleName)
    {
        if (!_CurrentView)
        {
            return false;
        }

        if (ruleIndex >= 0 && static_cast<uint32_t>(ruleIndex) < _CurrentView.Size())
        {
            const auto candidate = _CurrentView.GetAt(static_cast<uint32_t>(ruleIndex));
            if (candidate && (ruleName.empty() || candidate.Name() == ruleName))
            {
                CurrentRule(candidate);
                return true;
            }
        }

        if (!ruleName.empty())
        {
            for (const auto& vm : _CurrentView)
            {
                if (vm && vm.Name() == ruleName)
                {
                    CurrentRule(vm);
                    return true;
                }
            }
        }

        return false;
    }

    void LinkTooltipViewModel::ExpandAllRuleGroups()
    {
        if (!_RuleGroups)
        {
            return;
        }
        for (const auto& group : _RuleGroups)
        {
            if (group)
            {
                group.IsExpanded(true);
            }
        }
    }

    void LinkTooltipViewModel::CollapseAllRuleGroups()
    {
        if (!_RuleGroups)
        {
            return;
        }
        for (const auto& group : _RuleGroups)
        {
            if (group)
            {
                group.IsExpanded(false);
            }
        }
    }

    Editor::HyperlinkTooltipRuleViewModel LinkTooltipViewModel::RequestAddRuleWithPreset(const winrt::hstring& presetId)
    {
        _ensureRules(_WindowSettings);
        if (const auto preset = FindLinkTooltipPreset(presetId))
        {
            const auto rule = CreateRuleFromPreset(*preset);
            const auto vm = make<HyperlinkTooltipRuleViewModel>(rule, _WindowSettings, _KindList, _KindMap, _FileTypeGroupList, _FileTypeGroupMap, _IntegrationChoices, _RuleActionChoices);
            CurrentView().Append(vm);
            _applyAutomaticOrder();
            return vm;
        }
        return RequestAddRule();
    }

    // The manual-ordering switch. Turning it off re-sorts at once, so the list
    // never sits in an order the setting says it does not have. This is written
    // out rather than left to PERMANENT_OBSERVABLE_PROJECTED_SETTING because of
    // that side effect.
    void LinkTooltipViewModel::HyperlinkManualRuleOrder(bool value)
    {
        if (_WindowSettings.HyperlinkManualRuleOrder() == value)
        {
            return;
        }
        _WindowSettings.HyperlinkManualRuleOrder(value);
        _applyAutomaticOrder();
        _NotifyChanges(L"HasHyperlinkManualRuleOrder", L"HyperlinkManualRuleOrder", L"IsAutomaticRuleOrder");
    }
}
