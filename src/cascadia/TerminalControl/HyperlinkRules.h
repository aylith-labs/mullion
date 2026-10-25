// Shared rule evaluation for buffer links and embedded previews.
#pragma once
#include <algorithm>
#include <string>
#include <vector>
#include <til/regex.h>
#include "HyperlinkFileTypeGroups.h"
#include "../inc/LintelFileTypes.g.h"
namespace winrt::Microsoft::Terminal::Control::implementation
{
    namespace Control = winrt::Microsoft::Terminal::Control;
        struct EffectiveHyperlinkTooltipSettings
        {
            int32_t showDelay{ 0 };
            int32_t hideDelay{ 0 };
            int32_t maxWidth{ 0 };
            // Which built-in buttons this particular link gets. They come from a
            // list of ids -- "open", "copyLink", "copyPath", "reveal",
            // "showInPane" -- taken from the matched rule when it names any and
            // from hyperlink.tooltipButtons when it doesn't, so a rule replaces
            // the global choice rather than subtracting from it.
            bool showOpen{ false };
            bool showCopyLink{ false };
            bool showCopyPath{ false };
            bool showReveal{ false };
            bool showInPane{ false };
            // hyperlink.tooltipActions. Off means no buttons at all, so the ones
            // turned off above do not resurface in the card's overflow menu either.
            bool actionsEnabled{ true };
            // The pane, not the card, is where this link's preview belongs.
            bool preferPane{ false };
            // The "Ctrl+Click to follow link" line.
            bool showHint{ true };
            std::vector<Control::HyperlinkTooltipAction> customActions;
            // Preview: which integration ("" = automatic, "none" = off) and whether to
            // show one at all, from the matched rule.
            winrt::hstring integration;
            bool showPreview{ true };
            // The hovered text came from a text-kind rule's pattern, not from a URI.
            bool isTextMatch{ false };
            // The action id each click chord runs for this link: from the matched
            // rule when it names one, otherwise the global hyperlink.primaryAction /
            // hyperlink.alternativeAction. "none" means that chord does nothing here.
            winrt::hstring primaryAction;
            winrt::hstring alternativeAction;
            Control::HyperlinkIntegrationDisplayMode integrationDisplayMode{ Control::HyperlinkIntegrationDisplayMode::Above };
            Control::HyperlinkActionPlacement actionPlacement{ Control::HyperlinkActionPlacement::FarFromLink };
            // Whether to name the rule that decided all of the above on the card.
            bool showRule{ false };
            // Which rule that was. The index, because a rule has no id of its own
            // and its name is user-editable, may be empty and need not be unique --
            // and because this list is a faithful 1:1 mirror of the model's, so the
            // index is exact. The name comes along to display, and to check against
            // before the settings page opens whatever is at that index now.
            // -1 means no rule matched, which is worth saying out loud: it is the
            // answer to "why is this link not being previewed the way I asked".
            int32_t ruleIndex{ -1 };
            winrt::hstring ruleName;
        };
    inline void ApplyHyperlinkButtonList(EffectiveHyperlinkTooltipSettings& effective,
                                                const Windows::Foundation::Collections::IVector<winrt::hstring>& buttons)
    {
        effective.showOpen = false;
        effective.showCopyLink = false;
        effective.showCopyPath = false;
        effective.showReveal = false;
        effective.showInPane = false;

        for (const auto& button : buttons)
        {
            const std::wstring_view id{ button };
            if (til::equals_insensitive_ascii(id, L"open"))
            {
                effective.showOpen = true;
            }
            else if (til::equals_insensitive_ascii(id, L"copyLink"))
            {
                effective.showCopyLink = true;
            }
            else if (til::equals_insensitive_ascii(id, L"copyPath"))
            {
                effective.showCopyPath = true;
            }
            else if (til::equals_insensitive_ascii(id, L"reveal"))
            {
                effective.showReveal = true;
            }
            else if (til::equals_insensitive_ascii(id, L"showInPane"))
            {
                effective.showInPane = true;
            }
        }
    }

    // Matches one user-authored rule pattern against `text`.
    //
    // This uses ICU, not std::wregex, and that is the whole point. Rule patterns are
    // written against ICU's syntax because that is what compiles them everywhere else
    // that matters: Terminal::_getPatterns() scans the buffer with ICU, and
    // HyperlinkPreviewService reads their named captures back with
    // uregex_groupNumberFromName(). std::regex only implements the ECMAScript grammar
    // from ECMA-262 3rd edition, which has no named groups at all -- so `(?<key>...)`
    // threw std::regex_error here and the catch below quietly turned it into "no
    // match". Every shipped preset (Jira, GitHub, Slack, Stith) uses named groups, so
    // all of them were detected and highlighted by the scanner and then silently
    // refused by this function: no card, no preview, no per-rule action.
    inline bool RuleTextMatches(const std::wstring_view pattern, const std::wstring_view text, const bool requireFullMatch) noexcept
    {
        if (pattern.empty() || text.empty())
        {
            return false;
        }

        UErrorCode status = U_ZERO_ERROR;
        const auto re = til::ICU::CreateRegex(pattern, UREGEX_CASE_INSENSITIVE, &status);
        if (U_FAILURE(status) || !re)
        {
            // An uncompilable pattern never matches, rather than crashing or matching
            // everything. CreateRegex also caps the match time and stack, so a
            // pathological pattern cannot hang the hover either.
            return false;
        }

#pragma warning(suppress : 26490) // Don't use reinterpret_cast (type.1).
        uregex_setText(re.get(), reinterpret_cast<const UChar*>(text.data()), gsl::narrow_cast<int32_t>(text.size()), &status);
        if (U_FAILURE(status))
        {
            return false;
        }

        const auto matched = requireFullMatch ? uregex_matches(re.get(), 0, &status) : uregex_find(re.get(), 0, &status);
        return U_SUCCESS(status) && matched;
    }

    // Walks HyperlinkTooltipRules in order and returns the show/hide delay, max width,
    // built-in button visibility and custom action list that should actually be used for
    // the given hovered link -- the global settings, overridden by the first enabled rule
    // (if any) whose match criteria are all satisfied. isFileLink should be the same
    // file-vs-not-file test _resolvedHyperlinkTarget() already performs, since a file-type
    // criterion can only ever be satisfied by a link that resolves to a path.
    inline EffectiveHyperlinkTooltipSettings ResolveHyperlinkRules(const Control::IControlSettings& settings, std::wstring_view uri, bool isFileLink)
    {
        const auto actionsEnabled = settings.HyperlinkTooltipActions();
        EffectiveHyperlinkTooltipSettings effective{
            .showDelay = std::max(0, settings.HyperlinkTooltipShowDelay()),
            .hideDelay = std::max(0, settings.HyperlinkTooltipHideDelay()),
            .maxWidth = settings.HyperlinkTooltipMaxWidth(),
            .actionsEnabled = actionsEnabled,
            .preferPane = settings.HyperlinkPreviewInPane(),
            // A "click to follow link" line would be a lie when clicking is off,
            // so the master switch suppresses it regardless of the hint setting.
            .showHint = settings.HyperlinkTooltipHint() && settings.HyperlinkClickable(),
            .primaryAction = settings.HyperlinkPrimaryAction(),
            .alternativeAction = settings.HyperlinkAlternativeAction(),
            .integrationDisplayMode = settings.HyperlinkIntegrationDisplayMode(),
            .actionPlacement = settings.HyperlinkActionPlacement(),
            .showRule = settings.HyperlinkTooltipShowRule(),
        };

        // The global choice, which a matching rule may replace wholesale below. An
        // unset list is the shipped default rather than "show nothing": a control
        // whose settings never reached the adapter would otherwise lose every button.
        if (actionsEnabled)
        {
            if (const auto globalButtons = settings.HyperlinkTooltipButtons(); globalButtons && globalButtons.Size() > 0)
            {
                ApplyHyperlinkButtonList(effective, globalButtons);
            }
            else
            {
                effective.showCopyLink = true;
                effective.showInPane = true;
            }
        }

        const auto rules = settings.HyperlinkTooltipRules();
        if (!rules || uri.empty())
        {
            return effective;
        }

        // Bare POSIX paths (see _hoveredHyperlinkChanged) have no scheme of their own;
        // treat them as "file" so a rule can still target them by scheme.
        std::wstring scheme;
        if (isFileLink || uri.front() == L'/')
        {
            scheme = L"file";
        }
        else
        {
            try
            {
                scheme = Windows::Foundation::Uri{ winrt::hstring{ uri } }.SchemeName().c_str();
            }
            catch (...)
            {
            }
        }

        // A source location such as file:///Program.cs#L194 still has extension cs.
        const auto extension = isFileLink ? Lintel::ExtensionOf(uri) : std::wstring{};

        // Counted rather than taken from the iterator, because the index is how the
        // settings page is later told which rule this was: the list here is a faithful
        // 1:1 mirror of hyperlink.tooltipRules, and a rule has nothing else to identify
        // it by -- no id, and a name that is user-editable, optional and not unique.
        int32_t ruleIndex = -1;
        for (const auto& rule : rules)
        {
            ++ruleIndex;
            if (!rule || !rule.Enabled())
            {
                continue;
            }

            // A text-kind rule is not about links at all: its pattern is what the buffer
            // scanner used to find this run of plain text in the first place, so the rule
            // applies exactly when that pattern accounts for the whole of it. Anything less
            // than a full match would attach the rule to text it never selected. A text rule
            // with no pattern has nothing to match against and can never apply.
            const auto isTextRule = rule.Kind() == Control::HyperlinkMatchKind::Text;
            if (isTextRule)
            {
                const auto pattern = rule.Pattern();
                if (pattern.empty())
                {
                    continue;
                }

                if (!RuleTextMatches(pattern, uri, true))
                {
                    continue;
                }
            }

            // The scheme, pattern and file-type criteria below all describe a URI, so a text
            // rule skips them: the hovered run has no scheme, its pattern was already applied
            // in full above, and "the extension of a Jira issue key" means nothing.
            if (const auto schemes = rule.Schemes(); !isTextRule && schemes && schemes.Size() > 0)
            {
                const auto found = std::any_of(begin(schemes), end(schemes), [&](const auto& s) {
                    return til::equals_insensitive_ascii(std::wstring_view{ scheme }, std::wstring_view{ s });
                });
                if (!found)
                {
                    continue;
                }
            }

            if (const auto pattern = rule.Pattern(); !isTextRule && !pattern.empty())
            {
                if (!RuleTextMatches(pattern, uri, false))
                {
                    continue;
                }
            }

            const auto group = rule.FileTypeGroup();
            const auto customExtensions = rule.CustomExtensions();
            const auto hasExtensionCriteria = group != Control::HyperlinkFileTypeGroup::None || (customExtensions && customExtensions.Size() > 0);
            if (!isTextRule && hasExtensionCriteria)
            {
                if (!isFileLink)
                {
                    continue;
                }

                auto matches = HyperlinkFileTypeGroups::PathInGroup(group, uri);
                if (!matches && !extension.empty() && customExtensions)
                {
                    matches = std::any_of(begin(customExtensions), end(customExtensions), [&](const auto& e) {
                        return til::equals_insensitive_ascii(std::wstring_view{ extension }, std::wstring_view{ e });
                    });
                }
                if (!matches)
                {
                    continue;
                }
            }

            // All configured criteria matched (or none were configured, which is also a match).
            if (const auto showDelay = rule.TooltipShowDelay())
            {
                effective.showDelay = std::max(0, showDelay.Value());
            }
            if (const auto hideDelay = rule.TooltipHideDelay())
            {
                effective.hideDelay = std::max(0, hideDelay.Value());
            }
            if (const auto maxWidth = rule.TooltipMaxWidth())
            {
                effective.maxWidth = maxWidth.Value();
            }
            // A rule's own button list replaces the global one outright; an empty
            // list means "inherit", which is why it is tested before being applied.
            if (actionsEnabled)
            {
                if (const auto ruleButtons = rule.Buttons(); ruleButtons && ruleButtons.Size() > 0)
                {
                    ApplyHyperlinkButtonList(effective, ruleButtons);
                }
            }
            if (const auto showInPane = rule.ShowInPane())
            {
                effective.preferPane = showInPane.Value();
            }

            // An empty action id inherits the global chord action; "none" is a
            // deliberate "this rule has no such click" and is kept as-is so the
            // dispatcher can tell it apart from inheriting.
            if (const auto primary = rule.PrimaryAction(); !primary.empty())
            {
                effective.primaryAction = primary;
            }
            if (const auto alternative = rule.AlternativeAction(); !alternative.empty())
            {
                effective.alternativeAction = alternative;
            }

            if (actionsEnabled)
            {
                if (const auto actions = rule.CustomActions())
                {
                    effective.customActions.assign(begin(actions), end(actions));
                }
            }

            // Whether the hovered run is plain text rather than a URI is decided here and
            // nowhere else, because only the rule that matched knows it. Everything
            // downstream -- the punycode annotation, the target line, the two buttons that
            // need a link -- keys off this.
            effective.isTextMatch = isTextRule;
            effective.integration = rule.Integration();
            effective.showPreview = rule.ShowPreview();
            effective.ruleIndex = ruleIndex;
            effective.ruleName = rule.Name();

            break;
        }

        return effective;
    }

}
