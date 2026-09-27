// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "IntegrationsViewModel.h"
#include "IntegrationsViewModel.g.cpp"
#include "IntegrationViewModel.g.cpp"
#include "IntegrationSettingViewModel.g.cpp"
#include "IntegrationCredentialViewModel.g.cpp"
#include "IntegrationDisplayFieldViewModel.g.cpp"
#include "IntegrationFieldGroupViewModel.g.cpp"
#include "IntegrationTabViewModel.g.cpp"
#include "IntegrationMatcherViewModel.g.cpp"

using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // Shown when a manifest gives no icon of its own. E71B is "Link", which is
    // what every integration is ultimately about.
    static constexpr std::wstring_view DefaultIntegrationGlyph{ L"\xE71B" };

    // Returns the user's stored configuration for this integration, or null when
    // they've never configured it. Never writes.
    static Model::IntegrationSettings _findEntry(const Model::GlobalAppSettings& globals, const hstring& id)
    {
        if (const auto map = globals.Integrations(); map && map.HasKey(id))
        {
            return map.Lookup(id);
        }
        return nullptr;
    }

    // Same, but creates whatever is missing so the caller can write into it.
    //
    // The subtlety worth knowing: "integrations" is an INHERITABLE_SETTING, and an
    // inheritable setting that the user has never set returns a *freshly
    // constructed* fallback on every read (see IInheritable.h). Inserting into that
    // map would therefore write into a temporary and be silently lost, so we assign
    // the map back as the user's own value before touching it.
    static Model::IntegrationSettings _ensureEntry(const Model::GlobalAppSettings& globals, const hstring& id)
    {
        auto map = globals.Integrations();
        if (!map)
        {
            map = single_threaded_map<hstring, Model::IntegrationSettings>();
        }
        globals.Integrations(map);

        if (map.HasKey(id))
        {
            return map.Lookup(id);
        }

        Model::IntegrationSettings entry{};
        entry.Values(single_threaded_map<hstring, hstring>());
        map.Insert(id, entry);
        return entry;
    }

    // Drops an entry that no longer records anything: disabled, no values, no
    // field selection. _ensureEntry creates one the moment a box is typed in, so
    // without this an integration the user tried and undid keeps a
    // { "enabled": false } stub in settings.json forever.
    static void _pruneEmptyEntry(const Model::GlobalAppSettings& globals, const hstring& id)
    {
        const auto map = globals.Integrations();
        if (!map || !map.HasKey(id))
        {
            return;
        }

        const auto entry = map.Lookup(id);
        if (!entry || entry.Enabled())
        {
            return;
        }
        if (const auto values = entry.Values(); values && values.Size() > 0)
        {
            return;
        }
        if (const auto fields = entry.Fields(); fields && fields.Size() > 0)
        {
            return;
        }
        if (const auto tabs = entry.Tabs(); tabs && tabs.Size() > 0)
        {
            return;
        }

        map.Remove(id);
        // Same inheritable-setting trap as _ensureEntry: the map has to be the
        // user's own value, or the removal happens to a temporary.
        globals.Integrations(map);
    }

    // Which group a display field belongs to. A field no group names belongs to
    // the implicit one, whose key is empty -- an id a manifest cannot collide
    // with, since a group without a key is not a group.
    static hstring _groupKeyForField(const Model::IntegrationManifest& manifest, const hstring& fieldKey)
    {
        if (const auto groups = manifest.FieldGroups())
        {
            for (const auto& group : groups)
            {
                const auto groupKey = group.Key();
                if (groupKey.empty())
                {
                    continue;
                }
                if (const auto members = group.Fields())
                {
                    for (const auto& member : members)
                    {
                        if (member == fieldKey)
                        {
                            return groupKey;
                        }
                    }
                }
            }
        }
        return {};
    }

    static hstring _groupLabel(const Model::IntegrationManifest& manifest, const hstring& groupKey)
    {
        if (groupKey.empty())
        {
            return RS_(L"Integrations_DefaultFieldGroup");
        }
        if (const auto groups = manifest.FieldGroups())
        {
            for (const auto& group : groups)
            {
                if (group.Key() == groupKey)
                {
                    const auto label = group.Label();
                    return label.empty() ? groupKey : label;
                }
            }
        }
        return groupKey;
    }

#pragma region IntegrationSettingViewModel

    IntegrationSettingViewModel::IntegrationSettingViewModel(Model::IntegrationField field,
                                                             Model::GlobalAppSettings globalSettings,
                                                             hstring integrationId) :
        _Field{ std::move(field) },
        _GlobalSettings{ std::move(globalSettings) },
        _IntegrationId{ std::move(integrationId) }
    {
    }

    hstring IntegrationSettingViewModel::Label() const
    {
        const auto label = _Field.Label();
        return label.empty() ? _Field.Key() : label;
    }

    hstring IntegrationSettingViewModel::Value() const
    {
        if (const auto entry = _findEntry(_GlobalSettings, _IntegrationId))
        {
            if (const auto values = entry.Values(); values && values.HasKey(_Field.Key()))
            {
                return values.Lookup(_Field.Key());
            }
        }
        return _Field.DefaultValue();
    }

    void IntegrationSettingViewModel::Value(const hstring& value)
    {
        if (Value() == value)
        {
            return;
        }

        const auto entry = _ensureEntry(_GlobalSettings, _IntegrationId);
        auto values = entry.Values();
        if (!values)
        {
            values = single_threaded_map<hstring, hstring>();
            entry.Values(values);
        }

        // An emptied box or reset to default removes the key rather than storing it,
        // so settings.json goes back to not mentioning it at all.
        if (value.empty() || value == _Field.DefaultValue())
        {
            if (values.HasKey(_Field.Key()))
            {
                values.Remove(_Field.Key());
            }
            // Clearing the last thing the entry recorded takes the entry with it.
            _pruneEmptyEntry(_GlobalSettings, _IntegrationId);
        }
        else
        {
            values.Insert(_Field.Key(), value);
        }

        _NotifyChanges(L"Value");
    }

#pragma endregion

#pragma region IntegrationCredentialViewModel

    IntegrationCredentialViewModel::IntegrationCredentialViewModel(Model::IntegrationField field, hstring integrationId) :
        _Field{ std::move(field) },
        _IntegrationId{ std::move(integrationId) }
    {
        _refreshStored();
    }

    hstring IntegrationCredentialViewModel::Label() const
    {
        const auto label = _Field.Label();
        return label.empty() ? _Field.Key() : label;
    }

    hstring IntegrationCredentialViewModel::StatusText() const
    {
        return _isStored ? RS_(L"Integrations_CredentialStored") : RS_(L"Integrations_CredentialNotSet");
    }

    void IntegrationCredentialViewModel::_refreshStored()
    {
        auto stored = false;
        try
        {
            stored = Model::IntegrationCredentialStore::Has(_IntegrationId, _Field.Key());
        }
        CATCH_LOG();
        _isStored = stored;
    }

    // Writes straight through to the Windows credential vault -- there is no
    // "save the settings" step for a secret, and nothing about it ever reaches
    // settings.json.
    void IntegrationCredentialViewModel::Save(const hstring& value)
    {
        if (value.empty())
        {
            return;
        }

        try
        {
            Model::IntegrationCredentialStore::Set(_IntegrationId, _Field.Key(), value);
        }
        CATCH_LOG();

        _refreshStored();
        // The secret is in the vault now, and we can never read it back; the page
        // empties the PasswordBox itself once this returns.
        PendingValue(hstring{});
        _NotifyChanges(L"IsStored", L"StatusText");
    }

    void IntegrationCredentialViewModel::Clear()
    {
        try
        {
            Model::IntegrationCredentialStore::Remove(_IntegrationId, _Field.Key());
        }
        CATCH_LOG();

        _refreshStored();
        PendingValue(hstring{});
        _NotifyChanges(L"IsStored", L"StatusText");
    }

#pragma endregion

#pragma region IntegrationDisplayFieldViewModel

    IntegrationDisplayFieldViewModel::IntegrationDisplayFieldViewModel(Model::IntegrationDisplayField field,
                                                                      Model::IntegrationManifest manifest,
                                                                      Model::GlobalAppSettings globalSettings,
                                                                      hstring integrationId) :
        _Field{ std::move(field) },
        _Manifest{ std::move(manifest) },
        _GlobalSettings{ std::move(globalSettings) },
        _IntegrationId{ std::move(integrationId) }
    {
    }

    hstring IntegrationDisplayFieldViewModel::Label() const
    {
        const auto label = _Field.Label();
        return label.empty() ? _Field.Key() : label;
    }

    // The user's own answer for this key if they have given one, and the
    // manifest's default if they have not. Nothing has to be seeded, and nothing
    // has to be kept in step with the manifest: a key nobody has decided is
    // simply absent, which is a state the old list of "keys to show" could not
    // hold.
    bool IntegrationDisplayFieldViewModel::Visible() const
    {
        if (const auto entry = _findEntry(_GlobalSettings, _IntegrationId))
        {
            if (const auto fields = entry.Fields(); fields && fields.HasKey(_Field.Key()))
            {
                return fields.Lookup(_Field.Key());
            }
        }
        return _Field.DefaultVisible();
    }

    void IntegrationDisplayFieldViewModel::Visible(bool value)
    {
        if (Visible() == value)
        {
            return;
        }

        const auto entry = _ensureEntry(_GlobalSettings, _IntegrationId);
        auto overrides = entry.Fields();
        if (!overrides)
        {
            overrides = single_threaded_map<hstring, bool>();
            entry.Fields(overrides);
        }

        // A choice that agrees with the manifest is not recorded, it is erased:
        // that keeps settings.json to the decisions the user actually made, and
        // it means a manifest that later changes its own default is followed
        // rather than overruled by a value nobody chose.
        const auto key = _Field.Key();
        if (value == _Field.DefaultVisible())
        {
            if (overrides.HasKey(key))
            {
                overrides.Remove(key);
            }
        }
        else
        {
            overrides.Insert(key, value);
        }

        _NotifyChanges(L"Visible");
    }

#pragma endregion

#pragma region IntegrationFieldGroupViewModel

    IntegrationFieldGroupViewModel::IntegrationFieldGroupViewModel(hstring key,
                                                                   hstring label,
                                                                   std::vector<Editor::IntegrationDisplayFieldViewModel> fields) :
        _Key{ std::move(key) },
        _Label{ std::move(label) },
        _Fields{ single_threaded_observable_vector<Editor::IntegrationDisplayFieldViewModel>(std::move(fields)) }
    {
        // Ticking any one box changes what the header should show, so the header
        // listens to every field it owns.
        for (const auto& field : _Fields)
        {
            _childRevokers.push_back(field.PropertyChanged(winrt::auto_revoke, [this](auto&&, const Windows::UI::Xaml::Data::PropertyChangedEventArgs& args) {
                if (!_applying && args.PropertyName() == L"Visible")
                {
                    _NotifyChanges(L"GroupChecked");
                }
            }));
        }
    }

    // null is the indeterminate state: some of the group's fields are on.
    Windows::Foundation::IReference<bool> IntegrationFieldGroupViewModel::GroupChecked() const
    {
        uint32_t visible = 0;
        uint32_t total = 0;
        for (const auto& field : _Fields)
        {
            ++total;
            if (field.Visible())
            {
                ++visible;
            }
        }

        if (total == 0 || visible == 0)
        {
            return Windows::Foundation::IReference<bool>{ false };
        }
        if (visible == total)
        {
            return Windows::Foundation::IReference<bool>{ true };
        }
        return nullptr;
    }

    void IntegrationFieldGroupViewModel::GroupChecked(const Windows::Foundation::IReference<bool>& value)
    {
        // A three-state box cycles unchecked -> checked -> indeterminate, so the
        // click after "all on" arrives here as null. Treating that as "turn the
        // group off" is what makes one click clear a full or partial group.
        const auto selectAll = value && value.Value();

        _applying = true;
        for (const auto& field : _Fields)
        {
            field.Visible(selectAll);
        }
        _applying = false;

        // Raised once for the whole bulk change, and it also snaps the checkbox
        // back off the indeterminate state the click left it in.
        _NotifyChanges(L"GroupChecked");
    }

#pragma endregion

#pragma region IntegrationTabViewModel

    IntegrationTabViewModel::IntegrationTabViewModel(Model::IntegrationTab tab,
                                                     Model::IntegrationManifest manifest,
                                                     Model::GlobalAppSettings globalSettings,
                                                     hstring integrationId) :
        _Tab{ std::move(tab) },
        _Manifest{ std::move(manifest) },
        _GlobalSettings{ std::move(globalSettings) },
        _IntegrationId{ std::move(integrationId) }
    {
    }

    hstring IntegrationTabViewModel::Label() const
    {
        const auto label = _Tab.Label();
        return label.empty() ? _Tab.Key() : label;
    }

    // Same shape as IntegrationDisplayFieldViewModel::Visible, against the
    // "tabs" list instead of the "fields" one.
    bool IntegrationTabViewModel::Visible() const
    {
        if (const auto entry = _findEntry(_GlobalSettings, _IntegrationId))
        {
            if (const auto tabs = entry.Tabs(); tabs && tabs.HasKey(_Tab.Key()))
            {
                return tabs.Lookup(_Tab.Key());
            }
        }
        return _Tab.DefaultVisible();
    }

    void IntegrationTabViewModel::Visible(bool value)
    {
        if (Visible() == value)
        {
            return;
        }

        const auto entry = _ensureEntry(_GlobalSettings, _IntegrationId);
        auto overrides = entry.Tabs();
        if (!overrides)
        {
            overrides = single_threaded_map<hstring, bool>();
            entry.Tabs(overrides);
        }

        // "Every tab off" needs no special case any more: each "off" is written
        // as its own false, so it survives a read without an empty collection
        // having to stand for it.
        const auto key = _Tab.Key();
        if (value == _Tab.DefaultVisible())
        {
            if (overrides.HasKey(key))
            {
                overrides.Remove(key);
            }
        }
        else
        {
            overrides.Insert(key, value);
        }

        _NotifyChanges(L"Visible");
    }

#pragma endregion

#pragma region IntegrationMatcherViewModel

    IntegrationMatcherViewModel::IntegrationMatcherViewModel(Model::IntegrationMatcher matcher,
                                                             Model::WindowSettings windowSettings,
                                                             hstring integrationId,
                                                             hstring integrationName) :
        _Matcher{ std::move(matcher) },
        _WindowSettings{ std::move(windowSettings) },
        _IntegrationId{ std::move(integrationId) },
        _IntegrationName{ std::move(integrationName) }
    {
    }

    hstring IntegrationMatcherViewModel::Description() const
    {
        const auto description = _Matcher.Description();
        return description.empty() ? _Matcher.Pattern() : description;
    }

    // Turns the suggestion into a real text-kind rule on the Link Tooltip page,
    // bound to this integration. Returns the rule's name so the page can say what
    // it added.
    hstring IntegrationMatcherViewModel::AddAsRule()
    {
        const auto description = Description();
        const auto name = _IntegrationName.empty() ?
                              description :
                              til::hstring_format(FMT_COMPILE(L"{}: {}"), _IntegrationName, description);

        Model::HyperlinkTooltipRule rule{};
        rule.Name(name);
        rule.Enabled(true);
        rule.Kind(Model::HyperlinkMatchKind::Text);
        rule.Pattern(_Matcher.Pattern());
        rule.Integration(_IntegrationId);
        rule.ShowPreview(true);
        rule.CustomActions(single_threaded_vector<Model::HyperlinkTooltipAction>());

        auto rules = _WindowSettings.HyperlinkTooltipRules();
        if (!rules)
        {
            rules = single_threaded_vector<Model::HyperlinkTooltipRule>();
        }
        // Same inheritable-setting trap as _ensureEntry: pin the vector as the
        // user's own value, or the rule is appended to a temporary.
        _WindowSettings.HyperlinkTooltipRules(rules);
        rules.Append(rule);

        return name;
    }

#pragma endregion

#pragma region IntegrationViewModel

    IntegrationViewModel::IntegrationViewModel(Model::IntegrationManifest manifest,
                                               Model::GlobalAppSettings globalSettings,
                                               Model::WindowSettings windowSettings) :
        _Manifest{ std::move(manifest) },
        _GlobalSettings{ std::move(globalSettings) },
        _WindowSettings{ std::move(windowSettings) }
    {
        const auto id = Id();

        std::vector<Editor::IntegrationSettingViewModel> settingVMs;
        if (const auto fields = _Manifest.Settings())
        {
            for (const auto& field : fields)
            {
                settingVMs.push_back(make<IntegrationSettingViewModel>(field, _GlobalSettings, id));
            }
        }
        _Settings = single_threaded_observable_vector<Editor::IntegrationSettingViewModel>(std::move(settingVMs));

        std::vector<Editor::IntegrationCredentialViewModel> credentialVMs;
        if (const auto fields = _Manifest.Credentials())
        {
            for (const auto& field : fields)
            {
                credentialVMs.push_back(make<IntegrationCredentialViewModel>(field, id));
            }
        }
        _Credentials = single_threaded_observable_vector<Editor::IntegrationCredentialViewModel>(std::move(credentialVMs));

        // Build the field view models and their grouping in one pass. Walking the
        // manifest's own field order (rather than its group order) is what keeps
        // an implicit group of ungrouped fields in the place the manifest author
        // clearly meant it to be, instead of always last.
        std::vector<Editor::IntegrationDisplayFieldViewModel> fieldVMs;
        std::vector<hstring> groupOrder;
        std::vector<std::vector<Editor::IntegrationDisplayFieldViewModel>> groupedFields;
        std::vector<hstring> groupLabels;

        if (const auto fields = _Manifest.Fields())
        {
            for (const auto& field : fields)
            {
                const auto fieldVM = make<IntegrationDisplayFieldViewModel>(field, _Manifest, _GlobalSettings, id);
                fieldVMs.push_back(fieldVM);

                const auto groupKey = _groupKeyForField(_Manifest, field.Key());
                const auto existing = std::find(groupOrder.begin(), groupOrder.end(), groupKey);
                if (existing == groupOrder.end())
                {
                    groupOrder.push_back(groupKey);
                    groupLabels.push_back(_groupLabel(_Manifest, groupKey));
                    groupedFields.emplace_back();
                    groupedFields.back().push_back(fieldVM);
                }
                else
                {
                    groupedFields[static_cast<size_t>(existing - groupOrder.begin())].push_back(fieldVM);
                }
            }
        }
        _Fields = single_threaded_observable_vector<Editor::IntegrationDisplayFieldViewModel>(std::move(fieldVMs));

        std::vector<Editor::IntegrationFieldGroupViewModel> groupVMs;
        for (size_t i = 0; i < groupOrder.size(); ++i)
        {
            groupVMs.push_back(make<IntegrationFieldGroupViewModel>(groupOrder[i], groupLabels[i], std::move(groupedFields[i])));
        }
        _FieldGroups = single_threaded_observable_vector<Editor::IntegrationFieldGroupViewModel>(std::move(groupVMs));

        std::vector<Editor::IntegrationTabViewModel> tabVMs;
        if (const auto tabs = _Manifest.Tabs())
        {
            for (const auto& tab : tabs)
            {
                tabVMs.push_back(make<IntegrationTabViewModel>(tab, _Manifest, _GlobalSettings, id));
            }
        }
        _Tabs = single_threaded_observable_vector<Editor::IntegrationTabViewModel>(std::move(tabVMs));

        // Only text matchers the manifest flags as "suggested" are offered here --
        // link matchers need no rule of their own, they run against hovered links.
        std::vector<Editor::IntegrationMatcherViewModel> matcherVMs;
        if (const auto matchers = _Manifest.Matchers())
        {
            for (const auto& matcher : matchers)
            {
                if (matcher.Kind() == Model::IntegrationMatcherKind::Text && matcher.Suggested() && !matcher.Pattern().empty())
                {
                    matcherVMs.push_back(make<IntegrationMatcherViewModel>(matcher, _WindowSettings, id, Name()));
                }
            }
        }
        _SuggestedMatchers = single_threaded_observable_vector<Editor::IntegrationMatcherViewModel>(std::move(matcherVMs));

        // Typing into a setting or storing a credential is what flips
        // "not configured" off, so listen to every child that can do it.
        for (const auto& vm : _Settings)
        {
            _childRevokers.push_back(vm.PropertyChanged(winrt::auto_revoke, [this](auto&&, auto&&) { _recomputeConfigured(); }));
        }
        for (const auto& vm : _Credentials)
        {
            _childRevokers.push_back(vm.PropertyChanged(winrt::auto_revoke, [this](auto&&, auto&&) { _recomputeConfigured(); }));
        }

        _recomputeConfigured();
    }

    hstring IntegrationViewModel::Name() const
    {
        const auto name = _Manifest.Name();
        return name.empty() ? _Manifest.Id() : name;
    }

    hstring IntegrationViewModel::Icon() const
    {
        const auto icon = _Manifest.Icon();
        return icon.empty() ? hstring{ DefaultIntegrationGlyph } : icon;
    }

    hstring IntegrationViewModel::Trait() const
    {
        return IsBuiltIn() ? RS_(L"Integrations_BuiltInTrait") : hstring{};
    }

    hstring IntegrationViewModel::SourceDescription() const
    {
        return IsBuiltIn() ? hstring{} : Source();
    }

    hstring IntegrationViewModel::AccessibleName() const
    {
        const auto source = Source();
        if (source.empty())
        {
            return Name();
        }
        return til::hstring_format(FMT_COMPILE(L"{}, {}"), Name(), source);
    }

    bool IntegrationViewModel::Enabled() const
    {
        if (const auto entry = _findEntry(_GlobalSettings, Id()))
        {
            return entry.Enabled();
        }
        return false;
    }

    void IntegrationViewModel::Enabled(bool value)
    {
        if (Enabled() == value)
        {
            return;
        }
        _ensureEntry(_GlobalSettings, Id()).Enabled(value);
        if (!value)
        {
            // Toggling an otherwise unconfigured integration back off removes the
            // entry that toggling it on created.
            _pruneEmptyEntry(_GlobalSettings, Id());
        }
        _NotifyChanges(L"Enabled");
    }

    void IntegrationViewModel::_recomputeConfigured()
    {
        auto configured = true;

        for (const auto& setting : _Settings)
        {
            if (setting.Required() && setting.Value().empty())
            {
                configured = false;
                break;
            }
        }

        if (configured)
        {
            for (const auto& credential : _Credentials)
            {
                if (credential.Required() && !credential.IsStored())
                {
                    configured = false;
                    break;
                }
            }
        }

        if (configured != _isConfigured)
        {
            _isConfigured = configured;
            _NotifyChanges(L"IsConfigured", L"IsNotConfigured");
        }
    }

#pragma endregion

#pragma region IntegrationsViewModel

    IntegrationsViewModel::IntegrationsViewModel(Model::GlobalAppSettings globalSettings, Model::WindowSettings windowSettings) :
        _GlobalSettings{ std::move(globalSettings) },
        _WindowSettings{ std::move(windowSettings) }
    {
        std::vector<Editor::IntegrationViewModel> integrationVMs;
        if (const auto manifests = Model::IntegrationRegistry::All())
        {
            for (const auto& manifest : manifests)
            {
                integrationVMs.push_back(make<IntegrationViewModel>(manifest, _GlobalSettings, _WindowSettings));
            }
        }
        _Integrations = single_threaded_observable_vector<Editor::IntegrationViewModel>(std::move(integrationVMs));
    }

    hstring IntegrationsViewModel::UserDirectory() const
    {
        try
        {
            return Model::IntegrationRegistry::UserDirectory();
        }
        CATCH_LOG();
        return {};
    }

    void IntegrationsViewModel::RequestNavigateToLinkTooltip()
    {
        NavigateToLinkTooltipRequested.raise(*this, nullptr);
    }

#pragma endregion
}
