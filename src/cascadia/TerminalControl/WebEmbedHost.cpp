// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WebEmbedHost.h"
#include "HyperlinkHtmlHost.h"

#include "WebEmbedHost.g.cpp"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace WUX = winrt::Windows::UI::Xaml;

namespace
{
    // The smallest IUnknown that will carry a WebView2 callback. HyperlinkHtmlHost has
    // its own copies; these two templates cover every shape this file needs.
    template<typename I>
    struct ComCallback : I
    {
        ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++_refCount; }
        ULONG STDMETHODCALLTYPE Release() noexcept override
        {
            const auto remaining = --_refCount;
            if (remaining == 0)
            {
                delete this;
            }
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override
        {
            if (!ppvObject)
            {
                return E_POINTER;
            }
            if (riid == __uuidof(IUnknown) || riid == __uuidof(I))
            {
                *ppvObject = static_cast<I*>(this);
                AddRef();
                return S_OK;
            }
            *ppvObject = nullptr;
            return E_NOINTERFACE;
        }

    protected:
        virtual ~ComCallback() = default;

    private:
        std::atomic<ULONG> _refCount{ 1 };
    };

    // Invoke(HRESULT, Result*) -- the creation completions.
    template<typename I, typename Result>
    struct Completed final : ComCallback<I>
    {
        explicit Completed(std::function<void(HRESULT, Result*)> callback) :
            _callback{ std::move(callback) } {}
        HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, Result* result) noexcept override
        try
        {
            _callback(errorCode, result);
            return S_OK;
        }
        CATCH_RETURN()

    private:
        std::function<void(HRESULT, Result*)> _callback;
    };

    // Invoke(Sender*, Args*) -- the events.
    template<typename I, typename Sender, typename Args>
    struct Event final : ComCallback<I>
    {
        explicit Event(std::function<void(Args*)> callback) :
            _callback{ std::move(callback) } {}
        HRESULT STDMETHODCALLTYPE Invoke(Sender* /*sender*/, Args* args) noexcept override
        try
        {
            _callback(args);
            return S_OK;
        }
        CATCH_RETURN()

    private:
        std::function<void(Args*)> _callback;
    };

    template<typename T, typename... A>
    winrt::com_ptr<T> make_callback(A&&... args)
    {
        winrt::com_ptr<T> result;
        result.attach(new T{ std::forward<A>(args)... });
        return result;
    }

    using EnvironmentCompleted = Completed<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, ICoreWebView2Environment>;
    using CompositionCompleted = Completed<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler, ICoreWebView2CompositionController>;
    using NavigationStarting = Event<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2, ICoreWebView2NavigationStartingEventArgs>;
    using NewWindowRequested = Event<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2, ICoreWebView2NewWindowRequestedEventArgs>;
    using CertificateError = Event<ICoreWebView2ServerCertificateErrorDetectedEventHandler, ICoreWebView2, ICoreWebView2ServerCertificateErrorDetectedEventArgs>;
    using AcceleratorKey = Event<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller, ICoreWebView2AcceleratorKeyPressedEventArgs>;
    using CursorChanged = Event<ICoreWebView2CursorChangedEventHandler, ICoreWebView2CompositionController, IUnknown>;

    // scheme://host:port, lower-cased, or empty for anything Uri will not parse.
    std::wstring originOf(std::wstring_view uri)
    {
        try
        {
            const winrt::Windows::Foundation::Uri parsed{ winrt::hstring{ uri } };
            auto origin = fmt::format(L"{}://{}:{}", std::wstring_view{ parsed.SchemeName() }, std::wstring_view{ parsed.Host() }, parsed.Port());
            std::transform(origin.begin(), origin.end(), origin.begin(), ::towlower);
            return origin;
        }
        catch (...)
        {
            return {};
        }
    }
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    WebEmbedHost::WebEmbedHost(const FrameworkElement& host, uint64_t parentWindow) :
        _host{ host },
        _parent{ reinterpret_cast<HWND>(parentWindow) }
    {
        // A pane has no window handle of its own to give. Whatever holds focus on this
        // thread is inside the Terminal's window -- this runs from a click in it -- and
        // its root is the window keyboard focus has to come back to.
        if (!_parent)
        {
            if (const auto focused = ::GetFocus())
            {
                _parent = ::GetAncestor(focused, GA_ROOT);
            }
        }
        if (!_parent)
        {
            _parent = ::GetActiveWindow();
        }

        // The page is a visual of this element's own, sized with it.
        const auto compositor = ElementCompositionPreview::GetElementVisual(host).Compositor();
        _visual = compositor.CreateContainerVisual();
        _visual.RelativeSizeAdjustment({ 1.0f, 1.0f });
        ElementCompositionPreview::SetElementChildVisual(host, _visual);

        _sizeChanged = host.SizeChanged(winrt::auto_revoke, [this](auto&&, auto&&) { _applyBounds(); });

        // XAML gets the pointer, not the page: every event goes across by hand.
        _pointerMoved = host.PointerMoved(winrt::auto_revoke, [this](auto&&, const WUX::Input::PointerRoutedEventArgs& args) {
            _forwardPointer(args, COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE, 0);
            if (_cursor)
            {
                ::SetCursor(_cursor);
            }
        });
        _pointerPressed = host.PointerPressed(winrt::auto_revoke, [this](auto&&, const WUX::Input::PointerRoutedEventArgs& args) {
            const auto element = _host.get();
            if (!element)
            {
                return;
            }
            using winrt::Windows::UI::Input::PointerUpdateKind;
            const auto kind = args.GetCurrentPoint(element).Properties().PointerUpdateKind();
            auto event = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
            uint32_t data = 0;
            switch (kind)
            {
            case PointerUpdateKind::RightButtonPressed:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
                break;
            case PointerUpdateKind::MiddleButtonPressed:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
                break;
            case PointerUpdateKind::XButton1Pressed:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
                data = XBUTTON1;
                break;
            case PointerUpdateKind::XButton2Pressed:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
                data = XBUTTON2;
                break;
            default:
                break;
            }
            // A drag that leaves the element still belongs to the page.
            _pointerCaptured = element.CapturePointer(args.Pointer());
            // Keyboard input follows a click, as it would in a browser.
            if (_controller)
            {
                LOG_IF_FAILED(_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
            }
            _forwardPointer(args, event, data);
        });
        _pointerReleased = host.PointerReleased(winrt::auto_revoke, [this](auto&&, const WUX::Input::PointerRoutedEventArgs& args) {
            const auto element = _host.get();
            if (!element)
            {
                return;
            }
            using winrt::Windows::UI::Input::PointerUpdateKind;
            const auto kind = args.GetCurrentPoint(element).Properties().PointerUpdateKind();
            auto event = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
            uint32_t data = 0;
            switch (kind)
            {
            case PointerUpdateKind::RightButtonReleased:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
                break;
            case PointerUpdateKind::MiddleButtonReleased:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
                break;
            case PointerUpdateKind::XButton1Released:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
                data = XBUTTON1;
                break;
            case PointerUpdateKind::XButton2Released:
                event = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
                data = XBUTTON2;
                break;
            default:
                break;
            }
            _forwardPointer(args, event, data);
            if (_pointerCaptured)
            {
                element.ReleasePointerCapture(args.Pointer());
                _pointerCaptured = false;
            }
        });
        _pointerWheel = host.PointerWheelChanged(winrt::auto_revoke, [this](auto&&, const WUX::Input::PointerRoutedEventArgs& args) {
            const auto element = _host.get();
            if (!element)
            {
                return;
            }
            const auto properties = args.GetCurrentPoint(element).Properties();
            // The wheel delta travels as a signed WORD in an unsigned field, as it does
            // in WM_MOUSEWHEEL.
            const auto delta = static_cast<uint32_t>(static_cast<int32_t>(properties.MouseWheelDelta()));
            _forwardPointer(args, properties.IsHorizontalMouseWheel() ? COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL : COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL, delta);
        });
        _pointerExited = host.PointerExited(winrt::auto_revoke, [this](auto&&, const WUX::Input::PointerRoutedEventArgs& args) {
            if (!_pointerCaptured)
            {
                _forwardPointer(args, COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, 0);
            }
        });
        _pointerCaptureLost = host.PointerCaptureLost(winrt::auto_revoke, [this](auto&&, auto&&) {
            _pointerCaptured = false;
        });
    }

    WebEmbedHost::~WebEmbedHost()
    {
        *_alive = false;
        if (_controller)
        {
            LOG_IF_FAILED(_controller->Close());
        }
    }

    bool WebEmbedHost::IsAvailable() noexcept
    {
        return HyperlinkHtmlHost::IsAvailable();
    }

    void WebEmbedHost::Show(const winrt::hstring& url, bool dark)
    {
        if (url.empty() || !*_alive)
        {
            return;
        }

        _url = url;
        _origin = originOf(url);
        _dark = dark;
        _visible = true;
        _focusBeforeShow = ::GetFocus();

        if (_controller)
        {
            LOG_IF_FAILED(_controller->put_IsVisible(TRUE));
            _applyBounds();
            _navigate();
            return;
        }
        _create();
    }

    void WebEmbedHost::Hide()
    {
        _visible = false;

        // A page that was clicked into holds Win32 keyboard focus, and hiding it does not
        // move it: the terminal would stop getting keys with nothing on screen to say
        // why. Taken back only from a window inside our own, never from another app.
        const auto focused = ::GetFocus();
        const auto restoreFocus = _focusBeforeShow && focused && focused != _focusBeforeShow &&
                                  ::IsWindow(_focusBeforeShow) && _parent &&
                                  (focused == _parent || ::IsChild(_parent, focused));

        if (_controller)
        {
            LOG_IF_FAILED(_controller->put_IsVisible(FALSE));
        }
        if (restoreFocus)
        {
            ::SetFocus(_focusBeforeShow);
        }
        _focusBeforeShow = nullptr;
    }

    void WebEmbedHost::Close()
    {
        Hide();
        *_alive = false;
        _sizeChanged.revoke();
        _pointerMoved.revoke();
        _pointerPressed.revoke();
        _pointerReleased.revoke();
        _pointerWheel.revoke();
        _pointerExited.revoke();
        _pointerCaptureLost.revoke();
        if (_controller)
        {
            LOG_IF_FAILED(_controller->Close());
        }
        _webView = nullptr;
        _controller = nullptr;
        _composition = nullptr;
        _environment = nullptr;
        if (const auto host = _host.get())
        {
            ElementCompositionPreview::SetElementChildVisual(host, nullptr);
        }
        _visual = nullptr;
    }

    void WebEmbedHost::_create()
    {
        if (_creating || !_parent)
        {
            return;
        }
        _creating = true;

        const auto failed = [this](HRESULT hr) {
            LOG_HR(hr);
            _creating = false;
        };

        auto onComposition = make_callback<CompositionCompleted>([alive = _alive, this, failed](HRESULT hr, ICoreWebView2CompositionController* composition) {
            if (!*alive)
            {
                return;
            }
            if (FAILED(hr) || !composition)
            {
                failed(FAILED(hr) ? hr : E_POINTER);
                return;
            }
            _creating = false;
            _composition.copy_from(composition);
            _controller = _composition.try_as<ICoreWebView2Controller>();
            _configure();
        });

        auto onEnvironment = make_callback<EnvironmentCompleted>([alive = _alive, this, failed, onComposition](HRESULT hr, ICoreWebView2Environment* environment) {
            if (!*alive)
            {
                return;
            }
            if (FAILED(hr) || !environment)
            {
                failed(FAILED(hr) ? hr : E_POINTER);
                return;
            }
            _environment.copy_from(environment);
            const auto environment3 = _environment.try_as<ICoreWebView2Environment3>();
            if (!environment3)
            {
                failed(E_NOINTERFACE);
                return;
            }
            const auto created = environment3->CreateCoreWebView2CompositionController(_parent, onComposition.get());
            if (FAILED(created))
            {
                failed(created);
            }
        });

        const auto hr = HyperlinkHtmlHost::CreateEnvironment(onEnvironment.get());
        if (FAILED(hr))
        {
            failed(hr);
        }
    }

    void WebEmbedHost::_configure()
    {
        if (!_controller || !_composition)
        {
            return;
        }

        // Sizes in the element's own units; WebView2 multiplies by the scale itself.
        if (const auto controller3 = _controller.try_as<ICoreWebView2Controller3>())
        {
            LOG_IF_FAILED(controller3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RASTERIZATION_SCALE));
            LOG_IF_FAILED(controller3->put_ShouldDetectMonitorScaleChanges(FALSE));
            LOG_IF_FAILED(controller3->put_RasterizationScale(_scale()));
        }
        // Transparent until the page paints, so nothing flashes white over a dark card.
        if (const auto controller2 = _controller.try_as<ICoreWebView2Controller2>())
        {
            LOG_IF_FAILED(controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 }));
        }

        LOG_IF_FAILED(_composition->put_RootVisualTarget(static_cast<::IUnknown*>(winrt::get_unknown(_visual))));
        LOG_IF_FAILED(_controller->get_CoreWebView2(_webView.put()));
        if (!_webView)
        {
            return;
        }

        winrt::com_ptr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(_webView->get_Settings(settings.put())) && settings)
        {
            LOG_IF_FAILED(settings->put_AreDevToolsEnabled(FALSE));
            LOG_IF_FAILED(settings->put_IsStatusBarEnabled(FALSE));
            LOG_IF_FAILED(settings->put_IsZoomControlEnabled(FALSE));
        }

        EventRegistrationToken token{};

        // The page is an app with its own routing, so it may move around its own origin.
        // Leaving it is a link the user followed, and goes where the card's Open would.
        LOG_IF_FAILED(_webView->add_NavigationStarting(make_callback<NavigationStarting>([alive = _alive, this](ICoreWebView2NavigationStartingEventArgs* args) {
                                                           if (!*alive || !args)
                                                           {
                                                               return;
                                                           }
                                                           wil::unique_cotaskmem_string target;
                                                           LOG_IF_FAILED(args->get_Uri(&target));
                                                           if (_sameOrigin(target.get()))
                                                           {
                                                               return;
                                                           }
                                                           LOG_IF_FAILED(args->put_Cancel(TRUE));
                                                           _openElsewhere(target.get());
                                                       }).get(),
                                                       &token));

        // target=_blank, window.open: never a second browser window.
        LOG_IF_FAILED(_webView->add_NewWindowRequested(make_callback<NewWindowRequested>([alive = _alive, this](ICoreWebView2NewWindowRequestedEventArgs* args) {
                                                           if (!*alive || !args)
                                                           {
                                                               return;
                                                           }
                                                           LOG_IF_FAILED(args->put_Handled(TRUE));
                                                           wil::unique_cotaskmem_string target;
                                                           LOG_IF_FAILED(args->get_Uri(&target));
                                                           _openElsewhere(target.get());
                                                       }).get(),
                                                       &token));

        // A development certificate on the integration's own server -- what its fetch
        // steps opt into with allowUntrustedCertificate. That origin only.
        if (const auto webView14 = _webView.try_as<ICoreWebView2_14>())
        {
            LOG_IF_FAILED(webView14->add_ServerCertificateErrorDetected(make_callback<CertificateError>([alive = _alive, this](ICoreWebView2ServerCertificateErrorDetectedEventArgs* args) {
                                                                            if (!*alive || !args)
                                                                            {
                                                                                return;
                                                                            }
                                                                            wil::unique_cotaskmem_string requestUri;
                                                                            LOG_IF_FAILED(args->get_RequestUri(&requestUri));
                                                                            LOG_IF_FAILED(args->put_Action(_sameOrigin(requestUri.get()) ? COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW :
                                                                                                                                           COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_CANCEL));
                                                                        }).get(),
                                                                        &token));
        }

        // Esc inside the page never reaches XAML. Key-down only and not consumed, so the
        // page still sees it.
        LOG_IF_FAILED(_controller->add_AcceleratorKeyPressed(make_callback<AcceleratorKey>([alive = _alive, this](ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
                                                                 if (!*alive || !args)
                                                                 {
                                                                     return;
                                                                 }
                                                                 COREWEBVIEW2_KEY_EVENT_KIND kind{};
                                                                 UINT key{};
                                                                 if (SUCCEEDED(args->get_KeyEventKind(&kind)) && SUCCEEDED(args->get_VirtualKey(&key)) &&
                                                                     kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && key == VK_ESCAPE)
                                                                 {
                                                                     EscapePressed.raise(*this, nullptr);
                                                                 }
                                                             }).get(),
                                                             &token));

        // The page decides the pointer's shape -- a hand over a link, a beam over text.
        LOG_IF_FAILED(_composition->add_CursorChanged(make_callback<CursorChanged>([alive = _alive, this](IUnknown* /*args*/) {
                                                          if (!*alive || !_composition)
                                                          {
                                                              return;
                                                          }
                                                          HCURSOR cursor{ nullptr };
                                                          if (SUCCEEDED(_composition->get_Cursor(&cursor)))
                                                          {
                                                              _cursor = cursor;
                                                              ::SetCursor(cursor);
                                                          }
                                                      }).get(),
                                                      &token));

        LOG_IF_FAILED(_controller->put_IsVisible(_visible ? TRUE : FALSE));
        _applyBounds();
        _navigate();
    }

    void WebEmbedHost::_navigate()
    {
        if (!_webView || _url.empty())
        {
            return;
        }

        // Old runtimes have no profile to ask; the page's own prefers-color-scheme then
        // follows the OS instead of the Terminal's theme.
        if (const auto webView13 = _webView.try_as<ICoreWebView2_13>())
        {
            winrt::com_ptr<ICoreWebView2Profile> profile;
            if (SUCCEEDED(webView13->get_Profile(profile.put())) && profile)
            {
                LOG_IF_FAILED(profile->put_PreferredColorScheme(_dark ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT));
            }
        }

        // Showing the page that is already there keeps it as the user left it.
        if (_url == _loadedUrl)
        {
            return;
        }
        _loadedUrl = _url;
        LOG_IF_FAILED(_webView->Navigate(_url.c_str()));
    }

    double WebEmbedHost::_scale() const
    {
        try
        {
            if (const auto host = _host.get())
            {
                if (const auto root = host.XamlRoot())
                {
                    return root.RasterizationScale();
                }
            }
        }
        CATCH_LOG();
        return 1.0;
    }

    void WebEmbedHost::_applyBounds()
    {
        const auto host = _host.get();
        if (!host || !_controller)
        {
            return;
        }

        if (const auto controller3 = _controller.try_as<ICoreWebView2Controller3>())
        {
            LOG_IF_FAILED(controller3->put_RasterizationScale(_scale()));
        }
        const RECT bounds{ 0, 0, std::lround(host.ActualWidth()), std::lround(host.ActualHeight()) };
        LOG_IF_FAILED(_controller->put_Bounds(bounds));
    }

    bool WebEmbedHost::_sameOrigin(LPCWSTR uri) const
    {
        return uri && !_origin.empty() && _origin == originOf(uri);
    }

    void WebEmbedHost::_openElsewhere(LPCWSTR uri)
    {
        if (uri && uri[0] != L'\0')
        {
            OpenLinkRequested.raise(*this, winrt::hstring{ uri });
        }
    }

    void WebEmbedHost::_forwardPointer(const WUX::Input::PointerRoutedEventArgs& args, COREWEBVIEW2_MOUSE_EVENT_KIND kind, uint32_t mouseData)
    {
        // Ours either way: nothing under the page should also act on it.
        args.Handled(true);

        const auto host = _host.get();
        if (!host || !_composition)
        {
            return;
        }

        const auto point = args.GetCurrentPoint(host);
        const auto properties = point.Properties();
        const auto position = point.Position();
        const auto scale = _scale();

        // Bounds are in view pixels (BoundsMode), but input is in the page's raw pixels.
        const POINT where{ std::lround(position.X * scale), std::lround(position.Y * scale) };

        uint32_t keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
        if (properties.IsLeftButtonPressed()) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
        if (properties.IsRightButtonPressed()) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
        if (properties.IsMiddleButtonPressed()) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
        if (properties.IsXButton1Pressed()) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON1;
        if (properties.IsXButton2Pressed()) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2;
        using winrt::Windows::System::VirtualKeyModifiers;
        const auto modifiers = args.KeyModifiers();
        if ((modifiers & VirtualKeyModifiers::Shift) == VirtualKeyModifiers::Shift) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
        if ((modifiers & VirtualKeyModifiers::Control) == VirtualKeyModifiers::Control) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;

        LOG_IF_FAILED(_composition->SendMouseInput(kind, static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(keys), mouseData, where));
    }
}
