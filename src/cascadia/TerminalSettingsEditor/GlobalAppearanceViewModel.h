// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GlobalAppearanceViewModel.g.h"
#include "ViewModelHelpers.h"
#include "Utils.h"

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct GlobalAppearanceViewModel : GlobalAppearanceViewModelT<GlobalAppearanceViewModel>, ViewModelHelper<GlobalAppearanceViewModel>
    {
    public:
        GlobalAppearanceViewModel(Model::GlobalAppSettings globalSettings, Model::WindowSettings windowSettings);

        // DON'T YOU DARE ADD A `WINRT_CALLBACK(PropertyChanged` TO A CLASS DERIVED FROM ViewModelHelper. Do this instead:
        using ViewModelHelper<GlobalAppearanceViewModel>::PropertyChanged;

        WINRT_PROPERTY(Windows::Foundation::Collections::IObservableVector<Model::Theme>, ThemeList, nullptr);
        GETSET_BINDABLE_ENUM_SETTING(NewTabPosition, Model::NewTabPosition, _WindowSettings.NewTabPosition);
        GETSET_BINDABLE_ENUM_SETTING(TabWidthMode, winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode, _WindowSettings.TabWidthMode);
        GETSET_BINDABLE_ENUM_SETTING(TabPosition, Model::TabPosition, _WindowSettings.TabPosition);
        GETSET_BINDABLE_ENUM_SETTING(NewTabButtonPosition, Model::NewTabButtonPosition, _WindowSettings.NewTabButtonPosition);
        GETSET_BINDABLE_ENUM_SETTING(TabIconStyle, Model::IconStyle, _WindowSettings.TabIconStyle);
        GETSET_BINDABLE_ENUM_SETTING(TabCloseButton, Model::TabCloseButtonVisibility, _WindowSettings.TabCloseButton);
        GETSET_BINDABLE_ENUM_SETTING(DockWindow, Model::WindowDock, _WindowSettings.DockWindow);
        GETSET_BINDABLE_ENUM_SETTING(PaneTitlebarVisibility, Model::PaneTitlebarVisibility, _WindowSettings.PaneTitlebarVisibility);
        GETSET_BINDABLE_ENUM_SETTING(Motion, Model::MotionPreference, _GlobalSettings.Motion);

    public:
        winrt::Windows::Foundation::IInspectable CurrentTheme();
        void CurrentTheme(const winrt::Windows::Foundation::IInspectable& tag);
        static winrt::hstring ThemeNameConverter(const Model::Theme& theme);

        bool InvertedDisableAnimations();
        void InvertedDisableAnimations(bool value);

        void ShowTitlebarToggled(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& args);

        // tabWidthMode is width arithmetic, so it has nothing to say about a
        // vertical strip - TerminalPage pins SizeToContent there. This drives
        // the dropdown's IsEnabled so the setting reads as inert rather than
        // looking like it was ignored.
        bool TabWidthModeEnabled();
        bool NewTabButtonPositionEnabled();
        void TabPositionChanged(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);

        // The docked size means nothing until an edge is chosen, so the slider
        // greys out with the dropdown rather than sitting there being ignored.
        bool DockSizeEnabled();
        void DockWindowChanged(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs& args);

        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, AlwaysShowTabs);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, ShowTabsFullscreen);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, ShowTabIndex);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, CloseWindowOnLastTab);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, DockSize);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, ShowTabsInTitlebar);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, UseAcrylicInTabRow);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, ShowTitleInTitlebar);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, AlwaysOnTop);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, AutoHideWindow);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_GlobalSettings, AlwaysShowNotificationIcon);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, MinimizeToNotificationArea);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, ShowAdminShield);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, ShowTabIconOnTaskbar);
        PERMANENT_OBSERVABLE_PROJECTED_SETTING(_WindowSettings, EnableUnfocusedAcrylic);

    private:
        Model::GlobalAppSettings _GlobalSettings;
        Model::WindowSettings _WindowSettings;
        winrt::Windows::Foundation::IInspectable _currentTheme;

        void _UpdateThemeList();
    };
};

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(GlobalAppearanceViewModel);
}
