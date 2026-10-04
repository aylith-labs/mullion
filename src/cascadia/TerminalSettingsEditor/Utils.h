// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <winrt/Microsoft.Terminal.Settings.Editor.h>

// This macro must be used alongside GETSET_BINDABLE_ENUM_SETTING.
// Use this in your class's constructor after Initialize_Component().
// It sorts and initializes the observable list of enum entries with the enum name
// being its localized name, and also initializes the enum to EnumEntry
// map that's required to tell XAML what enum value the currently active
// setting has.
#define INITIALIZE_BINDABLE_ENUM_SETTING(name, enumMappingsName, enumType, resourceSectionAndType, resourceProperty)                                                                     \
    do                                                                                                                                                                                   \
    {                                                                                                                                                                                    \
        std::vector<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry> name##List;                                                                                                 \
        _##name##Map = winrt::single_threaded_map<enumType, winrt::Microsoft::Terminal::Settings::Editor::EnumEntry>();                                                                  \
        auto enumMapping##name = winrt::Microsoft::Terminal::Settings::Model::EnumMappings::enumMappingsName();                                                                          \
        for (auto [key, value] : enumMapping##name)                                                                                                                                      \
        {                                                                                                                                                                                \
            auto enumName = LocalizedNameForEnumName(resourceSectionAndType, key, resourceProperty);                                                                                     \
            auto entry = winrt::make<winrt::Microsoft::Terminal::Settings::Editor::implementation::EnumEntry>(enumName, winrt::box_value<enumType>(value), static_cast<int32_t>(value)); \
            name##List.emplace_back(entry);                                                                                                                                              \
            _##name##Map.Insert(value, entry);                                                                                                                                           \
        }                                                                                                                                                                                \
        std::sort(name##List.begin(), name##List.end(), EnumEntryComparator<enumType>());                                                                                                \
        _##name##List = winrt::single_threaded_observable_vector<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry>(std::move(name##List));                                        \
    } while (0);

#define INITIALIZE_BINDABLE_ENUM_SETTING_REVERSE_ORDER(name, enumMappingsName, enumType, resourceSectionAndType, resourceProperty)                                                       \
    do                                                                                                                                                                                   \
    {                                                                                                                                                                                    \
        std::vector<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry> name##List;                                                                                                 \
        _##name##Map = winrt::single_threaded_map<enumType, winrt::Microsoft::Terminal::Settings::Editor::EnumEntry>();                                                                  \
        auto enumMapping##name = winrt::Microsoft::Terminal::Settings::Model::EnumMappings::enumMappingsName();                                                                          \
        for (auto [key, value] : enumMapping##name)                                                                                                                                      \
        {                                                                                                                                                                                \
            auto enumName = LocalizedNameForEnumName(resourceSectionAndType, key, resourceProperty);                                                                                     \
            auto entry = winrt::make<winrt::Microsoft::Terminal::Settings::Editor::implementation::EnumEntry>(enumName, winrt::box_value<enumType>(value), static_cast<int32_t>(value)); \
            name##List.emplace_back(entry);                                                                                                                                              \
            _##name##Map.Insert(value, entry);                                                                                                                                           \
        }                                                                                                                                                                                \
        std::sort(name##List.begin(), name##List.end(), EnumEntryReverseComparator<enumType>());                                                                                         \
        _##name##List = winrt::single_threaded_observable_vector<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry>(std::move(name##List));                                        \
    } while (0);

// This macro must be used alongside INITIALIZE_BINDABLE_ENUM_SETTING.
// It declares the needed data structures, getters, and setters to make
// the given enum type bindable to XAML. It provides an observable list
// of EnumEntries so that we may display all possible values of the given
// enum type and its localized names. It also provides a getter and setter
// for the setting we wish to bind to.
// Current##name() checks HasKey before Lookup, and that guard is load-bearing.
//
// IMap::Lookup throws hresult_out_of_bounds when the key is absent, and XAML calls
// this getter while dispatching PropertyChanged and while re-realising virtualised
// rows on scroll. A throw there does not degrade to a failed binding: it escapes the
// callback and fails the process fast -- 0xC000041D,
// STATUS_FATAL_USER_CALLBACK_EXCEPTION, faulting in Windows.UI.Xaml.dll -- taking
// every window with it, not just the settings page.
//
// That was reproduced on the Link Tooltip page: change a dropdown, scroll the row
// back into view, and the whole Terminal disappears with no dialog. A combo box
// showing nothing selected is the right failure for a value the list does not have.
#define GETSET_BINDABLE_ENUM_SETTING(name, enumType, viewModelSettingGetSet)                                                             \
public:                                                                                                                                  \
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry> name##List()     \
    {                                                                                                                                    \
        return _##name##List;                                                                                                            \
    }                                                                                                                                    \
                                                                                                                                         \
    winrt::Windows::Foundation::IInspectable Current##name()                                                                             \
    {                                                                                                                                    \
        const auto key = viewModelSettingGetSet();                                                                                       \
        if (!_##name##Map || !_##name##Map.HasKey(key))                                                                                  \
        {                                                                                                                                \
            return nullptr;                                                                                                              \
        }                                                                                                                                \
        return winrt::box_value<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry>(_##name##Map.Lookup(key));                      \
    }                                                                                                                                    \
                                                                                                                                         \
    void Current##name(const winrt::Windows::Foundation::IInspectable& enumEntry)                                                        \
    {                                                                                                                                    \
        if (auto ee = enumEntry.try_as<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry>())                                       \
        {                                                                                                                                \
            auto setting = winrt::unbox_value<enumType>(ee.EnumValue());                                                                 \
            viewModelSettingGetSet(setting);                                                                                             \
        }                                                                                                                                \
    }                                                                                                                                    \
                                                                                                                                         \
private:                                                                                                                                 \
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Microsoft::Terminal::Settings::Editor::EnumEntry> _##name##List;   \
    winrt::Windows::Foundation::Collections::IMap<enumType, winrt::Microsoft::Terminal::Settings::Editor::EnumEntry> _##name##Map;

namespace winrt::Microsoft::Terminal::Settings
{
    winrt::hstring GetSelectedItemTag(const winrt::Windows::Foundation::IInspectable& comboBoxAsInspectable);
    winrt::hstring LocalizedNameForEnumName(const std::wstring_view sectionAndType, const std::wstring_view enumValue, const std::wstring_view propertyType);
    winrt::hstring ColorToHexString(const winrt::Windows::UI::Color& color);
    winrt::hstring FormatAccessibleName(const std::wstring_view headerResourceKey, const std::wstring_view value);
    safe_void_coroutine ExpandAncestorsAndBringIntoView(winrt::Windows::UI::Xaml::FrameworkElement root, winrt::Windows::UI::Xaml::Controls::Control control);
    // Sends the editor to one topic on the Documentation page, named by the x:Name of
    // its card. Three pages need this -- Shortcuts, Extensions and the Dropdown Menu,
    // whose "Learn more" links used to leave the app -- and each of them already holds
    // a weak IHostedInWindow, which is the MainPage. Does nothing if the window root
    // has gone away, which is the right answer for a link on a page being torn down.
    void OpenDocumentationTopic(const Editor::IHostedInWindow& windowRoot, const winrt::hstring& topicElementName);
    Editor::KeyChordListener FindKeyChordListener(const winrt::Windows::UI::Xaml::DependencyObject& root);
    winrt::Windows::UI::Xaml::Controls::Control FindFirstFocusable(const winrt::Windows::UI::Xaml::DependencyObject& root);
    winrt::Windows::UI::Xaml::Controls::Control ResolveFocusTarget(const winrt::Windows::UI::Xaml::Controls::Control& element);
}

// BODGY!
//
// The following function and struct are a workaround for GH#9320.
//
// DismissAllPopups can be used to dismiss all popups for a particular UI
// element. However, we've got a bunch of pages with scroll viewers that may or
// may not have popups in them. Rather than define the same exact body for all
// their ViewChanging events, the HasScrollViewer struct will just do it for
// you!
inline void DismissAllPopups(const winrt::Windows::UI::Xaml::XamlRoot& xamlRoot)
{
    const auto popups{ winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(xamlRoot) };
    for (const auto& p : popups)
    {
        p.IsOpen(false);
    }
}

template<typename T>
struct HasScrollViewer
{
    // When the ScrollViewer scrolls, dismiss any popups we might have.
    void ViewChanging(const winrt::Windows::Foundation::IInspectable& sender,
                      const winrt::Windows::UI::Xaml::Controls::ScrollViewerViewChangingEventArgs& /*e*/)
    {
        // Inside this struct, we can't get at the XamlRoot() that our subclass
        // implements. I mean, _we_ can, but when XAML does its code
        // generation, _XAML_ won't be able to figure it out.
        //
        // Fortunately for us, we don't need to! The sender is a UIElement, so
        // we can just get _their_ XamlRoot().
        if (const auto& uielem{ sender.try_as<winrt::Windows::UI::Xaml::UIElement>() })
        {
            DismissAllPopups(uielem.XamlRoot());
        }
    }

    // Finds the element with the given name and brings it into view
    void BringIntoViewWhenLoaded(const winrt::hstring elementName)
    {
        if (elementName.empty())
        {
            return;
        }

        auto* pThis = static_cast<T*>(this);
        _loadedRevoker = pThis->Loaded(winrt::auto_revoke, [weakThis{ pThis->get_weak() }, elementName](auto&&, auto&&) {
            if (auto page{ weakThis.get() })
            {
                if (const auto& controlToFocus{ page->FindName(elementName).try_as<winrt::Windows::UI::Xaml::Controls::Control>() })
                {
                    // If the named element is a SettingsExpander/SettingsCard with an
                    // interactive control in its Content, focus that inner control instead.
                    const auto& target{ winrt::Microsoft::Terminal::Settings::ResolveFocusTarget(controlToFocus) };

                    // We need to wait for the page to be loaded
                    // or else the call to StartBringIntoView()
                    // will end up doing nothing.
                    winrt::Microsoft::Terminal::Settings::ExpandAncestorsAndBringIntoView(page.template as<winrt::Windows::UI::Xaml::FrameworkElement>(), target);
                }
                page->_loadedRevoker.revoke();
            }
        });
    }

protected:
    winrt::Windows::UI::Xaml::FrameworkElement::Loaded_revoker _loadedRevoker;
};
