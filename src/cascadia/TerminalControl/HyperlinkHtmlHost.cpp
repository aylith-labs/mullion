// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "HyperlinkHtmlHost.h"

#include <wil/resource.h>
#include <winrt/Windows.Data.Json.h>

using namespace winrt::Windows::Data::Json;

namespace
{
    // The two loader entry points this host needs, resolved by name rather than linked
    // against WebView2LoaderStatic.lib on purpose: an import would make the DLL a
    // load-time requirement of Microsoft.Terminal.Control.dll, and the whole design here
    // is that Terminal behaves exactly as before when the DLL is not in the package --
    // which, today, it is not. Signatures copied from WebView2.h.
    using PfnGetAvailableCoreWebView2BrowserVersionString = HRESULT(STDAPICALLTYPE*)(PCWSTR browserExecutableFolder, LPWSTR* versionInfo);
    using PfnCreateCoreWebView2EnvironmentWithOptions = HRESULT(STDAPICALLTYPE*)(PCWSTR browserExecutableFolder, PCWSTR userDataFolder, ICoreWebView2EnvironmentOptions* environmentOptions, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);

    struct WebView2Loader
    {
        wil::unique_hmodule module;
        PfnGetAvailableCoreWebView2BrowserVersionString getAvailableBrowserVersionString{ nullptr };
        PfnCreateCoreWebView2EnvironmentWithOptions createEnvironmentWithOptions{ nullptr };
    };

    WebView2Loader& webView2Loader() noexcept
    {
        static WebView2Loader loader;
        return loader;
    }

    // WebView2 wants a writable folder of its own. The package's LocalState is the right
    // home for it; the temp directory is the answer for the unpackaged case, where
    // ApplicationData::Current() throws rather than returning null.
    std::wstring webView2UserDataFolder()
    {
        try
        {
            const auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
            return std::wstring{ local } + L"\\WebView2";
        }
        CATCH_LOG();

        wchar_t temp[MAX_PATH + 1]{};
        // GetTempPathW's result already ends in a backslash, so the name appends directly.
        const auto length = ::GetTempPathW(ARRAYSIZE(temp), &temp[0]);
        std::wstring path{ &temp[0], length };
        path += L"WindowsTerminal-WebView2";
        return path;
    }

    // Turns a string into a JSON string literal, quotes and escaping included, so it can be
    // pasted into the bootstrap script without the URI being able to end the literal.
    std::wstring jsonStringLiteral(const winrt::hstring& value)
    {
        try
        {
            return std::wstring{ JsonValue::CreateStringValue(value).Stringify() };
        }
        CATCH_LOG();
        return L"\"\"";
    }

    // Whatever the integration produced is about to be pasted into a <script>, so it has to
    // be JSON and nothing else. Round-tripping it through the parser is what guarantees
    // that; anything that does not parse becomes null rather than becoming code.
    std::wstring jsonValueLiteral(const winrt::hstring& json)
    {
        if (!json.empty())
        {
            try
            {
                JsonValue parsed{ nullptr };
                if (JsonValue::TryParse(json, parsed) && parsed)
                {
                    return std::wstring{ parsed.Stringify() };
                }
            }
            CATCH_LOG();
        }
        return L"null";
    }

    // A minimal IUnknown for the five WebView2 completion/event handlers below. WRL's
    // Callback<> and winrt::implements would both do, but each drags in machinery this
    // file does not otherwise need, and the whole of it is thirty lines.
    template<typename I>
    struct ComCallbackBase : I
    {
        ULONG STDMETHODCALLTYPE AddRef() noexcept override
        {
            return ++_refCount;
        }

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
        virtual ~ComCallbackBase() = default;

    private:
        std::atomic<ULONG> _refCount{ 1 };
    };

    struct EnvironmentCompletedHandler final : ComCallbackBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
    {
        explicit EnvironmentCompletedHandler(std::function<void(HRESULT, ICoreWebView2Environment*)> callback) :
            _callback{ std::move(callback) }
        {
        }

        HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment* result) noexcept override
        try
        {
            _callback(errorCode, result);
            return S_OK;
        }
        CATCH_RETURN()

    private:
        std::function<void(HRESULT, ICoreWebView2Environment*)> _callback;
    };

    struct ControllerCompletedHandler final : ComCallbackBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
    {
        explicit ControllerCompletedHandler(std::function<void(HRESULT, ICoreWebView2Controller*)> callback) :
            _callback{ std::move(callback) }
        {
        }

        HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller* result) noexcept override
        try
        {
            _callback(errorCode, result);
            return S_OK;
        }
        CATCH_RETURN()

    private:
        std::function<void(HRESULT, ICoreWebView2Controller*)> _callback;
    };

    struct ScriptAddedHandler final : ComCallbackBase<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>
    {
        explicit ScriptAddedHandler(std::function<void(HRESULT, LPCWSTR)> callback) :
            _callback{ std::move(callback) }
        {
        }

        HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR result) noexcept override
        try
        {
            _callback(errorCode, result);
            return S_OK;
        }
        CATCH_RETURN()

    private:
        std::function<void(HRESULT, LPCWSTR)> _callback;
    };

    struct NavigationStartingHandler final : ComCallbackBase<ICoreWebView2NavigationStartingEventHandler>
    {
        explicit NavigationStartingHandler(std::function<void(ICoreWebView2NavigationStartingEventArgs*)> callback) :
            _callback{ std::move(callback) }
        {
        }

        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/, ICoreWebView2NavigationStartingEventArgs* args) noexcept override
        try
        {
            _callback(args);
            return S_OK;
        }
        CATCH_RETURN()

    private:
        std::function<void(ICoreWebView2NavigationStartingEventArgs*)> _callback;
    };

    struct WebMessageReceivedHandler final : ComCallbackBase<ICoreWebView2WebMessageReceivedEventHandler>
    {
        explicit WebMessageReceivedHandler(std::function<void(ICoreWebView2WebMessageReceivedEventArgs*)> callback) :
            _callback{ std::move(callback) }
        {
        }

        HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args) noexcept override
        try
        {
            _callback(args);
            return S_OK;
        }
        CATCH_RETURN()

    private:
        std::function<void(ICoreWebView2WebMessageReceivedEventArgs*)> _callback;
    };
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    HyperlinkHtmlHost::~HyperlinkHtmlHost()
    {
        // Any WebView2 completion still in flight holds a copy of this flag and will see it.
        *_alive = false;

        if (_controller)
        {
            LOG_IF_FAILED(_controller->Close());
        }
    }

    bool HyperlinkHtmlHost::IsAvailable() noexcept
    {
        // Deliberately a runtime `&&` and not `if constexpr`: the flag is a compile-time
        // constant and it is false in every branding today, and testing it at runtime is
        // what keeps everything below compiled -- and therefore kept honest by the build --
        // instead of discarded. It is written as a short-circuit rather than an early
        // `return false` so that no statement in this file becomes provably unreachable,
        // which /W4 /WX would reject (C4702).
        static const auto available = Feature_HyperlinkPreviewHtml::IsEnabled() && _tryResolveLoader();
        return available;
    }

    HRESULT HyperlinkHtmlHost::CreateEnvironment(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) noexcept
    try
    {
        const auto& loader = webView2Loader();
        if (!IsAvailable() || !loader.createEnvironmentWithOptions)
        {
            return E_NOT_VALID_STATE;
        }
        const auto folder = webView2UserDataFolder();
        return loader.createEnvironmentWithOptions(nullptr, folder.c_str(), nullptr, handler);
    }
    CATCH_RETURN()

    bool HyperlinkHtmlHost::_tryResolveLoader() noexcept
    {
        auto& loader = webView2Loader();

        // The application directory and System32, and nothing else: not the working
        // directory, and not the rest of the search path.
        loader.module.reset(::LoadLibraryExW(L"WebView2Loader.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
        if (!loader.module)
        {
            return false;
        }

        loader.getAvailableBrowserVersionString = reinterpret_cast<PfnGetAvailableCoreWebView2BrowserVersionString>(::GetProcAddress(loader.module.get(), "GetAvailableCoreWebView2BrowserVersionString"));
        loader.createEnvironmentWithOptions = reinterpret_cast<PfnCreateCoreWebView2EnvironmentWithOptions>(::GetProcAddress(loader.module.get(), "CreateCoreWebView2EnvironmentWithOptions"));

        auto usable = loader.getAvailableBrowserVersionString != nullptr && loader.createEnvironmentWithOptions != nullptr;
        if (usable)
        {
            // A loader without a runtime behind it is no use. Not logged: "no WebView2
            // runtime is installed" is an ordinary answer here, not a fault.
            LPWSTR rawVersion{ nullptr };
            const auto hr = loader.getAvailableBrowserVersionString(nullptr, &rawVersion);
            const wil::unique_cotaskmem_string version{ rawVersion };
            usable = SUCCEEDED(hr) && version && version.get()[0] != L'\0';
        }

        if (!usable)
        {
            loader.getAvailableBrowserVersionString = nullptr;
            loader.createEnvironmentWithOptions = nullptr;
            loader.module.reset();
        }
        return usable;
    }

    void HyperlinkHtmlHost::Show(HWND owner, const RECT& ownerClientRect, const winrt::hstring& html, const winrt::hstring& dataJson, const winrt::hstring& uri, bool dark) noexcept
    {
        try
        {
            if (!owner)
            {
                return;
            }

            _owner = owner;
            _bounds = ownerClientRect;
            _html = html;
            _dataJson = dataJson;
            _uri = uri;
            _dark = dark;
            _visible = true;

            if (_webView)
            {
                _applyBounds();
                _prepareAndNavigate();
                return;
            }

            // First show: the environment and the controller are both created
            // asynchronously, and everything above is the pending show they will apply.
            _ensureEnvironment();
        }
        CATCH_LOG();
    }

    void HyperlinkHtmlHost::Move(const RECT& ownerClientRect) noexcept
    {
        _bounds = ownerClientRect;
        _applyBounds();
    }

    void HyperlinkHtmlHost::Hide() noexcept
    {
        _visible = false;
        if (_controller)
        {
            LOG_IF_FAILED(_controller->put_IsVisible(FALSE));
        }
    }

    void HyperlinkHtmlHost::_ensureEnvironment() noexcept
    {
        if (_environment)
        {
            _createController();
            return;
        }

        // Null unless IsAvailable() resolved the loader, which is the state every build
        // ships in today -- so this is where a Show() that skipped the check stops.
        const auto& loader = webView2Loader();
        if (!loader.createEnvironmentWithOptions || _creatingEnvironment)
        {
            return;
        }

        try
        {
            const auto folder = webView2UserDataFolder();

            winrt::com_ptr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> handler;
            handler.attach(new EnvironmentCompletedHandler{
                [alive = _alive, this](HRESULT errorCode, ICoreWebView2Environment* environment) {
                    if (!*alive)
                    {
                        return;
                    }
                    _creatingEnvironment = false;
                    LOG_IF_FAILED(errorCode);
                    if (FAILED(errorCode) || !environment)
                    {
                        return;
                    }
                    _environment.copy_from(environment);
                    _createController();
                } });

            _creatingEnvironment = true;
            const auto hr = loader.createEnvironmentWithOptions(nullptr, folder.c_str(), nullptr, handler.get());
            LOG_IF_FAILED(hr);
            if (FAILED(hr))
            {
                _creatingEnvironment = false;
            }
        }
        CATCH_LOG();
    }

    void HyperlinkHtmlHost::_createController() noexcept
    {
        if (!_environment || _controller || _creatingController || !_owner)
        {
            return;
        }

        try
        {
            winrt::com_ptr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> handler;
            handler.attach(new ControllerCompletedHandler{
                [alive = _alive, this](HRESULT errorCode, ICoreWebView2Controller* controller) {
                    if (!*alive)
                    {
                        return;
                    }
                    _creatingController = false;
                    LOG_IF_FAILED(errorCode);
                    if (FAILED(errorCode) || !controller)
                    {
                        return;
                    }
                    _controller.copy_from(controller);
                    _configureWebView();
                } });

            _creatingController = true;
            // The controller parents its own child HWND to the owner, which is the
            // top-level window rather than the XAML island: that is why the bounds handed
            // in are the owner's client pixels and not the card's DIPs.
            const auto hr = _environment->CreateCoreWebView2Controller(_owner, handler.get());
            LOG_IF_FAILED(hr);
            if (FAILED(hr))
            {
                _creatingController = false;
            }
        }
        CATCH_LOG();
    }

    void HyperlinkHtmlHost::_configureWebView() noexcept
    {
        if (!_controller)
        {
            return;
        }

        try
        {
            LOG_IF_FAILED(_controller->get_CoreWebView2(_webView.put()));
            if (!_webView)
            {
                return;
            }

            // A preview card is a card, not a browser: no context menu, no dev tools, no
            // status bar, no zoom. postMessage is the one channel the page gets.
            winrt::com_ptr<ICoreWebView2Settings> settings;
            const auto settingsHr = _webView->get_Settings(settings.put());
            LOG_IF_FAILED(settingsHr);
            if (SUCCEEDED(settingsHr) && settings)
            {
                LOG_IF_FAILED(settings->put_AreDefaultContextMenusEnabled(FALSE));
                LOG_IF_FAILED(settings->put_AreDevToolsEnabled(FALSE));
                LOG_IF_FAILED(settings->put_IsStatusBarEnabled(FALSE));
                LOG_IF_FAILED(settings->put_IsZoomControlEnabled(FALSE));
                LOG_IF_FAILED(settings->put_IsWebMessageEnabled(TRUE));
            }

            EventRegistrationToken token{};

            winrt::com_ptr<ICoreWebView2NavigationStartingEventHandler> navigationHandler;
            navigationHandler.attach(new NavigationStartingHandler{
                [alive = _alive, this](ICoreWebView2NavigationStartingEventArgs* args) {
                    if (!*alive || !args)
                    {
                        return;
                    }
                    if (_allowNavigation)
                    {
                        _allowNavigation = false;
                        return;
                    }
                    // Everything past the NavigateToString we asked for -- a link click, a
                    // redirect, a meta refresh -- is refused. A card does not navigate; a
                    // page that wants a link opened says so with postMessage({open}).
                    LOG_IF_FAILED(args->put_Cancel(TRUE));
                } });
            LOG_IF_FAILED(_webView->add_NavigationStarting(navigationHandler.get(), &token));

            winrt::com_ptr<ICoreWebView2WebMessageReceivedEventHandler> messageHandler;
            messageHandler.attach(new WebMessageReceivedHandler{
                [alive = _alive, this](ICoreWebView2WebMessageReceivedEventArgs* args) {
                    if (!*alive || !args)
                    {
                        return;
                    }
                    _handleWebMessage(args);
                } });
            LOG_IF_FAILED(_webView->add_WebMessageReceived(messageHandler.get(), &token));

            _applyBounds();
            _prepareAndNavigate();
        }
        CATCH_LOG();
    }

    void HyperlinkHtmlHost::_prepareAndNavigate() noexcept
    {
        if (!_webView || _html.empty())
        {
            return;
        }

        try
        {
            // The previous hover's injection goes first: the API accumulates, and a card
            // that has shown five links should not be running five copies of this.
            if (!_scriptId.empty())
            {
                LOG_IF_FAILED(_webView->RemoveScriptToExecuteOnDocumentCreated(_scriptId.c_str()));
                _scriptId.clear();
            }

            // The page reads its own integration's step output as window.__data, and the
            // link it was hovered on as window.__uri, before any of its own script runs.
            std::wstring script{ L"window.__data = " };
            script += jsonValueLiteral(_dataJson);
            script += L"; window.__uri = ";
            script += jsonStringLiteral(_uri);
            script += L";";

            winrt::com_ptr<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> scriptHandler;
            scriptHandler.attach(new ScriptAddedHandler{
                [alive = _alive, this](HRESULT errorCode, LPCWSTR id) {
                    if (!*alive)
                    {
                        return;
                    }
                    LOG_IF_FAILED(errorCode);
                    if (SUCCEEDED(errorCode) && id)
                    {
                        _scriptId.assign(id);
                    }
                } });
            LOG_IF_FAILED(_webView->AddScriptToExecuteOnDocumentCreated(script.c_str(), scriptHandler.get()));

            // Old runtimes have no ICoreWebView2_13 and therefore no profile to ask. That
            // is not an error: the page's own `color-scheme: light dark` still applies, so
            // a failed QI just means the OS setting decides instead of the card's theme.
            if (const auto webView13 = _webView.try_as<ICoreWebView2_13>())
            {
                winrt::com_ptr<ICoreWebView2Profile> profile;
                if (SUCCEEDED(webView13->get_Profile(profile.put())) && profile)
                {
                    LOG_IF_FAILED(profile->put_PreferredColorScheme(_dark ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT));
                }
            }

            _allowNavigation = true;
            LOG_IF_FAILED(_webView->NavigateToString(_html.c_str()));
        }
        CATCH_LOG();
    }

    void HyperlinkHtmlHost::_applyBounds() noexcept
    {
        if (!_controller)
        {
            return;
        }

        LOG_IF_FAILED(_controller->put_Bounds(_bounds));
        LOG_IF_FAILED(_controller->put_IsVisible(_visible ? TRUE : FALSE));
    }

    void HyperlinkHtmlHost::_handleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept
    {
        try
        {
            // postMessage(object) arrives as JSON and postMessage(string) as a string that
            // is usually JSON too, so take whichever one the page used. The string form
            // failing is the ordinary "it wasn't a string" answer, not something to log.
            std::wstring message;
            LPWSTR raw{ nullptr };
            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw)
            {
                const wil::unique_cotaskmem_string owned{ raw };
                message.assign(owned.get());
            }
            else
            {
                raw = nullptr;
                const auto hr = args->get_WebMessageAsJson(&raw);
                LOG_IF_FAILED(hr);
                const wil::unique_cotaskmem_string owned{ raw };
                if (SUCCEEDED(hr) && owned)
                {
                    message.assign(owned.get());
                }
            }

            if (message.empty())
            {
                return;
            }

            JsonObject json{ nullptr };
            if (!JsonObject::TryParse(winrt::hstring{ message }, json) || !json)
            {
                return;
            }

            // { "height": n } -- the page measured itself and wants that many pixels.
            if (const auto height = json.GetNamedNumber(L"height", 0.0); height > 0.0 && OnContentHeight)
            {
                OnContentHeight(static_cast<int32_t>(std::lround(height)));
            }

            // { "open": "url" } -- the page wants a link opened the way the card's own
            // Open button opens one, prompts and file:// resolution included.
            if (const auto open = json.GetNamedString(L"open", L""); !open.empty() && OnOpenLink)
            {
                OnOpenLink(open);
            }
        }
        CATCH_LOG();
    }
}
