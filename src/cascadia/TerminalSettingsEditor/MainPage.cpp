// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "MainPage.h"
#include "SettingsCard.h"
#include "MainPage.g.cpp"
#include "Launch.h"
#include "SessionRestore.h"
#include "Interaction.h"
#include "LinkTooltip.h"
#include "Integrations.h"
#include "Activity.h"
#include "Documentation.h"
#include "Compatibility.h"
#include "Extensions.h"
#include "Actions.h"
#include "ProfileViewModel.h"
#include "GlobalAppearance.h"
#include "GlobalAppearanceViewModel.h"
#include "ColorSchemes.h"
#include "EditColorScheme.h"
#include "Profiles.h"
#include "InteractionViewModel.h"
#include "LinkTooltipViewModel.h"
#include "IntegrationsViewModel.h"
#include "LaunchViewModel.h"
#include "SessionRestoreViewModel.h"
#include "NewTabMenuViewModel.h"
#include "NewTabMenu.h"
#include "NavConstants.h"
#include "ViewModelHelpers.h"
#include "..\types\inc\utils.hpp"
#include <..\WinRTUtils\inc\Utils.h>

#include <dwmapi.h>
#include <fmt/compile.h>

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
}

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // GH#19927 - Walk the visual tree of the given element to find any
    // Popups and set the theme on their Child element. In XAML Islands,
    // popup children render in the PopupRoot (separate from the content
    // tree), so they don't inherit our page's RequestedTheme
    static void _setThemeOnPopups(const winrt::Windows::UI::Xaml::DependencyObject& element,
                                  const winrt::Windows::UI::Xaml::ElementTheme theme)
    {
        const auto childCount = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(element);
        for (int32_t i = 0; i < childCount; i++)
        {
            const auto child = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(element, i);
            if (const auto popup = child.try_as<winrt::Windows::UI::Xaml::Controls::Primitives::Popup>())
            {
                if (const auto popupChild = popup.Child().try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
                {
                    popupChild.RequestedTheme(theme);
                }
            }
            _setThemeOnPopups(child, theme);
        }
    }

    // How often the unsaved-change backstop runs while the settings page is up.
    // Deliberately slow: it exists for the changes that notify nothing at all, and a
    // Save button that lights up within a second of a dropdown is indistinguishable
    // from instant, while the serialization it costs is not free.
    static constexpr std::chrono::seconds DirtyCheckInterval{ 1 };
    // And how long after a notified change, which is nearly all of them, to wait for
    // the typing to stop before serializing.
    static constexpr std::chrono::milliseconds DirtyCheckDebounce{ 120 };

    // The page that ViewModelChangeHook points at. Weak, and resolved on every call,
    // so the hook cannot outlive the page -- there is no destructor on a XAML page
    // that would reliably clear a raw pointer.
    static winrt::weak_ref<Editor::MainPage> g_mainPage;

    static void _notifyMainPageOfViewModelChange()
    {
        if (const auto page = g_mainPage.get())
        {
            winrt::get_self<MainPage>(page)->OnAnyViewModelChanged();
        }
    }

    // The first entry in the navigation that is actually a page.
    //
    // Since the list grew group headings, "the first menu item" and "the first
    // page" are different things: a NavigationViewItemHeader derives from
    // NavigationViewItemBase, not from NavigationViewItem, so it has no Tag and
    // as<NavigationViewItem> on one throws E_NOINTERFACE. Both callers used to
    // take entry zero and cast it, and on a Loaded handler that throw is a stowed
    // exception - the window fail-fasts at 0xc000027b before Settings draws.
    //
    // Returns null for a list with no pages at all, which cannot happen today but
    // is cheaper to handle than to prove impossible.
    template<typename T>
    static MUX::Controls::NavigationViewItem _firstNavigablePage(const T& items)
    {
        if (!items)
        {
            return nullptr;
        }
        for (const auto& item : items)
        {
            if (const auto navItem = item.template try_as<MUX::Controls::NavigationViewItem>())
            {
                return navItem;
            }
        }
        return nullptr;
    }

    static WUX::Controls::FontIcon _fontIconForNavTag(const std::wstring_view navTag)
    {
        WUX::Controls::FontIcon icon{};
        icon.Glyph(NavTagIconMap[navTag]);
        icon.FontFamily(Media::FontFamily{ L"Segoe Fluent Icons, Segoe MDL2 Assets" });
        icon.FontSize(16);
        return icon;
    }

    static Editor::ProfileViewModel _viewModelForProfile(const Model::Profile& profile, const Model::CascadiaSettings& appSettings, const Model::WindowSettings& windowSettings, const Windows::UI::Core::CoreDispatcher& dispatcher)
    {
        return winrt::make<implementation::ProfileViewModel>(profile, appSettings, windowSettings, dispatcher);
    }

    static ProfileSubPage ProfileSubPageFromBreadcrumb(BreadcrumbSubPage subPage, const Editor::ProfileViewModel& profile)
    {
        switch (subPage)
        {
        case BreadcrumbSubPage::None:
            return ProfileSubPage::Base;
        case BreadcrumbSubPage::Profile_Appearance:
            return ProfileSubPage::Appearance;
        case BreadcrumbSubPage::Profile_UnfocusedAppearance:
            // If the profile has no unfocused appearance, fall back to the base page.
            return profile.HasUnfocusedAppearance() ? ProfileSubPage::UnfocusedAppearance : ProfileSubPage::Base;
        case BreadcrumbSubPage::Profile_Terminal:
            return ProfileSubPage::Terminal;
        case BreadcrumbSubPage::Profile_Advanced:
            return ProfileSubPage::Advanced;
        default:
            // This should never happen
            assert(false);
            return ProfileSubPage::Base;
        }
    }

    MainPage::MainPage(const CascadiaSettings& settings, const Model::WindowSettings& windowSettings) :
        _settingsSource{ settings },
        _settingsClone{ settings.Copy() },
        _windowSettingsSource{ windowSettings },
        _windowSettingsClone{ _settingsClone.WindowSettingsDefaults() },
        _profilesPageVM{ winrt::make<ProfilesPageViewModel>() }
    {
        InitializeComponent();
        // The shortcuts list used to be given its scrolling here, by disabling
        // this ScrollViewer's while the Actions page was up, so the ListView
        // would be measured with a real height and virtualize instead of
        // realizing all hundred-odd rows. It did make the page open instantly.
        // It also emptied it.
        //
        // A ScrollViewer whose scrolling is Disabled reports an empty effective
        // viewport to its descendants, and a virtualizing ItemsStackPanel
        // intersects its realization window with that - so the list came out
        // 879x591 with zero realized containers on every visit. Nothing threw;
        // UIA showed the ViewModel alive (its Add button still navigated) and
        // the items source populated. The list next door on the Dropdown Menu
        // page, measured with infinite height, has always rendered fine.
        //
        // Actions.xaml bounds the list itself now, with a MaxHeight, which gives
        // the panel a viewport without anything above it having to change.
        // Before anything navigates, so the first page in does not drill in when
        // the user asked for no motion.
        _pageTransitions = contentFrame().ContentTransitions();
        _ApplyMotionPreference();

        _UpdateBackgroundForMica();

        _newTabMenuPageVM = winrt::make<NewTabMenuViewModel>(_settingsClone);
        _ntmViewModelChangedRevoker = _newTabMenuPageVM.PropertyChanged(winrt::auto_revoke, [this](auto&&, const PropertyChangedEventArgs& args) {
            const auto settingName{ args.PropertyName() };
            if (settingName == L"CurrentFolder")
            {
                if (const auto& currentFolder = _newTabMenuPageVM.CurrentFolder())
                {
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(currentFolder), currentFolder.Name(), BreadcrumbSubPage::NewTabMenu_Folder));
                    SettingsMainPage_ScrollViewer().ScrollToVerticalOffset(0);
                }
                else
                {
                    // If we don't have a current folder, we're at the root of the NTM
                    _breadcrumbs.Clear();
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(newTabMenuTag), RS_(L"Nav_NewTabMenu/Content"), BreadcrumbSubPage::None));
                }
                contentFrame().Navigate(xaml_typename<Editor::NewTabMenu>(), winrt::make<NavigateToPageArgs>(_newTabMenuPageVM, *this));
            }
        });

        _colorSchemesPageVM = winrt::make<ColorSchemesPageViewModel>(_settingsClone);
        _SetupColorSchemesEventHandling();

        _actionsVM = winrt::make<ActionsViewModel>(_settingsClone);
        _SetupActionsEventHandling();

        auto extensionsVMImpl = winrt::make_self<ExtensionsViewModel>(_settingsClone, _colorSchemesPageVM);
        extensionsVMImpl->NavigateToProfileRequested({ this, &MainPage::_NavigateToProfileHandler });
        extensionsVMImpl->NavigateToColorSchemeRequested({ this, &MainPage::_NavigateToColorSchemeHandler });
        _extensionsVM = *extensionsVMImpl;
        _extensionsViewModelChangedRevoker = _extensionsVM.PropertyChanged(winrt::auto_revoke, [=](auto&&, const PropertyChangedEventArgs& args) {
            const auto settingName{ args.PropertyName() };
            if (settingName == L"CurrentExtensionPackage")
            {
                if (const auto& currentExtensionPackage = _extensionsVM.CurrentExtensionPackage())
                {
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(currentExtensionPackage), currentExtensionPackage.DisplayName(), BreadcrumbSubPage::Extensions_Extension));
                    SettingsMainPage_ScrollViewer().ScrollToVerticalOffset(0);
                }
                else
                {
                    // If we don't have a current extension package, we're at the root of the Extensions page
                    _breadcrumbs.Clear();
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(extensionsTag), RS_(L"Nav_Extensions/Content"), BreadcrumbSubPage::None));
                }
                contentFrame().Navigate(xaml_typename<Editor::Extensions>(), winrt::make<NavigateToPageArgs>(_extensionsVM, *this));
            }
        });

        _SetupProfilesPageEventHandling();

        // Make sure to initialize the profiles _after_ we have initialized the color schemes page VM, because we pass
        // that VM into the appearance VMs within the profiles. The Profiles VM owns the per-profile list itself.
        _InitializeProfilesList();

        // Apply icons and tooltips (GH#19688, long names may be truncated) to static nav items
        for (const auto& item : SettingsNav().MenuItems())
        {
            const auto navItem = item.try_as<MUX::Controls::NavigationViewItem>();
            if (!navItem)
            {
                continue;
            }

            const auto stringTag = navItem.Tag().try_as<hstring>();
            if (!stringTag)
            {
                continue;
            }

            navItem.Icon(_fontIconForNavTag(*stringTag));
            if (const auto navItemContentString = navItem.Content().try_as<hstring>())
            {
                WUX::Controls::ToolTipService::SetToolTip(navItem, box_value(*navItemContentString));
            }
        }
        OpenJsonNavItem().Icon(_fontIconForNavTag(openJsonTag));
        WUX::Controls::ToolTipService::SetToolTip(OpenJsonNavItem(), box_value(RS_(L"Nav_OpenJSON/Content")));

        // The footer items are not in the loop above -- it walks MenuItems -- so this
        // one gets its icon and tooltip the same way Open JSON does: the tooltip is the
        // label, because with the pane collapsed the icon is all there is and the
        // tooltip is the only thing that says what it does. The longer explanation goes
        // to HelpText, and the switch itself carries it as a tooltip via its x:Uid, so
        // hovering the row names the setting and hovering the switch explains it.
        //
        // Its Content is the switch rather than a label, so the row has no name to
        // borrow and gets one explicitly; without it the automation tree shows an
        // unnamed list item wrapping a named switch.
        ShowDescriptionsNavItem().Icon(_fontIconForNavTag(showDescriptionsTag));
        Automation::AutomationProperties::SetName(ShowDescriptionsNavItem(), RS_(L"Nav_ShowDescriptions/OnContent"));
        WUX::Controls::ToolTipService::SetToolTip(ShowDescriptionsNavItem(), box_value(RS_(L"Nav_ShowDescriptions/OnContent")));

        Automation::AutomationProperties::SetHelpText(SaveButton(), RS_(L"Settings_SaveSettingsButton/[using:Windows.UI.Xaml.Controls]ToolTipService/ToolTip"));
        Automation::AutomationProperties::SetHelpText(ResetButton(), RS_(L"Settings_ResetSettingsButton/[using:Windows.UI.Xaml.Controls]ToolTipService/ToolTip"));
        Automation::AutomationProperties::SetHelpText(OpenJsonNavItem(), RS_(L"Nav_OpenJSON/[using:Windows.UI.Xaml.Controls]ToolTipService/ToolTip"));
        Automation::AutomationProperties::SetHelpText(ShowDescriptionsNavItem(), RS_(L"Nav_ShowDescriptions/[using:Windows.UI.Xaml.Controls]ToolTipService/ToolTip"));
        Automation::AutomationProperties::SetHelpText(AutoSaveSwitch(), RS_(L"Settings_AutoSaveSwitch/[using:Windows.UI.Xaml.Controls]ToolTipService/ToolTip"));

        _breadcrumbs = single_threaded_observable_vector<IInspectable>();
        _UpdateSearchIndex();

        _ReadEditorChromePreferences();

        // Last, so the baseline is taken from a clone nothing has touched yet. The
        // machinery that watches for it moving is armed in SettingsNav_Loaded rather
        // than here, because it needs get_weak() and this page is a composed XAML
        // control -- the outer object a weak reference has to name is not necessarily
        // in place while this constructor is still running.
        _RecordCleanState();
    }

    // Method Description:
    // - Update the Settings UI with a new CascadiaSettings to bind to
    // Arguments:
    // - settings - the new settings source
    // - windowSettings - the new window settings source
    // Return value:
    // - <none>
    void MainPage::UpdateSettings(const Model::CascadiaSettings& settings, const Model::WindowSettings& windowSettings)
    {
        // An auto-save writes settings.json, the Terminal notices the file changed,
        // reloads, and hands the result straight back here. Taking the full path for
        // that would rebuild the clone and every view model and re-navigate, which
        // with auto-save on happens while you are still typing -- the control you are
        // editing loses focus mid-word. So a reload carrying the hash one of our own
        // writes produced is absorbed: adopt the new source, keep the clone and the
        // view models, and leave the page where it is.
        //
        // Deliberately narrow. Only the auto-save path records a hash, so a manual
        // Save still rebuilds exactly as it always has, and any other reason the file
        // changed -- an external editor, defaults.json, a dynamic profile appearing --
        // cannot match and takes the full path. What absorbing costs is that
        // normalisation the load performed (a generated guid on a brand-new profile,
        // say) is not reflected in the editor until something reloads it for another
        // reason.
        if (_AbsorbOwnAutoSaveReload(settings))
        {
            _settingsSource = settings;
            _windowSettingsSource = windowSettings;
            return;
        }

        _settingsSource = settings;
        _settingsClone = settings.Copy();
        _windowSettingsSource = windowSettings;
        // NOTE: explicitly using the default window settings for the settings editor.
        // The settings editor isn't really per-window-name aware currently. We can fix that in the future.
        _windowSettingsClone = _settingsClone.WindowSettingsDefaults();

        _UpdateBackgroundForMica();
        _ApplyMotionPreference();

        // Seed the provenance mark before anything navigates, so the first page drawn
        // already agrees with the saved setting rather than picking it up only once
        // Appearance has been visited.
        SettingsCard::ImprintEnabled(_settingsClone.GlobalSettings().AylithImprint());
        SettingsCard::JsonOnlyImprintEnabled(_settingsClone.GlobalSettings().AylithImprintJsonOnly());
        _UpdateForkNavItems();

        // Capture data about where we are right now, so we can re-navigate to the same
        // place after we rebuild all the settings.
        IInspectable destination{ nullptr };
        auto subPage = BreadcrumbSubPage::None;
        if (const auto size = _breadcrumbs.Size(); size > 0)
        {
            const auto& crumb = _breadcrumbs.GetAt(size - 1).as<Breadcrumb>();
            destination = crumb->Tag();
            subPage = crumb->SubPage();
        }

        _InitializeProfilesList();

        // Update the Nav State with the new version of the settings
        _colorSchemesPageVM.UpdateSettings(_settingsClone);
        _actionsVM.UpdateSettings(_settingsClone);
        _newTabMenuPageVM.UpdateSettings(_settingsClone);
        _extensionsVM.UpdateSettings(_settingsClone, _colorSchemesPageVM);
        _profileDefaultsVM = nullptr; // Lazy-loaded upon navigation

        // Same deal, and load-bearing rather than tidiness: both of these hold the
        // WindowSettings clone they were built with. Discard replaces that clone, so
        // a surviving view model would keep reading and writing the object we just
        // threw away -- every edit landing somewhere nothing will ever save. Dropping
        // them also makes _Navigate take its "build a fresh one" path below instead
        // of the "already here, just close the sub-page" one, which would otherwise
        // reuse the stale VM.
        _linkTooltipVM = nullptr;
        _integrationsVM = nullptr;

        if (const auto& profileTag{ destination.try_as<Editor::ProfileViewModel>() })
        {
            // Find the new profile VM by guid
            if (const auto newProfileVM = _FindProfileViewModelByGuid(profileTag.OriginalProfileGuid()))
            {
                destination = newProfileVM;
            }
            else
            {
                // Fall back to the Profiles landing page
                destination = box_value(profilesTag);
                subPage = BreadcrumbSubPage::None;
            }
        }
        else if (destination.try_as<Editor::FolderEntryViewModel>())
        {
            destination = box_value(newTabMenuTag);
            subPage = BreadcrumbSubPage::NewTabMenu_Folder;
        }
        else if (destination.try_as<Editor::ExtensionPackageViewModel>())
        {
            destination = box_value(extensionsTag);
            subPage = BreadcrumbSubPage::Extensions_Extension;
        }
        else if (!destination.try_as<hstring>())
        {
            // Couldn't find a meaningful previous page. Fall back to the first one
            // that is a page: entry zero is a group heading, which carries no tag
            // and is not a NavigationViewItem.
            if (const auto first = _firstNavigablePage(_menuItemSource))
            {
                destination = first.Tag();
                subPage = BreadcrumbSubPage::None;
            }
        }

        _Navigate(destination, subPage);

        _UpdateSearchIndex();
    }

    void MainPage::SetHostingWindow(uint64_t hostingWindow) noexcept
    {
        _hostingHwnd.emplace(reinterpret_cast<HWND>(hostingWindow));
        // Now that we have a HWND, update our own BG to account for if that
        // window is using mica or not.
        _UpdateBackgroundForMica();
    }

    bool MainPage::TryPropagateHostingWindow(IInspectable object) noexcept
    {
        if (_hostingHwnd)
        {
            if (auto initializeWithWindow{ object.try_as<IInitializeWithWindow>() })
            {
                return SUCCEEDED(initializeWithWindow->Initialize(*_hostingHwnd));
            }
        }
        return false;
    }

    // Method Description:
    // - Creates a new profile and navigates to it in the Settings UI
    // Arguments:
    // - profileGuid: the guid of the profile we want to duplicate,
    //                can be empty to indicate that we should create a fresh profile
    void MainPage::_AddProfileHandler(winrt::guid profileGuid)
    {
        if (profileGuid != winrt::guid{})
        {
            // if we were given a non-empty guid, we want to duplicate the corresponding profile
            const auto profile = _settingsClone.FindProfile(profileGuid);
            if (profile)
            {
                const auto duplicated = _settingsClone.DuplicateProfile(profile);
                _CreateAndNavigateToNewProfile(duplicated);
            }
        }
        else
        {
            // we were given an empty guid, create a new profile
            _CreateAndNavigateToNewProfile(nullptr);
        }
    }

    uint64_t MainPage::GetHostingWindow() const noexcept
    {
        return reinterpret_cast<uint64_t>(_hostingHwnd.value_or(nullptr));
    }

    // Function Description:
    // - Called when the NavigationView is loaded. Navigates to the first item in the NavigationView, if no item is selected
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void MainPage::SettingsNav_Loaded(const IInspectable&, const RoutedEventArgs&)
    {
        if (SettingsNav().SelectedItem() == nullptr)
        {
            // The first entry is a group heading now, not a page. A
            // NavigationViewItemHeader derives from NavigationViewItemBase and not
            // from NavigationViewItem, so as<> on it throws E_NOINTERFACE - and on
            // a Loaded handler that is a stowed exception, which fail-fasts the
            // whole window at 0xc000027b before Settings ever draws.
            if (const auto initialItem = _firstNavigablePage(SettingsNav().MenuItems()))
            {
                SettingsNav().SelectedItem(initialItem);

                // Manually navigate because setting the selected item programmatically doesn't trigger ItemInvoked.
                if (const auto tag = initialItem.Tag())
                {
                    _Navigate(tag, BreadcrumbSubPage::None);
                }
            }
        }

        // GH#19927 - Theme the search box's internal popup now that the
        // visual tree is ready and control templates are applied
        const auto theme = _settingsSource.GlobalSettings().CurrentTheme(_windowSettingsSource);
        const auto hasThemeForSettings{ theme.Settings() != nullptr };
        const auto requestedTheme = hasThemeForSettings ? theme.Settings().RequestedTheme() : theme.RequestedTheme();
        _setThemeOnPopups(SettingsSearchBox(), requestedTheme);

        _ArmDirtyTracking();
    }

    // Starts watching for unsaved changes. Here rather than in the constructor because
    // both the throttle and the timer need a weak reference to this page, and
    // get_weak() on a composed XAML control wants the outer object -- which is not
    // necessarily in place while the derived constructor is still running. Nothing can
    // change a setting before the page has loaded, so nothing is missed by waiting.
    //
    // Loaded fires again whenever the settings pane is re-attached, so everything here
    // is written to be safe to repeat.
    void MainPage::_ArmDirtyTracking()
    {
        if (!_dirtyCheckThrottle)
        {
            _dirtyCheckThrottle = std::make_shared<ThrottledFunc<>>(
                winrt::Windows::System::DispatcherQueue::GetForCurrentThread(),
                til::throttled_func_options{
                    .delay = DirtyCheckDebounce,
                    .debounce = true,
                    .trailing = true,
                },
                [weakThis = get_weak()]() {
                    if (const auto page = weakThis.get())
                    {
                        page->_ReevaluateDirtyState();
                    }
                });
        }

        if (!_dirtyCheckTimer)
        {
            _dirtyCheckTimer = WUX::DispatcherTimer{};
            _dirtyCheckTimer.Interval(DirtyCheckInterval);
            _dirtyCheckTimer.Tick([weakThis = get_weak()](auto&&, auto&&) {
                const auto page = weakThis.get();
                if (!page)
                {
                    return;
                }

                // get_weak() here is the implementation's own, so get() already
                // hands back a com_ptr to the implementation. get_self is for
                // going from a *projected* type to the implementation behind it
                // (see _notifyMainPageOfViewModelChange, which resolves a
                // weak_ref<Editor::MainPage> and does need it); applying it to
                // this com_ptr asks the compiler for producer<MainPage, MainPage>,
                // which does not exist.
                const auto self = page.get();

                // SettingsNav_Unloaded is the fast way out, and for two of the three
                // hosts it is the one that fires: the tab host's pane teardown detaches
                // the page, and the dialog host explicitly does dialog.Content(nullptr)
                // when it closes. But this sweep serializes the whole settings document
                // on the UI thread once a second, so it must not depend on having found
                // every teardown path. settingsUIHost "window" is the one I could not
                // follow to an Unloaded -- the page lives in a Terminal window of its
                // own, and closing that destroys the island rather than detaching
                // anything -- so the tick also checks for itself and gives up when the
                // page is no longer in a live tree. A reparent that fires Unloaded
                // without a Loaded is covered by the same check.
                // Through the projected FrameworkElement, not the implementation
                // type: weakThis.get() hands back a com_ptr to the implementation,
                // which does not carry the projection's own members.
                const auto element = page.try_as<WUX::FrameworkElement>();
                if (!element || !element.IsLoaded())
                {
                    self->_dirtyCheckTimer.Stop();
                    return;
                }

                self->_ReevaluateDirtyState();
            });
        }
        _dirtyCheckTimer.Start();

        g_mainPage = *this;
        ViewModelChangeHook::AnyViewModelChanged = &_notifyMainPageOfViewModelChange;
    }

    void MainPage::SettingsNav_Unloaded(const IInspectable&, const RoutedEventArgs&)
    {
        // The timer is the only thing here that costs anything while nothing is on
        // screen -- it would go on serializing the settings once a second for a page
        // nobody is looking at. This is the prompt way to stop it; the tick itself also
        // stops when the page is no longer loaded, so a host whose teardown does not
        // reach here loses at most one more sweep. The hook stays armed either way: it
        // is cheap, it resolves a weak reference, and Loaded can bring this same page
        // back.
        if (_dirtyCheckTimer)
        {
            _dirtyCheckTimer.Stop();
        }
    }

    void MainPage::_AnnounceNavPaneState(bool opened)
    {
        if (const auto automationPeer{ Automation::Peers::FrameworkElementAutomationPeer::FromElement(SettingsNav()) })
        {
            automationPeer.RaiseNotificationEvent(
                Automation::Peers::AutomationNotificationKind::ActionCompleted,
                Automation::Peers::AutomationNotificationProcessing::MostRecent,
                opened ? RS_(L"Nav_PaneOpenedAnnouncement") : RS_(L"Nav_PaneClosedAnnouncement"),
                L"SettingsNavPaneState");
        }
    }

    void MainPage::SettingsNav_PaneOpened(const MUX::Controls::NavigationView&, const IInspectable&)
    {
        _AnnounceNavPaneState(true);
    }

    void MainPage::SettingsNav_PaneClosed(const MUX::Controls::NavigationView&, const IInspectable&)
    {
        _AnnounceNavPaneState(false);
    }

    // Function Description:
    // - Called when NavigationView items are invoked. Navigates to the corresponding page.
    // Arguments:
    // - args - additional event info from invoking the NavViewItem
    // Return Value:
    // - <none>
    void MainPage::SettingsNav_ItemInvoked(const MUX::Controls::NavigationView&, const MUX::Controls::NavigationViewItemInvokedEventArgs& args)
    {
        if (const auto clickedItemContainer = args.InvokedItemContainer())
        {
            // Group headings are NavigationViewItemHeaders, not NavigationViewItems, and
            // MUX's style for them sets IsEnabled="False" and IsTabStop="False" - so this
            // should never fire for one. Guarded anyway, because the fall-through below
            // is _Navigate with a null tag, which clears the breadcrumbs and navigates
            // nowhere; a blank page is a poor answer to a click that should not have been
            // possible.
            if (!clickedItemContainer.try_as<MUX::Controls::NavigationViewItem>())
            {
                return;
            }

            const auto navString = clickedItemContainer.Tag().try_as<hstring>();

            // Handled ahead of the scroll-to-top below, because this row is a toggle
            // rather than a destination and snapping the page back to the top because
            // someone flipped a switch in the footer would be its own small bug.
            if (navString && *navString == showDescriptionsTag)
            {
                // With the pane open, the switch is sitting right there and is the only
                // thing that flips it: if this flipped it too, a click the switch did
                // not swallow would be counted twice and cancel itself out.
                //
                // Collapsed, the pane draws the item as an icon and never realizes its
                // content, so there is no switch to click and nothing to double-fire.
                // The row has to be the control, or the setting is unreachable for as
                // long as the pane stays narrow - which here is most of the time.
                // Assigning IsOn raises Toggled, so the one handler still does the work.
                if (!SettingsNav().IsPaneOpen())
                {
                    const auto toggle = ShowDescriptionsSwitch();
                    toggle.IsOn(!toggle.IsOn());
                }
                return;
            }

            if (clickedItemContainer.IsSelected())
            {
                // Clicked on the selected item.
                // Don't navigate to the same page again.
                return;
            }
            else
            {
                // If we are navigating to a new page, scroll to the top
                SettingsMainPage_ScrollViewer().ScrollToVerticalOffset(0);
            }

            if (navString)
            {
                if (*navString == openJsonTag)
                {
                    const auto window = CoreWindow::GetForCurrentThread();
                    const auto rAltState = window.GetKeyState(VirtualKey::RightMenu);
                    const auto lAltState = window.GetKeyState(VirtualKey::LeftMenu);
                    const auto altPressed = WI_IsFlagSet(lAltState, CoreVirtualKeyStates::Down) ||
                                            WI_IsFlagSet(rAltState, CoreVirtualKeyStates::Down);
                    const auto target = altPressed ? SettingsTarget::DefaultsFile : SettingsTarget::SettingsFile;

                    TraceLoggingWrite(
                        g_hTerminalSettingsEditorProvider,
                        "OpenJson",
                        TraceLoggingDescription("Event emitted when the user clicks the Open JSON button in the settings UI"),
                        TraceLoggingValue(target == SettingsTarget::DefaultsFile ? "DefaultsFile" : "SettingsFile", "SettingsTarget", "The target settings file"),
                        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

                    OpenJson.raise(nullptr, target);
                    return;
                }
            }
            _Navigate(clickedItemContainer.Tag(), BreadcrumbSubPage::None);
        }
    }

    void MainPage::_PreNavigateHelper()
    {
        _profileViewModelChangedRevoker.revoke();
        // The Link Tooltip and Integrations handlers update breadcrumbs on
        // selection changes, so they must not be listening while on other pages.
        _linkTooltipViewModelChangedRevoker.revoke();
        _integrationsViewModelChangedRevoker.revoke();
        _breadcrumbs.Clear();
    }

    void MainPage::_SetupColorSchemesEventHandling()
    {
        _colorSchemesPageViewModelChangedRevoker = _colorSchemesPageVM.PropertyChanged(winrt::auto_revoke, [=](auto&&, const PropertyChangedEventArgs& args) {
            const auto settingName{ args.PropertyName() };
            const auto boxedTag = box_value(colorSchemesTag);
            if (settingName == L"CurrentPage")
            {
                const auto currentScheme = _colorSchemesPageVM.CurrentScheme();
                if (_colorSchemesPageVM.CurrentPage() == ColorSchemesSubPage::EditColorScheme && currentScheme)
                {
                    contentFrame().Navigate(xaml_typename<Editor::EditColorScheme>(), winrt::make<NavigateToPageArgs>(currentScheme, *this));
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, currentScheme.Name(), BreadcrumbSubPage::ColorSchemes_Edit));
                }
                else if (_colorSchemesPageVM.CurrentPage() == ColorSchemesSubPage::Base)
                {
                    _Navigate(boxedTag, BreadcrumbSubPage::None);
                }
            }
            else if (settingName == L"CurrentSchemeName")
            {
                _breadcrumbs.RemoveAtEnd();
                _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, _colorSchemesPageVM.CurrentScheme().Name(), BreadcrumbSubPage::ColorSchemes_Edit));
            }
        });
    }

    void MainPage::_SetupActionsEventHandling()
    {
        _actionsViewModelChangedRevoker = _actionsVM.PropertyChanged(winrt::auto_revoke, [=](auto&&, const PropertyChangedEventArgs& args) {
            const auto settingName{ args.PropertyName() };
            if (settingName == L"CurrentPage")
            {
                const auto boxedTag = box_value(actionsTag);
                if (_actionsVM.CurrentPage() == ActionsSubPage::Edit)
                {
                    contentFrame().Navigate(xaml_typename<Editor::EditAction>(), winrt::make<NavigateToPageArgs>(_actionsVM.CurrentCommand(), *this));
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, RS_(L"Nav_EditAction/Content"), BreadcrumbSubPage::Actions_Edit));
                }
                else if (_actionsVM.CurrentPage() == ActionsSubPage::Base)
                {
                    _Navigate(boxedTag, BreadcrumbSubPage::None);
                }
            }
        });
    }

    // The Link Tooltip page swaps between its rules list and one rule's editor in
    // place, so there is no second XAML page to navigate to -- only a breadcrumb
    // to push and pop. Opening a rule appends "<rule>"; closing one (or deleting
    // the open rule) puts the page's own crumb back on its own.
    void MainPage::_SetupLinkTooltipEventHandling()
    {
        _linkTooltipViewModelChangedRevoker = _linkTooltipVM.PropertyChanged(winrt::auto_revoke, [this](auto&&, const PropertyChangedEventArgs& args) {
            const auto settingName{ args.PropertyName() };
            const auto boxedTag = box_value(linkTooltipTag);
            if (settingName == L"CurrentRule")
            {
                if (_linkTooltipVM.CurrentRule())
                {
                    if (_breadcrumbs.Size() == 1)
                    {
                        _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, _linkTooltipVM.CurrentRuleName(), BreadcrumbSubPage::LinkTooltip_Rule));
                    }
                    else if (_breadcrumbs.Size() > 1)
                    {
                        _breadcrumbs.RemoveAtEnd();
                        _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, _linkTooltipVM.CurrentRuleName(), BreadcrumbSubPage::LinkTooltip_Rule));
                    }
                    SettingsMainPage_ScrollViewer().ScrollToVerticalOffset(0);
                }
                else
                {
                    while (_breadcrumbs.Size() > 1)
                    {
                        _breadcrumbs.RemoveAtEnd();
                    }
                }
            }
            else if (settingName == L"CurrentRuleName" && _breadcrumbs.Size() > 1)
            {
                // Renaming the open rule has to reach the crumb that names it.
                _breadcrumbs.RemoveAtEnd();
                _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, _linkTooltipVM.CurrentRuleName(), BreadcrumbSubPage::LinkTooltip_Rule));
            }
        });
    }

    void MainPage::_SetupIntegrationsEventHandling()
    {
        _integrationsViewModelChangedRevoker = _integrationsVM.PropertyChanged(winrt::auto_revoke, [this](auto&&, const PropertyChangedEventArgs& args) {
            const auto settingName{ args.PropertyName() };
            const auto boxedTag = box_value(integrationsTag);
            if (settingName == L"CurrentIntegration")
            {
                if (const auto currentIntegration = _integrationsVM.CurrentIntegration())
                {
                    if (_breadcrumbs.Size() == 1)
                    {
                        _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, currentIntegration.Name(), BreadcrumbSubPage::Integrations_Integration));
                    }
                    else if (_breadcrumbs.Size() > 1)
                    {
                        _breadcrumbs.RemoveAtEnd();
                        _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedTag, currentIntegration.Name(), BreadcrumbSubPage::Integrations_Integration));
                    }
                    SettingsMainPage_ScrollViewer().ScrollToVerticalOffset(0);
                }
                else
                {
                    while (_breadcrumbs.Size() > 1)
                    {
                        _breadcrumbs.RemoveAtEnd();
                    }
                }
            }
        });
    }

    void MainPage::_NavigateToProfileSubPage(const Editor::ProfileViewModel& profile, ProfileSubPage page, const IInspectable& breadcrumbTag, const hstring& elementToFocus)
    {
        // Frame::Navigate returns false rather than throwing when the page cannot
        // be activated, and every branch here used to discard it. That is half of
        // why "Terminal Emulation does nothing" was so hard to pin down: a page
        // that refuses to load looked exactly like a click that never arrived,
        // and the breadcrumb was appended either way - so the trail claimed a
        // page the frame had never shown.
        //
        // Now the crumb is only appended if the navigation actually happened,
        // and a refusal says so once rather than failing silently. Uniform
        // across all four sub-pages: whichever one breaks next should not need
        // this diagnosed a second time.
        const auto navigate = [&](auto&& pageType, const hstring& crumbText, BreadcrumbSubPage crumb) {
            if (contentFrame().Navigate(pageType, winrt::make<NavigateToPageArgs>(profile, *this, elementToFocus)))
            {
                if (crumb != BreadcrumbSubPage::None)
                {
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(breadcrumbTag, crumbText, crumb));
                }
                return true;
            }

            OutputDebugStringW(fmt::format(FMT_COMPILE(L"[SettingsEditor] Navigate to profile sub-page {} was refused\n"),
                                           static_cast<int32_t>(page))
                                   .c_str());
            return false;
        };

        if (page == ProfileSubPage::Base)
        {
            navigate(xaml_typename<Editor::Profiles_Base>(), {}, BreadcrumbSubPage::None);
        }
        else if (page == ProfileSubPage::Appearance)
        {
            navigate(xaml_typename<Editor::Profiles_Appearance>(), RS_(L"Profile_Appearance/Header"), BreadcrumbSubPage::Profile_Appearance);
        }
        else if (page == ProfileSubPage::UnfocusedAppearance)
        {
            contentFrame().Navigate(xaml_typename<Editor::Profiles_UnfocusedAppearance>(), winrt::make<NavigateToPageArgs>(profile, *this, elementToFocus));
            _breadcrumbs.Append(winrt::make<Breadcrumb>(breadcrumbTag, RS_(L"Profile_UnfocusedAppearanceTextBlock/Text"), BreadcrumbSubPage::Profile_UnfocusedAppearance));
        }
        else if (page == ProfileSubPage::Terminal)
        {
            navigate(xaml_typename<Editor::Profiles_Terminal>(), RS_(L"Profile_Terminal/Header"), BreadcrumbSubPage::Profile_Terminal);
        }
        else if (page == ProfileSubPage::Advanced)
        {
            navigate(xaml_typename<Editor::Profiles_Advanced>(), RS_(L"Profile_Advanced/Header"), BreadcrumbSubPage::Profile_Advanced);
        }
        SettingsMainPage_ScrollViewer().ScrollToVerticalOffset(0);
    }

    void MainPage::_SetupProfileEventHandling(const Editor::ProfileViewModel profile)
    {
        // If this is the base layer, the breadcrumb tag should be the globalProfileTag instead of the
        // ProfileViewModel, because the navigation menu item for this profile is the globalProfileTag.
        // See MainPage::UpdateSettings for why this matters
        const auto breadcrumbTag = profile.IsBaseLayer() ? box_value(globalProfileTag) : box_value(profile);
        const auto breadcrumbText = profile.IsBaseLayer() ? RS_(L"Nav_ProfileDefaults/Content") : profile.Name();
        _profileViewModelChangedRevoker = profile.PropertyChanged(winrt::auto_revoke, [=](auto&&, const PropertyChangedEventArgs& args) {
            const auto settingName{ args.PropertyName() };
            if (settingName == L"CurrentPage")
            {
                const auto currentPage = profile.CurrentPage();
                if (currentPage == ProfileSubPage::Base)
                {
                    _breadcrumbs.Clear();
                    _AppendProfilesRootCrumb();
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(breadcrumbTag, breadcrumbText, BreadcrumbSubPage::None));
                }
                _NavigateToProfileSubPage(profile, currentPage, breadcrumbTag, {});
            }
        });
    }

    void MainPage::_LazyLoadProfileDefaultsViewModel()
    {
        if (!_profileDefaultsVM)
        {
            _profileDefaultsVM = _viewModelForProfile(_settingsClone.ProfileDefaults(), _settingsClone, _windowSettingsClone, Dispatcher());
            _profileDefaultsVM.SetupAppearances(_colorSchemesPageVM.AllColorSchemes());
            _profileDefaultsVM.IsBaseLayer(true);
        }
    }

    // Method Description:
    // - Navigates to the page corresponding to the given nav tag. Updates the breadcrumb bar and selected nav view item accordingly.
    // Arguments:
    // - vm: the nav tag of the page to navigate to. Can be either the hstring for static pages or
    //         a view model object (i.e. ProfileViewModel, ColorSchemeViewModel, etc.) for dynamic pages
    // - subPage: the sub page to navigate to, used for pages that have multiple sub pages (i.e. Profile > Appearance/Terminal/Advanced)
    // - elementToFocus: the name of the element to focus on the target page
    void MainPage::_Navigate(const IInspectable& vm, BreadcrumbSubPage subPage, hstring elementToFocus)
    {
        _PreNavigateHelper();

        // Re-sync from the settings clone on every navigation, so the flag is right
        // even if the Appearance toggle's own handler did not run, and so a page
        // built after the switch was flipped agrees with it. The clone is the
        // authority; the toggle handler is only there to make the change visible
        // without leaving the page.
        SettingsCard::ImprintEnabled(_settingsClone.GlobalSettings().AylithImprint());
        SettingsCard::JsonOnlyImprintEnabled(_settingsClone.GlobalSettings().AylithImprintJsonOnly());
        _UpdateForkNavItems();

        hstring selectedNavTag;

        if (const auto clickedItemTag = vm.try_as<hstring>())
        {
            selectedNavTag = *clickedItemTag;
            if (*clickedItemTag == launchTag)
            {
                contentFrame().Navigate(xaml_typename<Editor::Launch>(), winrt::make<NavigateToPageArgs>(winrt::make<LaunchViewModel>(_settingsClone), *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Launch/Content"), BreadcrumbSubPage::None));
            }
            else if (*clickedItemTag == sessionRestoreTag)
            {
                // A fresh view model every time, like Launch and Interaction above
                // and below. That is also what keeps this page and Startup agreeing
                // about "When Terminal starts", which they both show: whichever is
                // navigated to second is built after the other one's write, and
                // reads the value back off the same settings clone.
                contentFrame().Navigate(xaml_typename<Editor::SessionRestore>(), winrt::make<NavigateToPageArgs>(winrt::make<SessionRestoreViewModel>(_settingsClone), *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_SessionRestore/Content"), BreadcrumbSubPage::None));
            }
            else if (*clickedItemTag == interactionTag)
            {
                contentFrame().Navigate(xaml_typename<Editor::Interaction>(), winrt::make<NavigateToPageArgs>(winrt::make<InteractionViewModel>(_settingsClone.GlobalSettings(), _windowSettingsClone), *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Interaction/Content"), BreadcrumbSubPage::None));
            }
            else if (*clickedItemTag == linkTooltipTag)
            {
                if (_linkTooltipVM && _linkTooltipVM.CurrentRule())
                {
                    // Coming back from a rule to the list. The page is already up, so
                    // this only closes the rule -- but _PreNavigateHelper has just
                    // cleared the breadcrumbs and revoked our listener, and neither
                    // comes back on its own down this path. Re-add the crumb and
                    // re-register, in that order, so the change above doesn't
                    // re-enter the handler.
                    //
                    // Without this the page heading simply disappears, and the empty
                    // crumb trail is also what UpdateSettings() reads to decide where
                    // to return to -- so Discard changes would land on the first page
                    // in the list rather than back here.
                    _linkTooltipVM.CurrentRule(nullptr);
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_LinkTooltip/Content"), BreadcrumbSubPage::None));
                    _SetupLinkTooltipEventHandling();
                }
                else
                {
                    // A fresh VM every time: the rules it lists are read straight
                    // from the settings clone, and starting over is also what makes
                    // the first breadcrumb come back to the list rather than to
                    // whichever rule was last open.
                    _linkTooltipVM = winrt::make<LinkTooltipViewModel>(_settingsClone.GlobalSettings(), _windowSettingsClone);
                    _SetupLinkTooltipEventHandling();
                    contentFrame().Navigate(xaml_typename<Editor::LinkTooltip>(), winrt::make<NavigateToPageArgs>(_linkTooltipVM, *this, elementToFocus));
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_LinkTooltip/Content"), BreadcrumbSubPage::None));
                }
            }
            else if (*clickedItemTag == integrationsTag)
            {
                if (_integrationsVM && _integrationsVM.CurrentIntegration())
                {
                    // Same as the Link Tooltip case above: restore the crumb and the
                    // listener that _PreNavigateHelper cleared, since this path never
                    // re-navigates the frame.
                    _integrationsVM.CurrentIntegration(nullptr);
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Integrations/Content"), BreadcrumbSubPage::None));
                    _SetupIntegrationsEventHandling();
                }
                else
                {
                    // "Add as rule" on this page creates a rule that lives on the Link
                    // Tooltip page, so give the VM a way to send us there afterwards.
                    _integrationsVM = winrt::make<IntegrationsViewModel>(_settingsClone.GlobalSettings(), _windowSettingsClone);
                    _integrationsVM.NavigateToLinkTooltipRequested({ this, &MainPage::_NavigateToLinkTooltipHandler });
                    _SetupIntegrationsEventHandling();
                    contentFrame().Navigate(xaml_typename<Editor::Integrations>(), winrt::make<NavigateToPageArgs>(_integrationsVM, *this, elementToFocus));
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Integrations/Content"), BreadcrumbSubPage::None));
                }
            }
            else if (*clickedItemTag == compatibilityTag)
            {
                contentFrame().Navigate(xaml_typename<Editor::Compatibility>(), winrt::make<NavigateToPageArgs>(winrt::make<CompatibilityViewModel>(_settingsClone), *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Compatibility/Content"), BreadcrumbSubPage::None));
            }
            else if (*clickedItemTag == activityTag)
            {
                // A fresh view model per navigation, so the list is re-read
                // every time the page is opened. The whole point of the page is
                // "what just happened", and a cached one would answer "what had
                // happened when you first looked".
                contentFrame().Navigate(xaml_typename<Editor::Activity>(), winrt::make<NavigateToPageArgs>(winrt::make<ActivityViewModel>(_settingsClone), *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Activity/Content"), BreadcrumbSubPage::None));
            }
            else if (*clickedItemTag == actionsTag)
            {
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Actions/Content"), BreadcrumbSubPage::None));
                if (subPage == BreadcrumbSubPage::Actions_Edit && _actionsVM.CurrentCommand() != nullptr)
                {
                    // Suppress the handler to avoid double-navigation
                    _actionsViewModelChangedRevoker.revoke();

                    // Navigate directly to EditAction instead of relying on PropertyChanged,
                    // which won't fire if CurrentPage is already Edit
                    _actionsVM.CurrentPage(ActionsSubPage::Edit);
                    contentFrame().Navigate(xaml_typename<Editor::EditAction>(), winrt::make<NavigateToPageArgs>(_actionsVM.CurrentCommand(), *this, elementToFocus));
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_EditAction/Content"), BreadcrumbSubPage::Actions_Edit));

                    // Re-register the handler for future user-driven changes
                    _SetupActionsEventHandling();
                }
                else
                {
                    contentFrame().Navigate(xaml_typename<Editor::Actions>(), winrt::make<NavigateToPageArgs>(_actionsVM, *this, elementToFocus));
                    _actionsVM.CurrentPage(ActionsSubPage::Base);
                }
            }
            else if (*clickedItemTag == newTabMenuTag)
            {
                if (_newTabMenuPageVM.CurrentFolder())
                {
                    // Setting CurrentFolder triggers the PropertyChanged event,
                    // which will navigate to the correct page and update the breadcrumbs appropriately
                    _newTabMenuPageVM.CurrentFolder(nullptr);
                }
                else
                {
                    // Navigate to the NewTabMenu page
                    contentFrame().Navigate(xaml_typename<Editor::NewTabMenu>(), winrt::make<NavigateToPageArgs>(_newTabMenuPageVM, *this, elementToFocus));
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_NewTabMenu/Content"), BreadcrumbSubPage::None));
                }
            }
            else if (*clickedItemTag == extensionsTag)
            {
                if (_extensionsVM.CurrentExtensionPackage())
                {
                    // Setting CurrentExtensionPackage triggers the PropertyChanged event,
                    // which will navigate to the correct page and update the breadcrumbs appropriately
                    _extensionsVM.CurrentExtensionPackage(nullptr);
                }
                else
                {
                    contentFrame().Navigate(xaml_typename<Editor::Extensions>(), winrt::make<NavigateToPageArgs>(_extensionsVM, *this, elementToFocus));
                    _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Extensions/Content"), BreadcrumbSubPage::None));
                }
            }
            else if (*clickedItemTag == profilesTag)
            {
                contentFrame().Navigate(xaml_typename<Editor::Profiles>(), winrt::make<NavigateToPageArgs>(_profilesPageVM, *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Profiles/Content"), BreadcrumbSubPage::None));
            }
            else if (*clickedItemTag == globalProfileTag)
            {
                _AppendProfilesRootCrumb();

                _LazyLoadProfileDefaultsViewModel();

                // Set CurrentPage before registering the handler to avoid double-navigation
                const ProfileSubPage profileSubPage = ProfileSubPageFromBreadcrumb(subPage, _profileDefaultsVM);
                _profileDefaultsVM.CurrentPage(profileSubPage);

                // Navigate directly to the correct sub-page
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_ProfileDefaults/Content"), BreadcrumbSubPage::None));
                _NavigateToProfileSubPage(_profileDefaultsVM, profileSubPage, vm, elementToFocus);

                // Register handler for future user-driven sub-page changes
                _SetupProfileEventHandling(_profileDefaultsVM);

                // Keep the Profiles nav item selected.
                selectedNavTag = profilesTag;
            }
            else if (*clickedItemTag == colorSchemesTag)
            {
                _AppendProfilesRootCrumb();
                selectedNavTag = profilesTag;

                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_ColorSchemes/Content"), BreadcrumbSubPage::None));
                contentFrame().Navigate(xaml_typename<Editor::ColorSchemes>(), winrt::make<NavigateToPageArgs>(_colorSchemesPageVM, *this, elementToFocus));

                if (subPage == BreadcrumbSubPage::ColorSchemes_Edit)
                {
                    _colorSchemesPageVM.CurrentPage(ColorSchemesSubPage::EditColorScheme);
                }
            }
            else if (*clickedItemTag == globalAppearanceTag)
            {
                contentFrame().Navigate(xaml_typename<Editor::GlobalAppearance>(), winrt::make<NavigateToPageArgs>(winrt::make<GlobalAppearanceViewModel>(_settingsClone.GlobalSettings(), _windowSettingsClone), *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Appearance/Content"), BreadcrumbSubPage::None));
            }
            else if (*clickedItemTag == documentationTag)
            {
                // No view model: the page edits nothing, so there is no clone to hand
                // it. elementToFocus doubles as the topic to open -- a search result
                // or a "Learn more" link names a topic card, and the page opens that
                // topic rather than merely scrolling to it.
                //
                // No sub-page handling either, deliberately. Opening a topic appends a
                // crumb from inside the page; clicking the "Documentation" crumb comes
                // straight back here and navigates a fresh page, which is the topic
                // list. That is the whole of its back navigation.
                contentFrame().Navigate(xaml_typename<Editor::Documentation>(), winrt::make<NavigateToPageArgs>(nullptr, *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, RS_(L"Nav_Documentation/Content"), BreadcrumbSubPage::None));
            }
        }
        else if (const auto& profile = vm.try_as<Editor::ProfileViewModel>())
        {
            _AppendProfilesRootCrumb();
            selectedNavTag = profilesTag;

            // Setting CurrentPage below is meant not to navigate - the handler is
            // registered afterwards for exactly that reason. But the revoker is a
            // single member that _SetupProfileEventHandling only replaces at the
            // END of this block, so until then the handler from the PREVIOUS visit
            // to a profile is still subscribed, and it fires on these
            // assignments: it re-navigates and appends a second copy of the
            // crumbs, using the tag and name it captured last time. Revoking up
            // front is what actually makes the comment below true.
            _profileViewModelChangedRevoker.revoke();

            if (profile.Orphaned())
            {
                contentFrame().Navigate(xaml_typename<Editor::Profiles_Base_Orphaned>(), winrt::make<NavigateToPageArgs>(profile, *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, profile.Name(), BreadcrumbSubPage::None));
                profile.CurrentPage(ProfileSubPage::Base);
                _SetupProfileEventHandling(profile);
            }
            else
            {
                // Set CurrentPage before registering the handler to avoid double-navigation
                const ProfileSubPage profileSubPage = ProfileSubPageFromBreadcrumb(subPage, profile);
                profile.CurrentPage(profileSubPage);

                // Navigate directly to the correct sub-page
                _breadcrumbs.Append(winrt::make<Breadcrumb>(vm, profile.Name(), BreadcrumbSubPage::None));
                _NavigateToProfileSubPage(profile, profileSubPage, vm, elementToFocus);

                // Register handler for future user-driven sub-page changes
                _SetupProfileEventHandling(profile);
            }
        }
        else if (const auto& colorSchemeVM = vm.try_as<Editor::ColorSchemeViewModel>())
        {
            const auto boxedColorSchemesTag = box_value(colorSchemesTag);

            // Suppress the handler to avoid double-navigation
            _colorSchemesPageViewModelChangedRevoker.revoke();

            _AppendProfilesRootCrumb();
            selectedNavTag = profilesTag;

            _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedColorSchemesTag, RS_(L"Nav_ColorSchemes/Content"), BreadcrumbSubPage::None));

            if (subPage == BreadcrumbSubPage::None)
            {
                contentFrame().Navigate(xaml_typename<Editor::ColorSchemes>(), winrt::make<NavigateToPageArgs>(_colorSchemesPageVM, *this, elementToFocus));
                _colorSchemesPageVM.CurrentScheme(nullptr);
                _colorSchemesPageVM.CurrentPage(ColorSchemesSubPage::Base);
            }
            else
            {
                _colorSchemesPageVM.CurrentScheme(colorSchemeVM);
                _colorSchemesPageVM.CurrentPage(ColorSchemesSubPage::EditColorScheme);
                contentFrame().Navigate(xaml_typename<Editor::EditColorScheme>(), winrt::make<NavigateToPageArgs>(colorSchemeVM, *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedColorSchemesTag, colorSchemeVM.Name(), BreadcrumbSubPage::ColorSchemes_Edit));
            }

            // Re-register the handler for future user-driven changes
            _SetupColorSchemesEventHandling();
        }
        else if (const auto& ntmEntryVM = vm.try_as<Editor::NewTabMenuEntryViewModel>())
        {
            selectedNavTag = newTabMenuTag;

            contentFrame().Navigate(xaml_typename<Editor::NewTabMenu>(), winrt::make<NavigateToPageArgs>(_newTabMenuPageVM, *this, elementToFocus));
            _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(newTabMenuTag), RS_(L"Nav_NewTabMenu/Content"), BreadcrumbSubPage::None));

            if (subPage == BreadcrumbSubPage::None)
            {
                _newTabMenuPageVM.CurrentFolder(nullptr);
            }
            else if (const auto& folderEntryVM = ntmEntryVM.try_as<Editor::FolderEntryViewModel>(); subPage == BreadcrumbSubPage::NewTabMenu_Folder && folderEntryVM)
            {
                // The given ntmEntryVM doesn't exist anymore since the whole tree had to be recreated.
                // Instead, let's look for a match by name and navigate to it.
                if (const auto& folderPath = _newTabMenuPageVM.FindFolderPathByName(folderEntryVM.Name()); folderPath.Size() > 0)
                {
                    for (const auto& step : folderPath)
                    {
                        // Take advantage of the PropertyChanged event to navigate
                        // to the correct folder and build the breadcrumbs as we go
                        _newTabMenuPageVM.CurrentFolder(step);
                    }
                }
                else
                {
                    // If we couldn't find a reasonable match, just go back to the root
                    _newTabMenuPageVM.CurrentFolder(nullptr);
                }
            }
        }
        else if (const auto& extPkgVM = vm.try_as<Editor::ExtensionPackageViewModel>())
        {
            selectedNavTag = extensionsTag;

            contentFrame().Navigate(xaml_typename<Editor::Extensions>(), winrt::make<NavigateToPageArgs>(_extensionsVM, *this, elementToFocus));
            _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(extensionsTag), RS_(L"Nav_Extensions/Content"), BreadcrumbSubPage::None));

            if (subPage == BreadcrumbSubPage::None)
            {
                _extensionsVM.CurrentExtensionPackage(nullptr);
            }
            else
            {
                bool found = false;
                for (const auto& pkgVM : _extensionsVM.ExtensionPackages())
                {
                    if (pkgVM.Package().Source() == extPkgVM.Package().Source())
                    {
                        // Take advantage of the PropertyChanged event to navigate
                        // to the correct extension package and build the breadcrumbs as we go.
                        const auto wasAlreadyOnExtension = (_extensionsVM.CurrentExtensionPackage() == pkgVM);
                        _extensionsVM.CurrentExtensionPackage(pkgVM);
                        found = true;

                        // If CurrentExtensionPackage was already this extension, PropertyChanged won't fire,
                        // so we add the breadcrumb manually.
                        if (wasAlreadyOnExtension)
                        {
                            _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(pkgVM), pkgVM.DisplayName(), BreadcrumbSubPage::Extensions_Extension));
                        }
                        break;
                    }
                }
                if (!found)
                {
                    // If we couldn't find a reasonable match, just go back to the root
                    _extensionsVM.CurrentExtensionPackage(nullptr);
                }
            }
        }
        else if (const auto& commandVM = vm.try_as<Editor::CommandViewModel>())
        {
            selectedNavTag = actionsTag;
            const auto boxedActionsTag = box_value(actionsTag);

            _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedActionsTag, RS_(L"Nav_Actions/Content"), BreadcrumbSubPage::None));

            if (subPage == BreadcrumbSubPage::None || !commandVM)
            {
                contentFrame().Navigate(xaml_typename<Editor::Actions>(), winrt::make<NavigateToPageArgs>(_actionsVM, *this, elementToFocus));
                _actionsVM.CurrentCommand(nullptr);
            }
            else
            {
                // Suppress the handler to avoid double-navigation
                _actionsViewModelChangedRevoker.revoke();

                _actionsVM.CurrentCommand(commandVM);
                _actionsVM.CurrentPage(ActionsSubPage::Edit);
                contentFrame().Navigate(xaml_typename<Editor::EditAction>(), winrt::make<NavigateToPageArgs>(commandVM, *this, elementToFocus));
                _breadcrumbs.Append(winrt::make<Breadcrumb>(boxedActionsTag, RS_(L"Nav_EditAction/Content"), BreadcrumbSubPage::Actions_Edit));

                // Re-register the handler for future user-driven changes
                _SetupActionsEventHandling();
            }
        }

        // Select the appropriate nav item
        if (!selectedNavTag.empty())
        {
            _SelectNavItemByTag(selectedNavTag);
        }

        // The clone is brand new, so it matches the file again: this is the baseline
        // for both a fresh open and a Discard.
        _RecordCleanState();
    }

    void MainPage::SaveButton_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        _settingsClone.LogSettingChanges(false);
        if (!_settingsClone.WriteSettingsToDisk())
        {
            ShowLoadWarningsDialog.raise(*this, _settingsClone.Warnings());
            return;
        }

        // Grey the buttons out now rather than waiting for the reload this write sets
        // off to come back around through UpdateSettings, which is debounced and would
        // leave them live for a beat after a successful save.
        _RecordCleanState();
    }

    // Link Tooltip and Integrations are whole pages this fork added, not settings
    // scattered among upstream's. Marking every row on them would be noise, so the
    // mark goes on the nav entry instead and says it once.
    //
    // The glyph is appended to the localized label rather than drawn as a separate
    // element, because a NavigationViewItem's Content is the label and giving it a
    // panel would cost the item its automation name.
    void MainPage::_UpdateForkNavItems()
    {
        // From resources rather than a literal here: the glyph then lives in a file
        // that is unambiguously UTF-8, instead of depending on this translation
        // unit's source charset, and a translator can drop it for a locale where a
        // trailing diamond reads badly. Held in a named hstring, not viewed straight
        // off the RS_ temporary, which would be dangling by the next statement.
        const auto imprintText = RS_(L"Nav_ForkImprintSuffix");
        const std::wstring_view imprint{ imprintText };
        if (imprint.empty())
        {
            return;
        }

        const auto enabled = SettingsCard::ImprintEnabled();

        for (const auto& item : { SessionRestoreNavItem(), LinkTooltipNavItem(), IntegrationsNavItem() })
        {
            if (!item)
            {
                continue;
            }
            const auto content = item.Content().try_as<hstring>();
            if (!content)
            {
                continue;
            }

            std::wstring label{ *content };
            const auto marked = label.size() > imprint.size() &&
                                label.compare(label.size() - imprint.size(), imprint.size(), imprint) == 0;
            if (enabled && !marked)
            {
                label.append(imprint);
            }
            else if (!enabled && marked)
            {
                label.resize(label.size() - imprint.size());
            }
            else
            {
                continue;
            }
            item.Content(box_value(hstring{ label }));
        }
    }

    void MainPage::ResetButton_Click(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        UpdateSettings(_settingsSource, _windowSettingsSource);
    }

    // Both editor-chrome switches, read back from where they were persisted. They are
    // in ApplicationState rather than settings.json because they describe how the
    // editor presents itself, and because a global setting would live on the clone --
    // which would make turning either of them on count as an unsaved change, and both
    // of them exist to interact with the Save button.
    void MainPage::_ReadEditorChromePreferences()
    {
        const auto state = Model::ApplicationState::SharedInstance();

        // Assigning IsOn raises Toggled, which would write the value straight back to
        // ApplicationState. Harmless but pointless, and it makes the handlers look
        // like they can run before the page has a baseline.
        _applyingChromePreferences = true;
        const auto showDescriptions = state.SettingsShowDescriptions();
        ShowDescriptionsSwitch().IsOn(showDescriptions);
        _autoSave = state.SettingsAutoSave();
        AutoSaveSwitch().IsOn(_autoSave);
        _applyingChromePreferences = false;

        // Seed the cards before anything navigates, so the first page drawn already
        // agrees with the switch -- the same reason the provenance marks are seeded in
        // UpdateSettings.
        SettingsCard::DescriptionsVisible(showDescriptions);
    }

    void MainPage::ShowDescriptionsSwitch_Toggled(const IInspectable& sender, const RoutedEventArgs& /*args*/)
    {
        if (_applyingChromePreferences)
        {
            return;
        }

        // Read the switch rather than any projected property: Toggled and a binding's
        // write-back are not ordered against each other, so anything read through the
        // binding here can still be reporting the old value. Same trap as
        // CompatibilityViewModel::AylithImprintToggled.
        const auto toggle = sender.try_as<ToggleSwitch>();
        if (!toggle)
        {
            return;
        }

        const auto isOn = toggle.IsOn();
        SettingsCard::DescriptionsVisible(isOn);
        Model::ApplicationState::SharedInstance().SettingsShowDescriptions(isOn);
    }

    void MainPage::AutoSaveSwitch_Toggled(const IInspectable& sender, const RoutedEventArgs& /*args*/)
    {
        if (_applyingChromePreferences)
        {
            return;
        }

        const auto toggle = sender.try_as<ToggleSwitch>();
        if (!toggle)
        {
            return;
        }

        _autoSave = toggle.IsOn();
        Model::ApplicationState::SharedInstance().SettingsAutoSave(_autoSave);

        // Turning it on has to flush whatever is already pending, or the changes made
        // before the switch was flipped would sit unsaved behind a disabled Save
        // button. Turning it off only has to re-enable the buttons, which
        // _ReevaluateDirtyState does on its way through.
        _ReevaluateDirtyState();
    }

    // True if this reload is settings.json coming back to us from one of our own
    // auto-save writes. See the comment at the top of UpdateSettings for why that
    // matters and why this is kept this narrow.
    bool MainPage::_AbsorbOwnAutoSaveReload(const Model::CascadiaSettings& settings)
    {
        if (_autoSaveWrittenHashes.empty())
        {
            return false;
        }

        const auto hash = settings.Hash();
        const auto it = std::ranges::find(_autoSaveWrittenHashes, hash);
        if (it == _autoSaveWrittenHashes.end())
        {
            return false;
        }

        // Drop this echo and anything older. Several writes can be in flight at once --
        // ReloadSettingsThrottled debounces, so a burst of auto-saves can produce fewer
        // reloads than writes -- which is the whole reason this is a list and not one
        // hash.
        _autoSaveWrittenHashes.erase(_autoSaveWrittenHashes.begin(), it + 1);
        return true;
    }

    void MainPage::_RecordCleanState()
    {
        _cleanFingerprint = _settingsClone.SerializedFingerprint();
        _cleanSerialized = _settingsClone.SerializedSettings();
        _unsavedChanges = false;
        _ApplySaveButtonState();
        _DescribeUnsavedChanges(_cleanFingerprint);
    }

    // Mullion: hovering Save or Discard lists what each would change, so neither is
    // pressed blind. Clean, they go back to their ordinary tooltips.
    void MainPage::_DescribeUnsavedChanges(const winrt::hstring& fingerprint)
    {
        // Keyed on the state too: a save turns the dirty fingerprint into the clean
        // one without changing it, and the list must still go.
        const auto key = winrt::hstring{ std::wstring{ fingerprint } + (_unsavedChanges ? L"*" : L"") };
        if (key == _describedFingerprint)
        {
            return;
        }
        _describedFingerprint = key;

        const auto saveTip = RS_(L"Settings_SaveSettingsButton/[using:Windows.UI.Xaml.Controls]ToolTipService/ToolTip");
        const auto discardTip = RS_(L"Settings_ResetSettingsButton/[using:Windows.UI.Xaml.Controls]ToolTipService/ToolTip");
        if (!_unsavedChanges || _cleanSerialized.empty())
        {
            WUX::Controls::ToolTipService::SetToolTip(SaveButton(), box_value(saveTip));
            WUX::Controls::ToolTipService::SetToolTip(ResetButton(), box_value(discardTip));
            return;
        }

        std::wstring list;
        for (const auto& line : _settingsClone.DescribeChangesFrom(_cleanSerialized, 20))
        {
            list.append(L"\n• ").append(line);
        }
        if (list.empty())
        {
            // The fingerprint moved but the documents compare equal as JSON -- a
            // formatting-only difference. Say nothing rather than list nothing.
            WUX::Controls::ToolTipService::SetToolTip(SaveButton(), box_value(saveTip));
            WUX::Controls::ToolTipService::SetToolTip(ResetButton(), box_value(discardTip));
            return;
        }
        WUX::Controls::ToolTipService::SetToolTip(SaveButton(), box_value(winrt::hstring{ std::wstring{ saveTip } + list }));
        WUX::Controls::ToolTipService::SetToolTip(ResetButton(), box_value(winrt::hstring{ std::wstring{ discardTip } + list }));
    }

    void MainPage::_ApplySaveButtonState()
    {
        // With auto-save on there is by definition never anything to save or discard,
        // so both buttons stay down rather than being hidden: the row they sit in is
        // where the auto-save switch lives, and a row that changes shape when you flip
        // the switch beside it reads as a glitch.
        const auto enabled = !_autoSave && _unsavedChanges;
        SaveButton().IsEnabled(enabled);
        ResetButton().IsEnabled(enabled);
    }

    void MainPage::OnAnyViewModelChanged()
    {
        if (_dirtyCheckThrottle)
        {
            _dirtyCheckThrottle->Run();
        }
    }

    // The one place that decides whether there is anything to save, and the one place
    // auto-save writes from.
    //
    // The verdict is always a fresh fingerprint compared against the one taken when
    // the clone last matched the file. Nothing here trusts *which* notification got us
    // here: a view model raises PropertyChanged for pure UI state as readily as for a
    // setting, and an enum dropdown writes through to the settings model and raises
    // nothing at all.
    void MainPage::_ReevaluateDirtyState()
    {
        const auto fingerprint = _settingsClone.SerializedFingerprint();
        if (fingerprint.empty())
        {
            // Serialization failed. Leave the previous verdict standing -- an empty
            // fingerprint is "cannot tell", and treating it as "unchanged" would grey
            // out Save over real edits.
            return;
        }

        auto dirty = _cleanFingerprint.empty() || fingerprint != _cleanFingerprint;

        if (dirty && _autoSave)
        {
            _settingsClone.LogSettingChanges(false);
            if (_settingsClone.WriteSettingsToDisk())
            {
                _autoSaveWrittenHashes.emplace_back(_settingsClone.Hash());
                // Bounded because a reload is not guaranteed for every write; without
                // this the list would grow for as long as the window stays open.
                if (_autoSaveWrittenHashes.size() > MaxTrackedAutoSaveWrites)
                {
                    _autoSaveWrittenHashes.erase(_autoSaveWrittenHashes.begin());
                }
                _cleanFingerprint = fingerprint;
                dirty = false;
            }
            else
            {
                // Stop auto-saving rather than retrying on the next tick. A write that
                // fails tends to keep failing (the file is locked, the disk is full),
                // and a retry loop would append a warning per attempt and never tell
                // anyone. Switching off hands the user back the Save button, which is
                // the path that does report it -- SaveButton_Click raises the warnings
                // dialog on the same failure.
                //
                // Nothing is raised from here. The only dialog available reports
                // settings *load* warnings, and a failed write usually adds none, so it
                // would come up either empty or describing something unrelated to what
                // just happened. Saying nothing and re-enabling the button the user
                // already knows is better than that; a real "could not write
                // settings.json" message would need a string and a surface of its own.
                _autoSave = false;
                _applyingChromePreferences = true;
                AutoSaveSwitch().IsOn(false);
                _applyingChromePreferences = false;
                Model::ApplicationState::SharedInstance().SettingsAutoSave(false);
            }
        }

        _unsavedChanges = dirty;
        _ApplySaveButtonState();
        _DescribeUnsavedChanges(dirty ? fingerprint : _cleanFingerprint);
    }

    void MainPage::BreadcrumbBar_ItemClicked(const Microsoft::UI::Xaml::Controls::BreadcrumbBar& /*sender*/, const Microsoft::UI::Xaml::Controls::BreadcrumbBarItemClickedEventArgs& args)
    {
        if (gsl::narrow_cast<uint32_t>(args.Index()) < (_breadcrumbs.Size() - 1))
        {
            const auto crumb = args.Item().as<Breadcrumb>();
            _Navigate(crumb->Tag(), crumb->SubPage());
        }
    }

    void MainPage::_InitializeProfilesList()
    {
        const auto& itemSource{ SettingsNav().MenuItemsSource() };
        if (!itemSource)
        {
            // There wasn't a MenuItemsSource set yet? The only way that's
            // possible is if we haven't used
            // _MoveXamlParsedNavItemsIntoItemSource to move the hardcoded menu
            // entries from XAML into our runtime menu item source. Do that now.

            _MoveXamlParsedNavItemsIntoItemSource();
        }

        // Populate the per-profile view models on the Profiles VM. The Profiles landing
        // page (and the search index) read this same list back through the VM.
        const auto& profileVMs = _profilesPageVM.Profiles();
        profileVMs.Clear();
        for (const auto& profile : _settingsClone.AllProfiles())
        {
            if (!profile.Deleted())
            {
                auto profileVM = _viewModelForProfile(profile, _settingsClone, _windowSettingsClone, Dispatcher());
                profileVM.SetupAppearances(_colorSchemesPageVM.AllColorSchemes());
                profileVM.DeleteProfileRequested({ this, &MainPage::_DeleteProfile });
                profileVMs.Append(profileVM);
            }
        }
    }

    // BODGY
    // Does the very wacky business of moving all our MenuItems that we
    // hardcoded in XAML into a runtime MenuItemsSource. We'll then use _that_
    // MenuItemsSource as the source for our nav view entries instead. This
    // lets us hardcode the initial entries in precompiled XAML, but then adjust
    // the items at runtime. Without using a MenuItemsSource, the NavView just
    // crashes when items are removed (see GH#13673)
    void MainPage::_MoveXamlParsedNavItemsIntoItemSource()
    {
        if (SettingsNav().MenuItemsSource())
        {
            // We've already copied over the original items to a source. We can
            // just skip this now.
            return;
        }

        auto menuItems{ SettingsNav().MenuItems() };
        _originalNumItems = menuItems.Size();
        // Remove all the existing items, and move them to a separate vector
        // that we'll use as a MenuItemsSource. By doing this, we avoid a WinUI
        // bug (MUX#6302) where modifying the NavView.Items() directly causes a
        // crash. By leaving these static entries in XAML, we maintain the
        // benefit of instantiating them from the XBF, rather than at runtime.
        //
        // --> Copy it into an STL vector to simplify our code and reduce COM overhead.
        auto original = std::vector<IInspectable>{ _originalNumItems, nullptr };
        menuItems.GetMany(0, original);

        _menuItemSource = winrt::single_threaded_observable_vector<IInspectable>(std::move(original));

        SettingsNav().MenuItemsSource(_menuItemSource);
    }

    void MainPage::_CreateAndNavigateToNewProfile(const Model::Profile& profile)
    {
        const auto newProfile{ profile ? profile : _settingsClone.CreateNewProfile() };
        const auto profileViewModel{ _viewModelForProfile(newProfile, _settingsClone, _windowSettingsClone, Dispatcher()) };
        profileViewModel.SetupAppearances(_colorSchemesPageVM.AllColorSchemes());
        profileViewModel.DeleteProfileRequested({ this, &MainPage::_DeleteProfile });

        _profilesPageVM.Profiles().Append(profileViewModel);

        // Select and navigate to the new profile
        _Navigate(profileViewModel);
    }

    void MainPage::_DeleteProfile(const IInspectable /*sender*/, const Editor::DeleteProfileEventArgs& args)
    {
        // Delete profile from settings model
        const auto guid{ args.ProfileGuid() };
        auto profileList{ _settingsClone.AllProfiles() };
        for (uint32_t i = 0; i < profileList.Size(); ++i)
        {
            if (profileList.GetAt(i).Guid() == guid)
            {
                profileList.RemoveAt(i);
                break;
            }
        }

        // Remove the profile VM
        const auto& profileVMs = _profilesPageVM.Profiles();
        for (uint32_t i = 0; i < profileVMs.Size(); ++i)
        {
            if (profileVMs.GetAt(i).OriginalProfileGuid() == guid)
            {
                profileVMs.RemoveAt(i);
                break;
            }
        }

        // Go back to the Profiles landing page
        _Navigate(box_value(profilesTag));
        SettingsMainPage_ScrollViewer().ChangeView(nullptr, 0.0, nullptr);
    }

    IObservableVector<IInspectable> MainPage::Breadcrumbs() noexcept
    {
        return _breadcrumbs;
    }

    void MainPage::_NavigateToProfileHandler(const IInspectable& /*sender*/, winrt::guid profileGuid)
    {
        if (const auto profileVM = _FindProfileViewModelByGuid(profileGuid))
        {
            _Navigate(profileVM);
        }
        // Silently fail if the profile wasn't found
    }

    Editor::ProfileViewModel MainPage::_FindProfileViewModelByGuid(winrt::guid profileGuid) const
    {
        for (const auto& profileVM : _profilesPageVM.Profiles())
        {
            if (profileVM.OriginalProfileGuid() == profileGuid)
            {
                return profileVM;
            }
        }
        return nullptr;
    }

    void MainPage::_NavigateToColorSchemeHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/)
    {
        _Navigate(box_value(hstring{ colorSchemesTag }), BreadcrumbSubPage::ColorSchemes_Edit);
    }

    // The Integrations page adds text-matching rules that are edited on the Link
    // Tooltip page; this is how it hands the user over to them.
    void MainPage::_NavigateToLinkTooltipHandler(const IInspectable& /*sender*/, const IInspectable& /*args*/)
    {
        _Navigate(box_value(hstring{ linkTooltipTag }));
    }

    // The one way into a specific page from outside the settings editor: the link
    // tooltip's "Matched by <rule>" line, asking to edit the rule it just named.
    //
    // Navigate first and select second, in that order, because _Navigate builds a
    // fresh LinkTooltipViewModel on the way in -- deliberately, so the page always
    // opens on the rules list rather than on whichever rule was last open. Selecting
    // beforehand would be undone by that. The nav item is selected explicitly
    // afterwards for the same reason _Navigate does it for a click: nothing else
    // will, since this did not come from the NavigationView.
    void MainPage::NavigateToLinkTooltipRule(int32_t ruleIndex, const winrt::hstring& ruleName)
    {
        _Navigate(box_value(hstring{ linkTooltipTag }));
        _SelectNavItemByTag(linkTooltipTag);

        if (_linkTooltipVM && ruleIndex >= 0)
        {
            // Leaves the user on the rules list when it comes back false -- the rule
            // may have been renamed or deleted since the tooltip named it, and the
            // list is a better answer than an unrelated rule opened for editing.
            _linkTooltipVM.SelectRule(ruleIndex, ruleName);
        }
    }

    void MainPage::NavigateToDocumentationTopic(const winrt::hstring& topicElementName)
    {
        _Navigate(box_value(hstring{ documentationTag }), BreadcrumbSubPage::None, topicElementName);
        _SelectNavItemByTag(documentationTag);
    }

    void MainPage::_AppendProfilesRootCrumb()
    {
        _breadcrumbs.Append(winrt::make<Breadcrumb>(box_value(profilesTag), RS_(L"Nav_Profiles/Content"), BreadcrumbSubPage::None));
    }

    void MainPage::_SelectNavItemByTag(std::wstring_view tag)
    {
        if (!_menuItemSource)
        {
            return;
        }
        for (auto&& menuItem : _menuItemSource)
        {
            if (const auto& navViewItem{ menuItem.try_as<MUX::Controls::NavigationViewItem>() })
            {
                if (const auto& itemTag{ navViewItem.Tag() })
                {
                    if (const auto& stringTag{ itemTag.try_as<hstring>() })
                    {
                        if (*stringTag == tag)
                        {
                            SettingsNav().SelectedItem(navViewItem);
                            return;
                        }
                    }
                }
            }
        }
    }

    void MainPage::_SetupProfilesPageEventHandling()
    {
        _profilesPageVM.OpenDefaultsRequested([this](const auto&, const auto&) {
            _Navigate(box_value(globalProfileTag));
        });
        _profilesPageVM.OpenColorSchemesRequested([this](const auto&, const auto&) {
            _Navigate(box_value(colorSchemesTag));
        });
        _profilesPageVM.AddProfileRequested([this](const auto&, const winrt::guid& sourceProfile) {
            _AddProfileHandler(sourceProfile);
        });
        _profilesPageVM.OpenProfileRequested([this](const auto&, const Editor::ProfileViewModel& profile) {
            _Navigate(profile);
        });
    }

    winrt::Windows::UI::Xaml::Media::Brush MainPage::BackgroundBrush()
    {
        return SettingsNav().Background();
    }

    // globals.motion, applied to the Settings UI.
    //
    // TerminalPage::_ApplyMotionPreference resolves the same setting for the
    // terminal window. This repeats the resolution rather than reading a shared
    // variable because the Settings UI is a different DLL: a header's inline
    // static would hand each module its own copy, which is exactly how the
    // activity log's enable flag came to be false in the module that needed it.
    // What crosses the boundary is Timeline::AllowDependentAnimations, which is
    // XAML's own process state.
    void MainPage::_ApplyMotionPreference()
    {
        auto enabled = true;
        switch (_settingsClone.GlobalSettings().Motion())
        {
        case Model::MotionPreference::Full:
            enabled = true;
            break;
        case Model::MotionPreference::Reduced:
            enabled = false;
            break;
        default:
            try
            {
                // disableAnimations is the older boolean this setting
                // supersedes; under "system" it still forces animations off.
                enabled = !_windowSettingsClone.DisableAnimations() &&
                          winrt::Windows::UI::ViewManagement::UISettings{}.AnimationsEnabled();
            }
            CATCH_LOG();
            break;
        }

        Media::Animation::Timeline::AllowDependentAnimations(enabled);

        // The drill-in page transition is the most visible motion in here, and
        // it is an independent animation, so AllowDependentAnimations does not
        // reach it.
        contentFrame().ContentTransitions(enabled ? _pageTransitions : nullptr);
    }

    // If the theme asks for Mica, then drop out our background, so that we
    // can have mica too.
    void MainPage::_UpdateBackgroundForMica()
    {
        // If we're in high contrast mode, don't override the theme.
        if (Windows::UI::ViewManagement::AccessibilitySettings accessibilitySettings; accessibilitySettings.HighContrast())
        {
            return;
        }

        bool isMicaAvailable = false;

        // Check to see if our hosting window supports Mica at all. We'll check
        // to see if the window has Mica enabled - if it does, then we can
        // assume that it supports Mica.
        //
        // We're doing this instead of checking if we're on Windows build 22621
        // or higher.
        if (_hostingHwnd.has_value())
        {
            int attribute = DWMSBT_NONE;
            const auto hr = DwmGetWindowAttribute(*_hostingHwnd, DWMWA_SYSTEMBACKDROP_TYPE, &attribute, sizeof(attribute));
            if (SUCCEEDED(hr))
            {
                isMicaAvailable = attribute == DWMSBT_MAINWINDOW;
            }
        }

        const auto theme = _settingsSource.GlobalSettings().CurrentTheme(_windowSettingsSource);
        const auto hasThemeForSettings{ theme.Settings() != nullptr };
        const auto appTheme = theme.RequestedTheme();
        const auto requestedTheme = (hasThemeForSettings) ? theme.Settings().RequestedTheme() : appTheme;

        RequestedTheme(requestedTheme);

        // GH#19927 - Theme the search box's internal popup so its
        // dropdown matches the app theme instead of the system theme
        _setThemeOnPopups(SettingsSearchBox(), requestedTheme);

        // Mica gets it's appearance from the app's theme, not necessarily the
        // Page's theme. In the case of dark app, light settings, mica will be a
        // dark color, and the text will also be dark, making the UI _very_ hard
        // to read. (and similarly in the inverse situation).
        //
        // To mitigate this, don't set the transparent background in the case
        // that our theme is different than the app's.
        const bool actuallyUseMica = isMicaAvailable && (appTheme == requestedTheme);

        const auto bgKey = (theme.Window() != nullptr && theme.Window().UseMica()) && actuallyUseMica ?
                               L"SettingsPageMicaBackground" :
                               L"SettingsPageBackground";

        // remember to use ThemeLookup to get the actual correct color for the
        // currently requested theme.
        if (const auto bgColor = ThemeLookup(Resources(), requestedTheme, winrt::box_value(bgKey)))
        {
            SettingsNav().Background(winrt::WUX::Media::SolidColorBrush(winrt::unbox_value<Windows::UI::Color>(bgColor)));
        }
    }

    safe_void_coroutine MainPage::SettingsSearchBox_TextChanged(const AutoSuggestBox& sender, const AutoSuggestBoxTextChangedEventArgs& args)
    {
        if (args.Reason() != AutoSuggestionBoxTextChangeReason::UserInput)
        {
            // Only respond to user input, not programmatic text changes
            co_return;
        }

        // remove leading spaces
        std::wstring queryW{ sender.Text() };
        const auto firstNonSpace{ queryW.find_first_not_of(L' ') };
        if (firstNonSpace == std::wstring::npos)
        {
            // Nothing to search for. Clear first, then let an emptied box fall back to
            // what was chosen before, which is what focusing an empty box offers too. A
            // box holding only spaces is not empty, so _ShowSearchHistory declines it and
            // the clear above stands.
            const auto& searchBox = SettingsSearchBox();
            searchBox.ItemsSource(nullptr);
            searchBox.IsSuggestionListOpen(false);
            _highlightedSearchResult = nullptr;
            _ShowSearchHistory();
            co_return;
        }

        const hstring sanitizedQuery{ queryW.substr(firstNonSpace) };
        if (sanitizedQuery.empty())
        {
            // empty query
            const auto& searchBox = SettingsSearchBox();
            searchBox.ItemsSource(nullptr);
            searchBox.IsSuggestionListOpen(false);
            _highlightedSearchResult = nullptr;
            co_return;
        }

        if (_currentSearch)
        {
            // a newer search has started, abandon this one
            _currentSearch.Cancel();
            co_return;
        }
        _currentSearch = SearchIndex::Instance().SearchAsync(sanitizedQuery,
                                                             _profilesPageVM.Profiles().GetView(),
                                                             get_self<implementation::NewTabMenuViewModel>(_newTabMenuPageVM)->FolderTreeFlatList().GetView(),
                                                             _colorSchemesPageVM.AllColorSchemes().GetView(),
                                                             _extensionsVM.ExtensionPackages().GetView(),
                                                             _actionsVM.CommandList().GetView());
        const auto results = co_await _currentSearch;
        _currentSearch = nullptr;

        // Update the UI with the results
        const auto& searchBox = SettingsSearchBox();
        _highlightedSearchResult = nullptr;
        searchBox.ItemsSource(results);
        searchBox.IsSuggestionListOpen(true);
    }

    void MainPage::_NavigateToSearchResult(const IInspectable& result)
    {
        const auto searchResult{ result.try_as<Editor::FilteredSearchResult>() };
        if (!searchResult)
        {
            return;
        }

        const auto searchResultImpl{ get_self<implementation::FilteredSearchResult>(searchResult) };
        if (searchResultImpl->IsNoResultsPlaceholder())
        {
            // don't navigate anywhere
            return;
        }

        // Navigate to the target page
        const auto& indexEntry{ searchResultImpl->SearchIndexEntry() };
        const auto navigationArg{ searchResultImpl->NavigationArg() };
        const auto subpage{ indexEntry.Entry->SubPage };
        const hstring elementToFocus{ indexEntry.Entry->ElementName };

        // User explicitly wants to see the unfocused appearance, so create it if it doesn't exist yet.
        if (subpage == BreadcrumbSubPage::Profile_UnfocusedAppearance)
        {
            if (const auto& profileVM{ navigationArg.try_as<Editor::ProfileViewModel>() })
            {
                profileVM.CreateUnfocusedAppearance();
            }
            else if (const auto& navTag{ navigationArg.try_as<hstring>() }; navTag && *navTag == globalProfileTag)
            {
                _LazyLoadProfileDefaultsViewModel();
                _profileDefaultsVM.CreateUnfocusedAppearance();
            }
        }

        // Reset the search box before navigating
        // LOAD-BEARING: closing the suggestion list moves focus back to the search box,
        // which would fight elementToFocus if we navigated first. Since Text() raises
        // TextChanged as a ProgrammaticChange (which is ignored), we have to drop the stale
        // results ourselves. Otherwise, the query button would reuse them on the next click.
        const auto& searchBox{ SettingsSearchBox() };
        searchBox.Text(L"");
        searchBox.ItemsSource(nullptr);
        searchBox.IsSuggestionListOpen(false);
        _highlightedSearchResult = nullptr;

        // Before navigating, not after: this is the only place that knows a result was
        // actually chosen rather than merely arrowed past, and _Navigate rebuilds
        // enough of the page that there is no reason to be holding anything across it.
        _RecordSearchHistory(indexEntry);

        _Navigate(navigationArg, subpage, elementToFocus);
    }

    void MainPage::SettingsSearchBox_QuerySubmitted(const AutoSuggestBox& sender, const AutoSuggestBoxQuerySubmittedEventArgs& args)
    {
        if (const auto& chosenSuggestion{ args.ChosenSuggestion() })
        {
            _NavigateToSearchResult(chosenSuggestion);
            return;
        }
        else if (_currentSearch)
        {
            // a search for the current query is still running, so the results are stale
            return;
        }
        else if (_highlightedSearchResult)
        {
            // navigate to the suggestion the user highlighted with the arrow keys
            _NavigateToSearchResult(_highlightedSearchResult);
            return;
        }
        else if (const auto& itemsSource{ sender.ItemsSource() })
        {
            if (const auto& results{ itemsSource.try_as<IObservableVector<IInspectable>>() }; results && results.Size() > 0)
            {
                // otherwise, navigate to the top result
                _NavigateToSearchResult(results.GetAt(0));
            }
        }
    }

    void MainPage::SettingsSearchBox_SuggestionChosen(const AutoSuggestBox&, const AutoSuggestBoxSuggestionChosenEventArgs& args)
    {
        // Don't navigate on arrow keys
        // Handle Enter/Click with QuerySubmitted() to instead
        // AutoSuggestBox will pass the chosen item to QuerySubmitted() via args.ChosenSuggestion().
        // Just record the highlighted suggestion so that the query button can navigate to it.
        _highlightedSearchResult = args.SelectedItem();
    }

    void MainPage::SettingsSearchBox_GotFocus(const IInspectable& /*sender*/, const RoutedEventArgs& /*args*/)
    {
        _ShowSearchHistory();
    }

    void MainPage::SettingsSearchBox_Tapped(const IInspectable& /*sender*/, const Windows::UI::Xaml::Input::TappedRoutedEventArgs& /*args*/)
    {
        _ShowSearchHistory();
    }

    // Reads the remembered keys out of ApplicationState. Null is what an untouched
    // state.json gives back, so it is a normal answer rather than a failure.
    static std::vector<hstring> _readSearchHistory()
    {
        std::vector<hstring> history;
        if (const auto stored = Model::ApplicationState::SharedInstance().SettingsSearchHistory())
        {
            history.reserve(stored.Size());
            for (const auto& name : stored)
            {
                history.emplace_back(name);
            }
        }
        return history;
    }

    // What the box offers while it is empty: the settings it has actually been used to
    // reach, newest first. Typing replaces them with live results, and emptying the box
    // brings them back (SettingsSearchBox_TextChanged).
    void MainPage::_ShowSearchHistory()
    {
        const auto& searchBox = SettingsSearchBox();
        if (!searchBox.Text().empty())
        {
            // Whatever is in the box is what the list should be about.
            return;
        }

        if (searchBox.IsSuggestionListOpen())
        {
            // Already open over an empty box, which can only be this list: TextChanged
            // closes it for anything else. Rebuilding the items here would reset the
            // selection out from under whoever is arrowing through them, and GotFocus
            // bubbles up out of the suggestions as well as out of the text box.
            return;
        }

        // A new list, so whatever was highlighted in the old one no longer applies.
        _highlightedSearchResult = nullptr;

        const auto results = SearchIndex::Instance().Recall(_readSearchHistory());
        if (results.Size() == 0)
        {
            // Nothing chosen yet, or the index has not finished loading. Either way, an
            // empty dropdown would be worse than none.
            searchBox.ItemsSource(nullptr);
            searchBox.IsSuggestionListOpen(false);
            return;
        }

        searchBox.ItemsSource(results);
        searchBox.IsSuggestionListOpen(true);
    }

    // Remembers a chosen setting, most recent first and only once each. SearchIndex
    // decides whether this result is one a stored key could rebuild and hands back the
    // key, or an empty string when it is not, so the cases it declines (a particular
    // profile, color scheme or New Tab Menu folder, whose navigation argument is a live
    // view model) simply leave no trace instead of leaving a broken entry.
    void MainPage::_RecordSearchHistory(const LocalizedIndexEntry& entry)
    {
        const auto key = SearchIndex::Instance().RecallKey(entry);
        if (key.empty())
        {
            return;
        }

        // Rebuilt rather than mutated in place, which gets newest-first, the de-duplication
        // and the cap in one pass: the chosen key leads, then everything else it was not.
        const auto previous = _readSearchHistory();
        std::vector<hstring> history;
        history.reserve(MaxSearchHistoryEntries);
        history.emplace_back(key);
        for (const auto& name : previous)
        {
            if (history.size() >= MaxSearchHistoryEntries)
            {
                break;
            }
            if (name != key)
            {
                history.emplace_back(name);
            }
        }

        Model::ApplicationState::SharedInstance().SettingsSearchHistory(single_threaded_vector<hstring>(std::move(history)));
    }

    safe_void_coroutine MainPage::_UpdateSearchIndex()
    {
        auto weakThis = get_weak();

        co_await winrt::resume_background();

        if (auto strongThis = weakThis.get())
        {
            if (strongThis->_currentSearch && strongThis->_currentSearch.Status() == AsyncStatus::Started)
            {
                strongThis->_currentSearch.Cancel();
            }
            SearchIndex::Instance().Reset();
        }
    }
}
