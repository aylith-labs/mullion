/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- WebEmbedHost.h

Abstract:
- An integration's embeddable page, hosted in a XAML element through a WebView2
  composition controller. See WebEmbedHost.idl for the contract.

- Why composition and not a child window: the Terminal's top-level window is
  WS_EX_NOREDIRECTIONBITMAP and its XAML island does not clip siblings, so a
  WebView2 *window* parented there loads its page and is never seen. As a visual in
  the element's own composition tree the page is drawn by XAML's compositor, clips
  and scrolls with its container, and sits under anything XAML draws above it. The
  price is input: XAML receives the pointer, and every event is forwarded here.

--*/
#pragma once

#include "WebEmbedHost.g.h"

// Neither is in the pch, and a forward-declared projection compiles until the first
// call. Composition for the visual, Hosting for ElementCompositionPreview.
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>

// See HyperlinkHtmlHost.h for why the warning level drops around this include.
#pragma warning(push, 3)
#include <WebView2.h>
#pragma warning(pop)

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct WebEmbedHost : WebEmbedHostT<WebEmbedHost>
    {
        WebEmbedHost(const Windows::UI::Xaml::FrameworkElement& host, uint64_t parentWindow);
        ~WebEmbedHost();

        static bool IsAvailable() noexcept;

        void Show(const winrt::hstring& url, bool dark);
        void Hide();
        void Close();

        til::typed_event<Control::WebEmbedHost, winrt::hstring> OpenLinkRequested;
        til::typed_event<Control::WebEmbedHost, winrt::Windows::Foundation::IInspectable> EscapePressed;

    private:
        winrt::weak_ref<Windows::UI::Xaml::FrameworkElement> _host;
        HWND _parent{ nullptr };
        Windows::UI::Composition::ContainerVisual _visual{ nullptr };

        winrt::com_ptr<ICoreWebView2Environment> _environment;
        winrt::com_ptr<ICoreWebView2CompositionController> _composition;
        winrt::com_ptr<ICoreWebView2Controller> _controller;
        winrt::com_ptr<ICoreWebView2> _webView;
        HCURSOR _cursor{ nullptr };

        // Handed to every asynchronous WebView2 callback: a completion that lands after
        // Close() or destruction does nothing. Everything runs on the UI thread.
        std::shared_ptr<bool> _alive{ std::make_shared<bool>(true) };
        bool _creating{ false };
        bool _visible{ false };
        bool _dark{ false };
        winrt::hstring _url;
        winrt::hstring _loadedUrl;
        std::wstring _origin;
        HWND _focusBeforeShow{ nullptr };
        bool _pointerCaptured{ false };

        winrt::Windows::UI::Xaml::FrameworkElement::SizeChanged_revoker _sizeChanged;
        winrt::Windows::UI::Xaml::UIElement::PointerMoved_revoker _pointerMoved;
        winrt::Windows::UI::Xaml::UIElement::PointerPressed_revoker _pointerPressed;
        winrt::Windows::UI::Xaml::UIElement::PointerReleased_revoker _pointerReleased;
        winrt::Windows::UI::Xaml::UIElement::PointerWheelChanged_revoker _pointerWheel;
        winrt::Windows::UI::Xaml::UIElement::PointerExited_revoker _pointerExited;
        winrt::Windows::UI::Xaml::UIElement::PointerCaptureLost_revoker _pointerCaptureLost;

        void _create();
        void _configure();
        void _navigate();
        void _applyBounds();
        double _scale() const;
        bool _sameOrigin(LPCWSTR uri) const;
        void _openElsewhere(LPCWSTR uri);
        void _forwardPointer(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& args, COREWEBVIEW2_MOUSE_EVENT_KIND kind, uint32_t mouseData);
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(WebEmbedHost);
}
