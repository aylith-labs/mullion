/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- IntegrationManifest.h

Abstract:
- A parsed integration.json (see Integrations.idl): the declarative description
  of one link-preview plugin, plus the small objects it is made of.

--*/
#pragma once

#include "IntegrationField.g.h"
#include "IntegrationMatcher.g.h"
#include "IntegrationFetchStep.g.h"
#include "IntegrationDisplayField.g.h"
#include "IntegrationFieldGroup.g.h"
#include "IntegrationTab.g.h"
#include "IntegrationAction.g.h"
#include "IntegrationManifest.g.h"
#include "JsonUtils.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    // The comma in IMap<K, V> would split WINRT_PROPERTY's macro arguments.
    using IntegrationHeaderMap = Windows::Foundation::Collections::IMap<hstring, hstring>;

    struct IntegrationField : IntegrationFieldT<IntegrationField>
    {
        IntegrationField() noexcept = default;
        static com_ptr<IntegrationField> FromJson(const Json::Value& json);

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(hstring, Placeholder);
        WINRT_PROPERTY(hstring, Editor);
        WINRT_PROPERTY(hstring, Description);
        WINRT_PROPERTY(bool, Required, false);
        WINRT_PROPERTY(bool, Secret, false);
        WINRT_PROPERTY(hstring, DefaultValue);
    };

    struct IntegrationMatcher : IntegrationMatcherT<IntegrationMatcher>
    {
        IntegrationMatcher() noexcept = default;
        static com_ptr<IntegrationMatcher> FromJson(const Json::Value& json);

        WINRT_PROPERTY(Model::IntegrationMatcherKind, Kind, Model::IntegrationMatcherKind::Link);
        WINRT_PROPERTY(hstring, Pattern);
        WINRT_PROPERTY(hstring, HostSetting);
        WINRT_PROPERTY(hstring, LinkTemplate);
        WINRT_PROPERTY(hstring, OpenAction);
        WINRT_PROPERTY(bool, Suggested, false);
        WINRT_PROPERTY(hstring, Description);
    };

    struct IntegrationFetchStep : IntegrationFetchStepT<IntegrationFetchStep>
    {
        IntegrationFetchStep() noexcept = default;
        static com_ptr<IntegrationFetchStep> FromJson(const Json::Value& json);

        WINRT_PROPERTY(hstring, Id);
        WINRT_PROPERTY(Model::IntegrationFetchType, Type, Model::IntegrationFetchType::Http);
        WINRT_PROPERTY(hstring, Url);
        WINRT_PROPERTY(hstring, Method);
        WINRT_PROPERTY(IntegrationHeaderMap, Headers);
        WINRT_PROPERTY(hstring, AuthType);
        WINRT_PROPERTY(hstring, AuthUser);
        WINRT_PROPERTY(hstring, AuthPassword);
        WINRT_PROPERTY(hstring, Body);
        WINRT_PROPERTY(bool, AllowUntrustedCertificate, false);
        WINRT_PROPERTY(hstring, CommandLine);
        WINRT_PROPERTY(hstring, Stdin);
        WINRT_PROPERTY(int32_t, TimeoutMs, 8000);
        WINRT_PROPERTY(hstring, When);
        WINRT_PROPERTY(hstring, Unless);
        WINRT_PROPERTY(bool, Optional, false);
        WINRT_PROPERTY(bool, Deferred, false);
    };

    struct IntegrationDisplayField : IntegrationDisplayFieldT<IntegrationDisplayField>
    {
        IntegrationDisplayField() noexcept = default;
        static com_ptr<IntegrationDisplayField> FromJson(const Json::Value& json);

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(hstring, Path);
        WINRT_PROPERTY(Model::IntegrationFieldKind, Kind, Model::IntegrationFieldKind::Text);
        WINRT_PROPERTY(hstring, IconPath);
        WINRT_PROPERTY(hstring, EachPath);
        WINRT_PROPERTY(hstring, Placement);
        WINRT_PROPERTY(hstring, LinkTemplate);
        WINRT_PROPERTY(hstring, LinkPath);
        WINRT_PROPERTY(hstring, ColorPath);
        WINRT_PROPERTY(hstring, Color);
        WINRT_PROPERTY(hstring, Format);
        WINRT_PROPERTY(bool, DefaultVisible, false);
    };

    struct IntegrationFieldGroup : IntegrationFieldGroupT<IntegrationFieldGroup>
    {
        IntegrationFieldGroup() noexcept = default;
        static com_ptr<IntegrationFieldGroup> FromJson(const Json::Value& json);

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<hstring>, Fields);
    };

    struct IntegrationTab : IntegrationTabT<IntegrationTab>
    {
        IntegrationTab() noexcept = default;
        static com_ptr<IntegrationTab> FromJson(const Json::Value& json);

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(Model::IntegrationTabKind, Kind, Model::IntegrationTabKind::Body);
        WINRT_PROPERTY(hstring, Path);
        WINRT_PROPERTY(hstring, Format);
        WINRT_PROPERTY(hstring, ItemAuthorPath);
        WINRT_PROPERTY(hstring, ItemAvatarPath);
        WINRT_PROPERTY(hstring, ItemBodyPath);
        WINRT_PROPERTY(hstring, ItemTimePath);
        WINRT_PROPERTY(hstring, StepId);
        WINRT_PROPERTY(bool, DefaultVisible, false);
    };

    struct IntegrationAction : IntegrationActionT<IntegrationAction>
    {
        IntegrationAction() noexcept = default;
        static com_ptr<IntegrationAction> FromJson(const Json::Value& json);

        WINRT_PROPERTY(hstring, Key);
        WINRT_PROPERTY(hstring, Label);
        WINRT_PROPERTY(Model::IntegrationActionKind, Kind, Model::IntegrationActionKind::Button);

        WINRT_PROPERTY(hstring, OptionsPath);
        WINRT_PROPERTY(hstring, OptionIdPath);
        WINRT_PROPERTY(hstring, OptionLabelPath);
        WINRT_PROPERTY(hstring, OptionBadgePath);
        WINRT_PROPERTY(hstring, OptionColorPath);
        WINRT_PROPERTY(hstring, OptionTargetIdPath);
        WINRT_PROPERTY(hstring, CurrentStatePath);
        WINRT_PROPERTY(hstring, OptionFieldsPath);

        WINRT_PROPERTY(hstring, Method);
        WINRT_PROPERTY(hstring, Url);
        WINRT_PROPERTY(hstring, Body);
        WINRT_PROPERTY(IntegrationHeaderMap, Headers);
        WINRT_PROPERTY(hstring, AuthType);
        WINRT_PROPERTY(hstring, AuthUser);
        WINRT_PROPERTY(hstring, AuthPassword);
        WINRT_PROPERTY(bool, AllowUntrustedCertificate, false);
        WINRT_PROPERTY(int32_t, TimeoutMs, 8000);
    };

    struct IntegrationManifest : IntegrationManifestT<IntegrationManifest>
    {
        IntegrationManifest() noexcept = default;

        // Returns null when the JSON is not a usable manifest (no id or name).
        static com_ptr<IntegrationManifest> FromJson(const Json::Value& json, const hstring& source, bool isBuiltIn);
        static com_ptr<IntegrationManifest> FromString(std::string_view text, const hstring& source, bool isBuiltIn);

        WINRT_PROPERTY(hstring, Id);
        WINRT_PROPERTY(hstring, Name);
        WINRT_PROPERTY(hstring, Icon);
        WINRT_PROPERTY(int32_t, Version, 1);
        WINRT_PROPERTY(hstring, Source);
        WINRT_PROPERTY(bool, IsBuiltIn, false);
        WINRT_PROPERTY(int32_t, CacheSeconds, 300);
        WINRT_PROPERTY(hstring, Html);
        WINRT_PROPERTY(hstring, Embed);
        WINRT_PROPERTY(hstring, AccountProvider);

        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationField>, Settings);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationField>, Credentials);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationMatcher>, Matchers);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationFetchStep>, FetchSteps);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationDisplayField>, Fields);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationFieldGroup>, FieldGroups);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationTab>, Tabs);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<Model::IntegrationAction>, Actions);
        WINRT_PROPERTY(Windows::Foundation::Collections::IVector<hstring>, DetectPatterns);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(IntegrationField);
    BASIC_FACTORY(IntegrationMatcher);
    BASIC_FACTORY(IntegrationFetchStep);
    BASIC_FACTORY(IntegrationDisplayField);
    BASIC_FACTORY(IntegrationFieldGroup);
    BASIC_FACTORY(IntegrationTab);
    BASIC_FACTORY(IntegrationAction);
    BASIC_FACTORY(IntegrationManifest);
}

// Manifests are read-only for the settings model: they are parsed, never
// written back. ToJson exists only so the generic container traits compile.
#define INTEGRATION_READONLY_CONVERSION_TRAIT(type)                                          \
    template<>                                                                               \
    struct ConversionTrait<type>                                                             \
    {                                                                                        \
        type FromJson(const Json::Value& json)                                               \
        {                                                                                    \
            return *implementation::type::FromJson(json);                                    \
        }                                                                                    \
        bool CanConvert(const Json::Value& json) const                                       \
        {                                                                                    \
            return json.isObject();                                                          \
        }                                                                                    \
        Json::Value ToJson(const type&)                                                      \
        {                                                                                    \
            return Json::Value::null;                                                        \
        }                                                                                    \
        std::string TypeDescription() const                                                  \
        {                                                                                    \
            return #type;                                                                    \
        }                                                                                    \
    };

namespace Microsoft::Terminal::Settings::Model::JsonUtils
{
    using namespace winrt::Microsoft::Terminal::Settings::Model;

    INTEGRATION_READONLY_CONVERSION_TRAIT(IntegrationField)
    INTEGRATION_READONLY_CONVERSION_TRAIT(IntegrationMatcher)
    INTEGRATION_READONLY_CONVERSION_TRAIT(IntegrationFetchStep)
    INTEGRATION_READONLY_CONVERSION_TRAIT(IntegrationDisplayField)
    INTEGRATION_READONLY_CONVERSION_TRAIT(IntegrationFieldGroup)
    INTEGRATION_READONLY_CONVERSION_TRAIT(IntegrationTab)
    INTEGRATION_READONLY_CONVERSION_TRAIT(IntegrationAction)
}

#undef INTEGRATION_READONLY_CONVERSION_TRAIT
