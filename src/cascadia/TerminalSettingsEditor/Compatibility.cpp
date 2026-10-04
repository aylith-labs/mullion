// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Compatibility.h"
#include "EnumEntry.h"
#include "SettingsCard.h"
#include "Compatibility.g.cpp"
#include "CompatibilityViewModel.g.cpp"

using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    CompatibilityViewModel::CompatibilityViewModel(Model::CascadiaSettings settings) :
        _settings{ settings }
    {
        INITIALIZE_BINDABLE_ENUM_SETTING(TextMeasurement, TextMeasurement, winrt::Microsoft::Terminal::Control::TextMeasurement, L"Globals_TextMeasurement_", L"Text");
        INITIALIZE_BINDABLE_ENUM_SETTING(AmbiguousWidth, AmbiguousWidth, winrt::Microsoft::Terminal::Control::AmbiguousWidth, L"Globals_AmbiguousWidth_", L"Text");
        INITIALIZE_BINDABLE_ENUM_SETTING(GraphicsAPI, GraphicsAPI, winrt::Microsoft::Terminal::Control::GraphicsAPI, L"Globals_GraphicsAPI_", L"Text");
    }

    bool CompatibilityViewModel::DebugFeaturesAvailable() const noexcept
    {
        return Feature_DebugModeUI::IsEnabled();
    }

    // The setting lives on the settings clone and is only written on Save, but the
    // mark is a property of the editor's own chrome -- it should follow the switch
    // immediately, on every page, including this one. So the value is pushed
    // straight to SettingsCard rather than waiting to be reloaded.
    void CompatibilityViewModel::AylithImprintToggled(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        // Read the switch, not AylithImprint(). Toggled and the TwoWay binding's
        // write-back are not ordered against each other, so the projected property
        // can still be reporting the old value here -- which meant this pushed
        // "false" on the way on, and the mark never appeared anywhere.
        if (const auto toggle = sender.try_as<winrt::Windows::UI::Xaml::Controls::ToggleSwitch>())
        {
            SettingsCard::ImprintEnabled(toggle.IsOn());
        }
    }

    // Same reasoning as AylithImprintToggled, including reading the switch rather
    // than the projected property: Toggled and the TwoWay binding's write-back are
    // not ordered against each other.
    void CompatibilityViewModel::AylithImprintJsonOnlyToggled(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& /* args */)
    {
        if (const auto toggle = sender.try_as<winrt::Windows::UI::Xaml::Controls::ToggleSwitch>())
        {
            SettingsCard::JsonOnlyImprintEnabled(toggle.IsOn());
        }
    }

    void CompatibilityViewModel::ResetApplicationState()
    {
        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "ResetApplicationState",
            TraceLoggingDescription("Event emitted when the user resets their application state"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        _settings.ResetApplicationState();
    }

    void CompatibilityViewModel::ResetToDefaultSettings()
    {
        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "ResetToDefaultSettings",
            TraceLoggingDescription("Event emitted when the user resets their settings to their default value"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        _settings.ResetToDefaultSettings();
    }

    Compatibility::Compatibility()
    {
        InitializeComponent();
    }

    void Compatibility::OnNavigatedTo(const NavigationEventArgs& e)
    {
        const auto args = e.Parameter().as<Editor::NavigateToPageArgs>();
        _ViewModel = args.ViewModel().as<Editor::CompatibilityViewModel>();
        BringIntoViewWhenLoaded(args.ElementToFocus());

        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "NavigatedToPage",
            TraceLoggingDescription("Event emitted when the user navigates to a page in the settings UI"),
            TraceLoggingValue("compatibility", "PageId", "The identifier of the page that was navigated to"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    void Compatibility::ResetApplicationStateButton_Click(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::RoutedEventArgs& /*e*/)
    {
        _ViewModel.ResetApplicationState();
        ResetCacheFlyout().Hide();
    }
}
