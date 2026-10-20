// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "LinkTooltipViewModel.g.h"
#include "RuleGroupViewModel.g.h"
#include "HyperlinkTooltipRuleViewModel.g.h"
#include "HyperlinkTooltipActionViewModel.g.h"
#include "IntegrationChoiceViewModel.g.h"
#include "ButtonChoiceViewModel.g.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct IntegrationChoiceViewModel : IntegrationChoiceViewModelT<IntegrationChoiceViewModel>
    {
    public:
        IntegrationChoiceViewModel(hstring id, hstring displayName) :
            _Id{ std::move(id) },
            _DisplayName{ std::move(displayName) } {}

        hstring Id() const noexcept { return _Id; }
        hstring DisplayName() const noexcept { return _DisplayName; }

    private:
        hstring _Id;
        hstring _DisplayName;
    };

    // A checkbox for one built-in card button. It owns no state of its own: the
    // two callbacks read and write whichever id list the choice belongs to, so
    // the checkboxes can never drift from the list that is actually saved.
    struct ButtonChoiceViewModel : ButtonChoiceViewModelT<ButtonChoiceViewModel>, ViewModelHelper<ButtonChoiceViewModel>
    {
    public:
        using SelectedGetter = std::function<bool(const hstring&)>;
        using SelectedSetter = std::function<void(const hstring&, bool)>;

        ButtonChoiceViewModel(hstring id, hstring label, SelectedGetter getter, SelectedSetter setter) :
            _Id{ std::move(id) },
            _Label{ std::move(label) },
            _getter{ std::move(getter) },
            _setter{ std::move(setter) } {}

        using ViewModelHelper<ButtonChoiceViewModel>::PropertyChanged;

        hstring Id() const noexcept { return _Id; }
        hstring Label() const noexcept { return _Label; }

        bool Selected() const { return _getter ? _getter(_Id) : false; }
        void Selected(bool value)
        {
            if (!_setter || Selected() == value)
            {
                return;
            }
            _setter(_Id, value);
            _NotifyChanges(L"Selected");
        }

        // Called by the owner when the whole list changed out from under the
        // checkboxes (the override toggle flipping, say). Not in the IDL --
        // only the owning view model, which holds the implementation type, calls it.
        void RaiseSelectedChanged() { _NotifyChanges(L"Selected"); }

    private:
        hstring _Id;
        hstring _Label;
        SelectedGetter _getter;
        SelectedSetter _setter;
    };

    struct HyperlinkTooltipActionViewModel : HyperlinkTooltipActionViewModelT<HyperlinkTooltipActionViewModel>, ViewModelHelper<HyperlinkTooltipActionViewModel>
    {
    public:
        HyperlinkTooltipActionViewModel(Model::HyperlinkTooltipAction action) :
            _Action{ action } {}

        // DON'T YOU DARE ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper. Do this instead:
        using ViewModelHelper<HyperlinkTooltipActionViewModel>::PropertyChanged;

        GETSET_OBSERVABLE_PROJECTED_SETTING(_Action, Name);
        GETSET_OBSERVABLE_PROJECTED_SETTING(_Action, ActionId);

        hstring Icon() const { return _Action.Icon().Path(); }
        void Icon(const hstring& value)
        {
            _Action.Icon(Model::MediaResourceHelper::FromString(value));
            _NotifyChanges(L"Icon");
        }

        Model::HyperlinkTooltipAction Action() const noexcept { return _Action; }

    private:
        Model::HyperlinkTooltipAction _Action;
    };

    struct HyperlinkTooltipRuleViewModel : HyperlinkTooltipRuleViewModelT<HyperlinkTooltipRuleViewModel>, ViewModelHelper<HyperlinkTooltipRuleViewModel>
    {
    public:
        HyperlinkTooltipRuleViewModel(Model::HyperlinkTooltipRule rule,
                                      Model::WindowSettings windowSettings,
                                      Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> kindList,
                                      Windows::Foundation::Collections::IMap<Model::HyperlinkMatchKind, Editor::EnumEntry> kindMap,
                                      Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> fileTypeGroupList,
                                      Windows::Foundation::Collections::IMap<Model::HyperlinkFileTypeGroup, Editor::EnumEntry> fileTypeGroupMap,
                                      Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> integrationChoices,
                                      Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> actionChoices);
        HyperlinkTooltipRuleViewModel(Model::HyperlinkTooltipRule rule, Model::WindowSettings windowSettings);

        using ViewModelHelper<HyperlinkTooltipRuleViewModel>::PropertyChanged;

        hstring Name() const { return _Rule.Name(); }
        void Name(const hstring& value);
        hstring DisplayName() const;

        GETSET_OBSERVABLE_PROJECTED_SETTING(_Rule, Enabled);
        GETSET_OBSERVABLE_PROJECTED_SETTING(_Rule, Integration);
        GETSET_OBSERVABLE_PROJECTED_SETTING(_Rule, ShowPreview);

    public:
        // Not the projected-setting macro, because the pattern is half of what
        // the rules list shows as a rule's summary, and now that the list is a
        // view of its own an edit here has to reach it.
        hstring Pattern() const { return _Rule.Pattern(); }
        void Pattern(const hstring& value)
        {
            if (_Rule.Pattern() != value)
            {
                _Rule.Pattern(value);
                _recomputePreview();
                _NotifyChanges(L"Pattern", L"SummaryText");
            }
        }

        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> FileTypeGroupList() const noexcept { return _FileTypeGroupList; }
        Windows::Foundation::IInspectable CurrentFileTypeGroup() const;
        void CurrentFileTypeGroup(const Windows::Foundation::IInspectable& enumEntry);

        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> KindList() const noexcept { return _KindList; }
        Windows::Foundation::IInspectable CurrentKind() const;
        void CurrentKind(const Windows::Foundation::IInspectable& enumEntry);

        bool IsLinkKind() const noexcept { return _Rule.Kind() == Model::HyperlinkMatchKind::Link; }
        bool IsTextKind() const noexcept { return _Rule.Kind() == Model::HyperlinkMatchKind::Text; }

        Windows::Foundation::Collections::IVector<Editor::IntegrationChoiceViewModel> IntegrationChoices() const noexcept { return _IntegrationChoices; }
        Windows::Foundation::IInspectable CurrentIntegrationChoice() const;
        void CurrentIntegrationChoice(const Windows::Foundation::IInspectable& value);

        // Re-announces every property the rule editor binds, so opening a rule
        // repopulates the page one property at a time instead of relying on the
        // single batched "CurrentRule changed" update. See the call site in
        // LinkTooltipViewModel::CurrentRule for why that batch cannot be trusted.
        void NotifyAllProperties();

        hstring SummaryText() const;

        // The rule editor's preview card: a line of sample text, and what this
        // rule picks out of it. Editor-only state -- nothing here is saved.
        hstring PreviewSample() const noexcept { return _PreviewSample; }
        void PreviewSample(const hstring& value);
        hstring PreviewStatus() const noexcept { return _PreviewStatus; }
        bool HasPreviewMatch() const noexcept { return _HasPreviewMatch; }
        hstring PreviewBefore() const noexcept { return _PreviewBefore; }
        hstring PreviewMatch() const noexcept { return _PreviewMatch; }
        hstring PreviewAfter() const noexcept { return _PreviewAfter; }
        hstring PreviewCaptures() const noexcept { return _PreviewCaptures; }
        bool HasPreviewCaptures() const noexcept { return !_PreviewCaptures.empty(); }

        hstring Schemes() const;
        void Schemes(const hstring& value);
        hstring CustomExtensions() const;
        void CustomExtensions(const hstring& value);

        bool OverrideShowDelay() const noexcept { return static_cast<bool>(_Rule.TooltipShowDelay()); }
        void OverrideShowDelay(bool value);
        int32_t ShowDelay() const noexcept;
        void ShowDelay(int32_t value);

        bool OverrideHideDelay() const noexcept { return static_cast<bool>(_Rule.TooltipHideDelay()); }
        void OverrideHideDelay(bool value);
        int32_t HideDelay() const noexcept;
        void HideDelay(int32_t value);

        bool OverrideMaxWidth() const noexcept { return static_cast<bool>(_Rule.TooltipMaxWidth()); }
        void OverrideMaxWidth(bool value);
        int32_t MaxWidth() const noexcept;
        void MaxWidth(int32_t value);

        // A rule with no buttons of its own inherits the global list, so an
        // empty (or absent) list is exactly what "don't override" means.
        bool OverrideButtons() const noexcept;
        void OverrideButtons(bool value);
        Windows::Foundation::Collections::IObservableVector<Editor::ButtonChoiceViewModel> ButtonChoices() const noexcept { return _ButtonChoices; }

        bool OverrideShowInPane() const noexcept { return static_cast<bool>(_Rule.ShowInPane()); }
        void OverrideShowInPane(bool value);
        bool ShowInPane() const noexcept;
        void ShowInPane(bool value);

        Windows::Foundation::Collections::IVector<Editor::IntegrationChoiceViewModel> ActionChoices() const noexcept { return _ActionChoices; }
        Windows::Foundation::IInspectable CurrentPrimaryAction() const;
        void CurrentPrimaryAction(const Windows::Foundation::IInspectable& value);
        Windows::Foundation::IInspectable CurrentAlternativeAction() const;
        void CurrentAlternativeAction(const Windows::Foundation::IInspectable& value);

        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipActionViewModel> CustomActions() const noexcept { return _CustomActions; }
        Editor::HyperlinkTooltipActionViewModel RequestAddCustomAction();
        void RequestDeleteCustomAction(const Editor::HyperlinkTooltipActionViewModel& vm);
        void ApplyPreset(const winrt::hstring& presetId);

        Model::HyperlinkTooltipRule Rule() const noexcept { return _Rule; }

    private:
        Model::HyperlinkTooltipRule _Rule;
        Model::WindowSettings _WindowSettings;
        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipActionViewModel> _CustomActions;
        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipActionViewModel>::VectorChanged_revoker _customActionsChangedRevoker;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _KindList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkMatchKind, Editor::EnumEntry> _KindMap;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _FileTypeGroupList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkFileTypeGroup, Editor::EnumEntry> _FileTypeGroupMap;
        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> _IntegrationChoices;
        Windows::Foundation::Collections::IObservableVector<Editor::ButtonChoiceViewModel> _ButtonChoices;
        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> _ActionChoices;

        // What each override reverts to while it is switched off, so unticking a
        // box and ticking it again brings the number back rather than the built-in
        // default. It used to live in the number box itself, which is precisely
        // what made an untouched rule save values nobody set -- see ShowDelay.
        int32_t _ShadowShowDelay{ 250 };
        int32_t _ShadowHideDelay{ 400 };
        int32_t _ShadowMaxWidth{ 640 };
        bool _ShadowShowInPane{ false };

        hstring _PreviewSample;
        hstring _PreviewStatus;
        hstring _PreviewBefore;
        hstring _PreviewMatch;
        hstring _PreviewAfter;
        hstring _PreviewCaptures;
        bool _HasPreviewMatch{ false };

        void _raiseButtonChoicesChanged();
        // Re-runs the pattern over the sample. Called by everything the result
        // depends on: the sample itself, the pattern, the kind and the schemes.
        void _storePreview();
        void _recomputePreview();
    };

    struct RuleGroupViewModel : RuleGroupViewModelT<RuleGroupViewModel>, ViewModelHelper<RuleGroupViewModel>
    {
    public:
        RuleGroupViewModel(winrt::hstring platformId,
                           winrt::hstring platformName,
                           winrt::hstring platformIcon,
                           std::vector<Editor::HyperlinkTooltipRuleViewModel> rules);

        using ViewModelHelper<RuleGroupViewModel>::PropertyChanged;

        winrt::hstring PlatformId() const noexcept { return _platformId; }
        winrt::hstring PlatformName() const noexcept { return _platformName; }
        winrt::hstring PlatformIcon() const noexcept { return _platformIcon; }
        winrt::hstring CountBadge() const;

        bool IsEnabled() const;
        void IsEnabled(bool value);

        bool IsExpanded() const noexcept { return _isExpanded; }
        void IsExpanded(bool value)
        {
            if (_isExpanded != value)
            {
                _isExpanded = value;
                _NotifyChanges(L"IsExpanded");
            }
        }

        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipRuleViewModel> Rules() const noexcept { return _rules; }

        void RefreshState();

    private:
        winrt::hstring _platformId;
        winrt::hstring _platformName;
        winrt::hstring _platformIcon;
        // Collapsed by default: with every platform expanded the list opens as a
        // wall of regexes you have to scroll past to reach anything. The group
        // headers already carry the name, the enabled toggle and the n/n badge,
        // which is what you actually scan for.
        bool _isExpanded{ false };
        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipRuleViewModel> _rules;
    };

    struct LinkTooltipViewModel : LinkTooltipViewModelT<LinkTooltipViewModel>, ViewModelHelper<LinkTooltipViewModel>
    {
    public:
        LinkTooltipViewModel(Model::GlobalAppSettings globalSettings, Model::WindowSettings windowSettings);

        using ViewModelHelper<LinkTooltipViewModel>::PropertyChanged;

        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, DetectURLs);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkClickable);

        // Hand-written rather than the macro: flipping this off has to re-sort
        // the rules, which a plain projected setter would not do.
        bool HyperlinkManualRuleOrder() const { return _WindowSettings.HyperlinkManualRuleOrder(); }
        void HyperlinkManualRuleOrder(bool value);
        bool HasHyperlinkManualRuleOrder() const { return _WindowSettings.HasHyperlinkManualRuleOrder(); }
        // The grouped view's Visibility, since XAML has no way to negate a binding.
        bool IsAutomaticRuleOrder() const { return !_WindowSettings.HyperlinkManualRuleOrder(); }
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipMaxWidth);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipMaxHeight);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipNested);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipShowDelay);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipHideDelay);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipActions);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipHint);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkTooltipShowRule);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkPreviewInPane);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkRemoteFilePreviews);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkIntegrationDisplayMode);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, HyperlinkActionPlacement);

        Windows::Foundation::IInspectable CurrentIntegrationDisplayMode() const;
        void CurrentIntegrationDisplayMode(const Windows::Foundation::IInspectable& enumEntry);
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> IntegrationDisplayModeList() const noexcept { return _IntegrationDisplayModeList; }

        Windows::Foundation::IInspectable CurrentActionPlacement() const;
        void CurrentActionPlacement(const Windows::Foundation::IInspectable& enumEntry);
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> ActionPlacementList() const noexcept { return _ActionPlacementList; }

        Windows::Foundation::Collections::IObservableVector<Editor::ButtonChoiceViewModel> ClickableKindChoices() const noexcept { return _ClickableKindChoices; }

        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> ClickModifierList() const noexcept { return _ClickModifierList; }
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> ClickGestureList() const noexcept { return _ClickGestureList; }
        Windows::Foundation::IInspectable CurrentPrimaryClickModifier() const;
        void CurrentPrimaryClickModifier(const Windows::Foundation::IInspectable& enumEntry);
        Windows::Foundation::IInspectable CurrentPrimaryClickGesture() const;
        void CurrentPrimaryClickGesture(const Windows::Foundation::IInspectable& enumEntry);
        Windows::Foundation::IInspectable CurrentAlternativeClickModifier() const;
        void CurrentAlternativeClickModifier(const Windows::Foundation::IInspectable& enumEntry);
        Windows::Foundation::IInspectable CurrentAlternativeClickGesture() const;
        void CurrentAlternativeClickGesture(const Windows::Foundation::IInspectable& enumEntry);

        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> ActionChoices() const noexcept { return _ActionChoices; }
        // The same list with a leading "inherit" entry, shared by every rule's two
        // action pickers. One list for all of them on purpose -- see the comment on
        // _RuleActionChoices in the constructor.
        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> RuleActionChoices() const noexcept { return _RuleActionChoices; }
        Windows::Foundation::IInspectable CurrentPrimaryAction() const;
        void CurrentPrimaryAction(const Windows::Foundation::IInspectable& value);
        Windows::Foundation::IInspectable CurrentAlternativeAction() const;
        void CurrentAlternativeAction(const Windows::Foundation::IInspectable& value);

        winrt::hstring SafeUriSchemes() const;
        void SafeUriSchemes(const winrt::hstring& value);

        Windows::Foundation::Collections::IObservableVector<Editor::ButtonChoiceViewModel> ButtonChoices() const noexcept { return _ButtonChoices; }

        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> KindList() const noexcept { return _KindList; }
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> FileTypeGroupList() const noexcept { return _FileTypeGroupList; }
        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> IntegrationChoices() const noexcept { return _IntegrationChoices; }

        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipRuleViewModel> CurrentView() const noexcept { return _CurrentView; }
        Windows::Foundation::Collections::IObservableVector<Editor::RuleGroupViewModel> RuleGroups() const noexcept { return _RuleGroups; }

        Editor::HyperlinkTooltipRuleViewModel CurrentRule() const noexcept { return _CurrentRule; }
        void CurrentRule(const Editor::HyperlinkTooltipRuleViewModel& vm);
        bool IsEditingRule() const noexcept { return static_cast<bool>(_CurrentRule); }
        bool IsNotEditingRule() const noexcept { return !_CurrentRule; }
        hstring CurrentRuleName() const { return _CurrentRule ? _CurrentRule.DisplayName() : hstring{}; }

        void RequestDeleteRule(const Editor::HyperlinkTooltipRuleViewModel& vm);
        Editor::HyperlinkTooltipRuleViewModel RequestAddRule();
        Editor::HyperlinkTooltipRuleViewModel RequestDuplicateRule(const Editor::HyperlinkTooltipRuleViewModel& source);
        Editor::HyperlinkTooltipRuleViewModel RequestAddRuleWithPreset(const winrt::hstring& presetId);
        bool IsPresetInUse(const winrt::hstring& presetId) const;
        // The same test, ignoring the rule currently open. Applying a preset to the
        // rule that already is that preset is a re-sync, not a duplicate, so the
        // Apply preset menu must not grey out the entry you are standing on.
        bool IsPresetInUseElsewhere(const winrt::hstring& presetId) const;
        bool SelectRule(int32_t ruleIndex, const winrt::hstring& ruleName);
        void ExpandAllRuleGroups();
        void CollapseAllRuleGroups();

    private:
        Model::GlobalAppSettings _GlobalSettings;
        Model::WindowSettings _WindowSettings;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _KindList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkMatchKind, Editor::EnumEntry> _KindMap;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _FileTypeGroupList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkFileTypeGroup, Editor::EnumEntry> _FileTypeGroupMap;
        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> _IntegrationChoices;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _IntegrationDisplayModeList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkIntegrationDisplayMode, Editor::EnumEntry> _IntegrationDisplayModeMap;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _ActionPlacementList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkActionPlacement, Editor::EnumEntry> _ActionPlacementMap;
        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipRuleViewModel> _CurrentView;
        Windows::Foundation::Collections::IObservableVector<Editor::RuleGroupViewModel> _RuleGroups;
        Windows::Foundation::Collections::IObservableVector<Editor::HyperlinkTooltipRuleViewModel>::VectorChanged_revoker _rulesChangedRevoker;
        Windows::Foundation::Collections::IObservableVector<Editor::ButtonChoiceViewModel> _ButtonChoices;
        Windows::Foundation::Collections::IObservableVector<Editor::ButtonChoiceViewModel> _ClickableKindChoices;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _ClickModifierList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkClickModifier, Editor::EnumEntry> _ClickModifierMap;
        Windows::Foundation::Collections::IObservableVector<Editor::EnumEntry> _ClickGestureList;
        Windows::Foundation::Collections::IMap<Model::HyperlinkClickGesture, Editor::EnumEntry> _ClickGestureMap;
        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> _ActionChoices;
        Windows::Foundation::Collections::IObservableVector<Editor::IntegrationChoiceViewModel> _RuleActionChoices;
        Editor::HyperlinkTooltipRuleViewModel _CurrentRule{ nullptr };
        // Watches the rule being edited so a rename reaches the breadcrumb.
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _currentRuleChangedRevoker;

        // Guards _applyAutomaticOrder against re-entering through the
        // VectorChanged handler its own ReplaceAll raises.
        bool _reorderingInProgress{ false };

        void _applyAutomaticOrder();
        void _updateRuleGroups();
        bool _isPresetInUse(const winrt::hstring& presetId, const Model::HyperlinkTooltipRule& except) const;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(HyperlinkTooltipActionViewModel);
    BASIC_FACTORY(HyperlinkTooltipRuleViewModel);
    BASIC_FACTORY(LinkTooltipViewModel);
    // ButtonChoiceViewModel, IntegrationChoiceViewModel and RuleGroupViewModel
    // declare no constructor in the IDL -- their owner builds them -- so none
    // has an activation factory.
}
