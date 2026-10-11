/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- HyperlinkHtmlHost.h

Abstract:
- Hosts an integration's own HTML representation of a hovered link inside the
  hyperlink card, using WebView2. A plugin that returns an "html" key gets a real
  browser rectangle instead of the generic label/value field list, can read the
  step results it produced as window.__data, and can talk back to the card with
  chrome.webview.postMessage({ height }) and ({ open }).

- Gated on Feature_HyperlinkPreviewHtml and on WebView2Loader.dll (now packaged)
  with a runtime behind it. The loader is resolved by name, never linked, so a
  machine without WebView2 just gets the field list.

- KNOWN LIMIT: this host parents a WebView2 *window* to the top-level window, and
  that window is WS_EX_NOREDIRECTIONBITMAP with a XAML island that does not clip
  siblings -- a page hosted this way loads but is never seen. WebEmbedHost, which
  hosts WebView2 as a composition visual inside the XAML tree, is the working
  pattern; the HTML representation should move onto it before anything relies on it.

- This is a plain C++ class with no WinRT projection: nothing outside
  TermControl.cpp has any business owning one.

--*/
#pragma once

// WebView2.h is a MIDL-generated header of some 60,000 lines. Level 3 for the
// duration of the include so that a warning inside somebody else's generated
// code cannot fail our /W4 /WX build.
#pragma warning(push, 3)
#include <WebView2.h>
#pragma warning(pop)

namespace winrt::Microsoft::Terminal::Control::implementation
{
    class HyperlinkHtmlHost
    {
    public:
        HyperlinkHtmlHost() = default;
        ~HyperlinkHtmlHost();

        HyperlinkHtmlHost(const HyperlinkHtmlHost&) = delete;
        HyperlinkHtmlHost& operator=(const HyperlinkHtmlHost&) = delete;
        HyperlinkHtmlHost(HyperlinkHtmlHost&&) = delete;
        HyperlinkHtmlHost& operator=(HyperlinkHtmlHost&&) = delete;

        // Feature flag on, WebView2Loader.dll present and loadable, and a WebView2
        // runtime actually installed. Resolved once and cached; false in every build
        // that ships today. Callers must check this before creating a host -- Show()
        // is a no-op without it, but the card should not collapse its field list for
        // a host that will never appear.
        static bool IsAvailable() noexcept;

        // The same loader and user-data folder, for WebEmbedHost: one WebView2 profile
        // for everything the Terminal hosts, so cookies and storage are shared. Fails
        // with E_NOT_VALID_STATE when IsAvailable() is false.
        static HRESULT CreateEnvironment(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) noexcept;

        // ownerClientRect is in *physical pixels*, relative to the client area of the
        // owning top-level window -- that is what a WebView2 controller's bounds mean,
        // and it is not the coordinate space the card itself is laid out in. See
        // TermControl::_htmlHostRect.
        void Show(HWND owner, const RECT& ownerClientRect, const winrt::hstring& html, const winrt::hstring& dataJson, const winrt::hstring& uri, bool dark) noexcept;
        void Move(const RECT& ownerClientRect) noexcept;
        void Hide() noexcept;

        // Raised on the UI thread (WebView2 marshals its events back to the thread that
        // created the controller, which is the thread that called Show).
        // OnContentHeight: the page reported how tall it wants to be, in physical pixels.
        // OnOpenLink: the page asked for a URL to be opened the normal way.
        std::function<void(int32_t)> OnContentHeight;
        std::function<void(winrt::hstring)> OnOpenLink;

    private:
        static bool _tryResolveLoader() noexcept;

        void _ensureEnvironment() noexcept;
        void _createController() noexcept;
        void _configureWebView() noexcept;
        void _prepareAndNavigate() noexcept;
        void _applyBounds() noexcept;
        void _handleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept;

        winrt::com_ptr<ICoreWebView2Environment> _environment;
        winrt::com_ptr<ICoreWebView2Controller> _controller;
        winrt::com_ptr<ICoreWebView2> _webView;

        // Handed to every asynchronous WebView2 callback so a completion that lands after
        // this host is gone knows to do nothing. Everything here runs on one thread, so a
        // plain flag behind a shared_ptr is enough.
        std::shared_ptr<bool> _alive{ std::make_shared<bool>(true) };

        HWND _owner{ nullptr };
        RECT _bounds{};
        winrt::hstring _html;
        winrt::hstring _dataJson;
        winrt::hstring _uri;
        bool _dark{ false };
        bool _visible{ false };
        bool _creatingEnvironment{ false };
        bool _creatingController{ false };
        // NavigateToString is the only navigation this host permits; everything the page
        // tries afterwards is cancelled. Set immediately before each NavigateToString so a
        // second link's HTML can still replace the first one's.
        bool _allowNavigation{ false };
        // AddScriptToExecuteOnDocumentCreated accumulates, so the previous hover's
        // window.__data injection is removed by id before the next one is added.
        std::wstring _scriptId;
    };
}
