// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "Integrations.g.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct Integrations : public HasScrollViewer<Integrations>, IntegrationsT<Integrations>
    {
    public:
        Integrations();

        void OnNavigatedTo(const winrt::Windows::UI::Xaml::Navigation::NavigationEventArgs& e);

        void IntegrationNavigator_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void CloseIntegrationEditor_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);

        void CredentialPassword_Changed(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void SaveCredential_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void ClearCredential_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);

        void AddMatcherAsRule_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void GoToLinkTooltip_Click(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);

        void IntegrationEnabled_Toggled(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void PreferredOwners_Loaded(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        winrt::fire_and_forget _RefreshAccount(bool discover = false, winrt::Windows::UI::Xaml::Controls::MenuFlyout menu = nullptr);
        void _RenderOwners();
        bool _SaveOwners();
        std::vector<std::wstring> _ownerValues;
        // Organizations account discovery found: tried after the pinned ones.
        std::vector<std::wstring> _discoveredOwners;
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::StackPanel> _ownerHost;
        Editor::IntegrationSettingViewModel _ownerSetting{ nullptr };
        void RepoOwners_Loaded(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void _RenderRepoOwners();
        winrt::weak_ref<winrt::Windows::UI::Xaml::Controls::StackPanel> _repoOwnerHost;
        Editor::IntegrationSettingViewModel _repoOwnerSetting{ nullptr };
        uint64_t _accountGeneration{};

        WINRT_CALLBACK(PropertyChanged, Windows::UI::Xaml::Data::PropertyChangedEventHandler);
        WINRT_OBSERVABLE_PROPERTY(Editor::IntegrationsViewModel, ViewModel, _PropertyChangedHandlers, nullptr);
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(Integrations);
}
