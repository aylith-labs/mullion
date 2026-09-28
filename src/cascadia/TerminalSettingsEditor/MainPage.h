// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "MainPage.g.h"
#include "Breadcrumb.g.h"
#include "NavigateToPageArgs.g.h"
#include "Utils.h"
#include "SearchIndex.h"

#include <ThrottledFunc.h>

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    struct Breadcrumb : BreadcrumbT<Breadcrumb>
    {
        Breadcrumb(IInspectable tag, winrt::hstring label, BreadcrumbSubPage subPage) :
            _Tag{ tag },
            _Label{ label },
            _SubPage{ subPage } {}

        hstring ToString() { return _Label; }

        WINRT_PROPERTY(IInspectable, Tag);
        WINRT_PROPERTY(winrt::hstring, Label);
        WINRT_PROPERTY(BreadcrumbSubPage, SubPage);
    };

    struct NavigateToPageArgs : NavigateToPageArgsT<NavigateToPageArgs>
    {
    public:
        NavigateToPageArgs(Windows::Foundation::IInspectable viewModel, Editor::IHostedInWindow windowRoot, const hstring& elementToFocus = {}) :
            _ViewModel(viewModel),
            _WeakWindowRoot(windowRoot),
            _ElementToFocus(elementToFocus) {}

        Editor::IHostedInWindow WindowRoot() const noexcept { return _WeakWindowRoot.get(); }
        Windows::Foundation::IInspectable ViewModel() const noexcept { return _ViewModel; }
        hstring ElementToFocus() const noexcept { return _ElementToFocus; }

    private:
        winrt::weak_ref<Editor::IHostedInWindow> _WeakWindowRoot;
        Windows::Foundation::IInspectable _ViewModel{ nullptr };
        hstring _ElementToFocus{};
    };

    struct MainPage : MainPageT<MainPage>
    {
        MainPage() = delete;
        MainPage(const Model::CascadiaSettings& settings, const Model::WindowSettings& windowSettings);

        void UpdateSettings(const Model::CascadiaSettings& settings, const Model::WindowSettings& windowSettings);

        safe_void_coroutine SettingsSearchBox_TextChanged(const Windows::UI::Xaml::Controls::AutoSuggestBox& sender, const Windows::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs& args);
        void SettingsSearchBox_QuerySubmitted(const Windows::UI::Xaml::Controls::AutoSuggestBox& sender, const Windows::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs& args);
        void SettingsSearchBox_SuggestionChosen(const Windows::UI::Xaml::Controls::AutoSuggestBox& sender, const Windows::UI::Xaml::Controls::AutoSuggestBoxSuggestionChosenEventArgs& args);
        void SettingsSearchBox_GotFocus(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void SettingsSearchBox_Tapped(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::TappedRoutedEventArgs& args);

        void SettingsNav_Loaded(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void SettingsNav_Unloaded(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void ShowDescriptionsSwitch_Toggled(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void AutoSaveSwitch_Toggled(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void SettingsNav_ItemInvoked(const Microsoft::UI::Xaml::Controls::NavigationView& sender, const Microsoft::UI::Xaml::Controls::NavigationViewItemInvokedEventArgs& args);
        void SettingsNav_PaneOpened(const Microsoft::UI::Xaml::Controls::NavigationView& sender, const Windows::Foundation::IInspectable& args);
        void SettingsNav_PaneClosed(const Microsoft::UI::Xaml::Controls::NavigationView& sender, const Windows::Foundation::IInspectable& args);
        void SaveButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void ResetButton_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void BreadcrumbBar_ItemClicked(const Microsoft::UI::Xaml::Controls::BreadcrumbBar& sender, const Microsoft::UI::Xaml::Controls::BreadcrumbBarItemClickedEventArgs& args);

        void SetHostingWindow(uint64_t hostingWindow) noexcept;
        void NavigateToLinkTooltipRule(int32_t ruleIndex, const winrt::hstring& ruleName);
        void NavigateToDocumentationTopic(const winrt::hstring& topicElementName);
        bool TryPropagateHostingWindow(IInspectable object) noexcept;
        uint64_t GetHostingWindow() const noexcept;

        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush();

        // Called from ViewModelChangeHook when any view model raises PropertyChanged.
        // Public only because the free function that forwards to it lives at file
        // scope in MainPage.cpp, which cannot reach a private member.
        void OnAnyViewModelChanged();

        Windows::Foundation::Collections::IObservableVector<IInspectable> Breadcrumbs() noexcept;
        Editor::ExtensionsViewModel ExtensionsVM() const noexcept { return _extensionsVM; }
        Editor::ActionsViewModel ActionsVM() const noexcept { return _actionsVM; }

        til::typed_event<Windows::Foundation::IInspectable, Model::SettingsTarget> OpenJson;
        til::typed_event<Windows::Foundation::IInspectable, Windows::Foundation::Collections::IVectorView<Model::SettingsLoadWarnings>> ShowLoadWarningsDialog;

    private:
        Windows::Foundation::Collections::IObservableVector<IInspectable> _breadcrumbs;
        Windows::Foundation::Collections::IObservableVector<IInspectable> _menuItemSource;
        size_t _originalNumItems = 0u;

        Model::CascadiaSettings _settingsSource;
        Model::CascadiaSettings _settingsClone;
        Model::WindowSettings _windowSettingsSource{ nullptr };
        Model::WindowSettings _windowSettingsClone{ nullptr };

        std::optional<HWND> _hostingHwnd;

        void _InitializeProfilesList();
        void _CreateAndNavigateToNewProfile(const Model::Profile& profile);
        void _DeleteProfile(const Windows::Foundation::IInspectable sender, const Editor::DeleteProfileEventArgs& args);
        void _AddProfileHandler(const winrt::guid profileGuid);

        void _SetupProfileEventHandling(const winrt::Microsoft::Terminal::Settings::Editor::ProfileViewModel profile);
        void _SetupColorSchemesEventHandling();
        void _SetupActionsEventHandling();
        void _SetupLinkTooltipEventHandling();
        void _SetupIntegrationsEventHandling();
        void _SetupProfilesPageEventHandling();
        void _NavigateToProfileSubPage(const Editor::ProfileViewModel& profile, ProfileSubPage page, const IInspectable& breadcrumbTag, const hstring& elementToFocus);

        void _PreNavigateHelper();
        void _UpdateForkNavItems();
        void _Navigate(const IInspectable& vm, BreadcrumbSubPage subPage = BreadcrumbSubPage::None, hstring elementToFocus = {});
        void _NavigateToProfileHandler(const IInspectable& sender, winrt::guid profileGuid);
        void _NavigateToColorSchemeHandler(const IInspectable& sender, const IInspectable& args);
        void _NavigateToLinkTooltipHandler(const IInspectable& sender, const IInspectable& args);
        Editor::ProfileViewModel _FindProfileViewModelByGuid(winrt::guid profileGuid) const;

        void _AppendProfilesRootCrumb();
        void _SelectNavItemByTag(std::wstring_view tag);

        void _AnnounceNavPaneState(bool opened);

        void _UpdateBackgroundForMica();
        void _MoveXamlParsedNavItemsIntoItemSource();

        // globals.motion, applied to this page's own animations.
        void _ApplyMotionPreference();
        // What MainPage.xaml asked for, kept so reduced motion can be undone.
        winrt::Windows::UI::Xaml::Media::Animation::TransitionCollection _pageTransitions{ nullptr };

        // Unsaved-change tracking, and the auto-save that rides on it.
        //
        // There is no single "the settings changed" signal to hang this on: the
        // observable view-model macros raise PropertyChanged for pure UI state as well
        // as for settings, and GETSET_BINDABLE_ENUM_SETTING writes through to the
        // settings model without raising anything. So the verdict is always a
        // comparison of CascadiaSettings::SerializedFingerprint against what it was
        // when the clone was made, and the notifications below only decide *when* to
        // look.
        void _ArmDirtyTracking();
        void _RecordCleanState();
        void _ReevaluateDirtyState();
        void _ApplySaveButtonState();
        void _ReadEditorChromePreferences();
        bool _AbsorbOwnAutoSaveReload(const Model::CascadiaSettings& settings);

        // How many of our own auto-save writes to keep hashes for. A reload is not
        // guaranteed per write -- ReloadSettingsThrottled debounces -- so the list
        // absorbs a burst, and anything this far back is never coming.
        static constexpr size_t MaxTrackedAutoSaveWrites{ 8 };

        // The clone's fingerprint as of the last time it matched settings.json: when
        // the editor opened, when Discard rebuilt it, or when a save wrote it out.
        //
        // The clone against its own earlier serialization, deliberately, rather than the
        // clone against _settingsSource. Copy() only has to produce something that
        // behaves the same, not something that serializes byte-for-byte identically, and
        // a source-vs-clone comparison would read as "unsaved changes" from the instant
        // Settings opened if it ever did not. Comparing the clone to itself asks the
        // narrower question we actually want answered: has anything moved since we last
        // agreed with the file.
        winrt::hstring _cleanFingerprint;
        // The clean clone's serialization itself, kept so Save and Discard can say
        // what they would change (CascadiaSettings::DescribeChangesFrom), and the
        // fingerprint that list was last built for, so the one-second sweep does not
        // rebuild it when nothing moved.
        winrt::hstring _cleanSerialized;
        winrt::hstring _describedFingerprint;
        void _DescribeUnsavedChanges(const winrt::hstring& fingerprint);
        bool _unsavedChanges{ false };
        bool _autoSave{ false };
        // Set while _ReadEditorChromePreferences is pushing stored values into the two
        // switches, so their Toggled handlers can tell "restoring" from "the user just
        // flipped this" and not write the value straight back out.
        bool _applyingChromePreferences{ false };
        // settings.json's hash after each of our own auto-save writes, so the reloads
        // those writes provoke can be recognised and not tear the page down mid-edit.
        // Only ever appended to by the auto-save path -- a manual Save still goes
        // through the full rebuild, as it always has.
        std::vector<winrt::hstring> _autoSaveWrittenHashes;
        // Coalesces a burst of PropertyChanged (every keystroke in a text box raises
        // one) into a single serialization.
        std::shared_ptr<ThrottledFunc<>> _dirtyCheckThrottle;
        // The backstop for the changes nothing notifies about. Runs only while the
        // page is loaded.
        winrt::Windows::UI::Xaml::DispatcherTimer _dirtyCheckTimer{ nullptr };

        safe_void_coroutine _UpdateSearchIndex();

        // The settings search box remembers what it was used to reach, and offers those
        // again while it is empty. Persisted in ApplicationState next to the other two
        // pieces of editor chrome, for the reasons given there.
        void _ShowSearchHistory();
        void _RecordSearchHistory(const LocalizedIndexEntry& entry);

        // How many chosen settings to keep. Enough to cover coming back to the same few
        // rows over a session, short enough that the list stays a list and not a log.
        static constexpr size_t MaxSearchHistoryEntries{ 10 };

        winrt::Microsoft::Terminal::Settings::Editor::ProfileViewModel _profileDefaultsVM{ nullptr };
        winrt::Microsoft::Terminal::Settings::Editor::ColorSchemesPageViewModel _colorSchemesPageVM{ nullptr };
        winrt::Microsoft::Terminal::Settings::Editor::ActionsViewModel _actionsVM{ nullptr };
        winrt::Microsoft::Terminal::Settings::Editor::NewTabMenuViewModel _newTabMenuPageVM{ nullptr };
        winrt::Microsoft::Terminal::Settings::Editor::ExtensionsViewModel _extensionsVM{ nullptr };
        winrt::Microsoft::Terminal::Settings::Editor::ProfilesPageViewModel _profilesPageVM{ nullptr };
        // Rebuilt on every navigation to the Link Tooltip page (it holds no state
        // the page itself doesn't), and kept only so opening a rule can push a
        // "Link Tooltip > <rule>" crumb.
        winrt::Microsoft::Terminal::Settings::Editor::LinkTooltipViewModel _linkTooltipVM{ nullptr };
        winrt::Microsoft::Terminal::Settings::Editor::IntegrationsViewModel _integrationsVM{ nullptr };

        Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IObservableVector<Windows::Foundation::IInspectable>> _currentSearch{ nullptr };

        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _profileViewModelChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _colorSchemesPageViewModelChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _actionsViewModelChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _ntmViewModelChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _extensionsViewModelChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _linkTooltipViewModelChangedRevoker;
        Windows::UI::Xaml::Data::INotifyPropertyChanged::PropertyChanged_revoker _integrationsViewModelChangedRevoker;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Editor::factory_implementation
{
    BASIC_FACTORY(MainPage);
}
