// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace ViewModelChangeHook
{
    // Called after any view model in the settings editor raises PropertyChanged.
    // MainPage points this at itself so it can re-check whether its settings clone
    // still matches what is on disk, which is what drives the Save button.
    //
    // A file-scope hook rather than subscribing to each view model: the editor builds
    // dozens of them, several nested inside a profile, and they come and go with every
    // navigation - so "subscribe to all of them" is a list that silently stops being
    // complete. There is exactly one MainPage and the editor is single-threaded UI
    // code, so one pointer is enough. It is set to a free function that resolves a
    // weak reference, so it cannot outlive the page it points at.
    //
    // This only ever means "something may have changed". VIEW_MODEL_OBSERVABLE_PROPERTY
    // is used for pure UI state too (the current folder, the selected sub-page), so the
    // verdict has to come from comparing the serialized settings, never from the fact
    // that this ran. It is also not sufficient on its own: a write that goes straight
    // through to the settings model raises nothing at all, which is why MainPage keeps
    // a slower backstop as well. GETSET_BINDABLE_ENUM_SETTING is the commonest such
    // write, so it calls Notify itself.
    inline void (*AnyViewModelChanged)(){ nullptr };

    inline void Notify()
    {
        if (AnyViewModelChanged)
        {
            AnyViewModelChanged();
        }
    }
}

template<typename T>
struct ViewModelHelper
{
public:
    winrt::event_token PropertyChanged(const ::winrt::Windows::UI::Xaml::Data::PropertyChangedEventHandler& handler)
    {
        return _propertyChangedHandlers.add(handler);
    }

    void PropertyChanged(const winrt::event_token& token)
    {
        _propertyChangedHandlers.remove(token);
    }

protected:
    void _NotifyChangeCore(const std::wstring_view name)
    {
        _propertyChangedHandlers(*static_cast<T*>(this), ::winrt::Windows::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        ViewModelChangeHook::Notify();
    }

    // template recursion base case: single dispatch
    void _NotifyChanges(const std::wstring_view name) { _NotifyChangeCore(name); }

    template<typename... Args>
    void _NotifyChanges(const std::wstring_view name, Args&&... more)
    {
        _NotifyChangeCore(name);
        _NotifyChanges(std::forward<Args>(more)...);
    }

protected:
    winrt::event<::winrt::Windows::UI::Xaml::Data::PropertyChangedEventHandler> _propertyChangedHandlers;
};

#define GETSET_OBSERVABLE_PROJECTED_SETTING(target, name) \
public:                                                   \
    auto name() const                                     \
    {                                                     \
        return target.name();                             \
    };                                                    \
    template<typename T>                                  \
    void name(const T& value)                             \
    {                                                     \
        if (target.name() != value)                       \
        {                                                 \
            target.name(value);                           \
            _NotifyChanges(L"Has" #name, L## #name);      \
        }                                                 \
    }

#define _BASE_OBSERVABLE_PROJECTED_SETTING(target, name) \
    GETSET_OBSERVABLE_PROJECTED_SETTING(target, name)    \
    bool Has##name() const                               \
    {                                                    \
        return target.Has##name();                       \
    }

// Defines a setting that reflects another object's same-named
// setting.
#define OBSERVABLE_PROJECTED_SETTING(target, name)   \
    _BASE_OBSERVABLE_PROJECTED_SETTING(target, name) \
    void Clear##name()                               \
    {                                                \
        const auto t = target;                       \
        const auto hadValue{ t.Has##name() };        \
        t.Clear##name();                             \
        if (hadValue)                                \
        {                                            \
            _NotifyChanges(L"Has" #name, L## #name); \
        }                                            \
    }                                                \
    auto name##OverrideSource() const                \
    {                                                \
        return target.name##OverrideSource();        \
    }

// Defines a setting that reflects another object's same-named
// setting, but which cannot be erased.
#define PERMANENT_OBSERVABLE_PROJECTED_SETTING(target, name) \
    _BASE_OBSERVABLE_PROJECTED_SETTING(target, name)

// Defines a basic observable property that uses the _NotifyChanges
// system from ViewModelHelper. This is very similar to WINRT_OBSERVABLE_PROPERTY
// except it leverages _NotifyChanges.
#define VIEW_MODEL_OBSERVABLE_PROPERTY(type, name, ...) \
public:                                                 \
    type name() const noexcept                          \
    {                                                   \
        return _##name;                                 \
    };                                                  \
    void name(const type& value)                        \
    {                                                   \
        if (_##name != value)                           \
        {                                               \
            _##name = value;                            \
            _NotifyChanges(L## #name);                  \
        }                                               \
    };                                                  \
                                                        \
private:                                                \
    type _##name{ __VA_ARGS__ };
