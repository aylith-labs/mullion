// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <til/static_map.h>

// Navigation tags used to identify pages in the Settings UI NavigationView.
// These tags are stored as the Tag property on NavigationViewItems.
namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    inline constexpr std::wstring_view openJsonTag{ L"OpenJson_Nav" };
    // Not a page: a footer switch that decides whether rows print their
    // descriptions. It carries a tag only so ItemInvoked can recognise it and
    // decline to navigate anywhere.
    inline constexpr std::wstring_view showDescriptionsTag{ L"ShowDescriptions_Nav" };
    inline constexpr std::wstring_view launchTag{ L"Launch_Nav" };
    inline constexpr std::wstring_view sessionRestoreTag{ L"SessionRestore_Nav" };
    inline constexpr std::wstring_view interactionTag{ L"Interaction_Nav" };
    inline constexpr std::wstring_view linkTooltipTag{ L"LinkTooltip_Nav" };
    inline constexpr std::wstring_view integrationsTag{ L"Integrations_Nav" };
    inline constexpr std::wstring_view compatibilityTag{ L"Compatibility_Nav" };
    inline constexpr std::wstring_view activityTag{ L"Activity_Nav" };
    inline constexpr std::wstring_view actionsTag{ L"Actions_Nav" };
    inline constexpr std::wstring_view newTabMenuTag{ L"NewTabMenu_Nav" };
    inline constexpr std::wstring_view extensionsTag{ L"Extensions_Nav" };
    inline constexpr std::wstring_view profilesTag{ L"Profiles_Nav" };
    inline constexpr std::wstring_view globalProfileTag{ L"GlobalProfile_Nav" };
    inline constexpr std::wstring_view colorSchemesTag{ L"ColorSchemes_Nav" };
    inline constexpr std::wstring_view globalAppearanceTag{ L"GlobalAppearance_Nav" };
    inline constexpr std::wstring_view documentationTag{ L"Documentation_Nav" };

    // Map from navigation tags to Segoe MDL2 Assets icon glyphs
    inline constexpr til::static_map NavTagIconMap{
        std::pair{ launchTag, L"\xE7B5" }, /* Set Lock Screen */
        // Mandatory, not decorative: MainPage's nav-item loop looks every tag up
        // in this map without guarding the lookup, so a tag missing from here
        // fails at construction rather than merely rendering no icon.
        std::pair{ sessionRestoreTag, L"\xE777" }, /* Update Restore */
        std::pair{ interactionTag, L"\xE7C9" }, /* Touch Pointer */
        std::pair{ linkTooltipTag, L"\xE71B" }, /* Link */
        std::pair{ integrationsTag, L"\xE774" }, /* Globe */
        std::pair{ globalAppearanceTag, L"\xE771" }, /* Personalize */
        std::pair{ colorSchemesTag, L"\xE790" }, /* Color */
        std::pair{ compatibilityTag, L"\xEC7A" }, /* Developer Tools */
        std::pair{ activityTag, L"\xE81C" }, /* History */
        std::pair{ actionsTag, L"\xE765" }, /* Keyboard Classic */
        std::pair{ newTabMenuTag, L"\xE71D" }, /* All Apps */
        std::pair{ extensionsTag, L"\xEA86" }, /* Puzzle */
        std::pair{ profilesTag, L"\xE7EE" }, /* Other User */
        std::pair{ globalProfileTag, L"\xE7EE" }, /* Other User */
        std::pair{ openJsonTag, L"\xE713" }, /* Settings */
        std::pair{ showDescriptionsTag, L"\xE7B3" }, /* Reading Mode */
        std::pair{ documentationTag, L"\xE897" }, /* Help */
    };
}
