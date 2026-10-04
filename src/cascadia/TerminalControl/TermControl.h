// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "SearchBoxControl.h"
#include "HyperlinkRules.h"
#include "TermControl.g.h"
#include "../../buffer/out/search.h"
#include "../../cascadia/TerminalCore/Terminal.hpp"
#include "../../renderer/uia/UiaRenderer.hpp"
#include "../../tsf/Handle.h"

#include "ControlInteractivity.h"

namespace Microsoft::Console::VirtualTerminal
{
    struct MouseButtonState;
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct TermControl;

    // Only forward-declared here: HyperlinkHtmlHost.h pulls in the whole of WebView2.h,
    // and TermControl.cpp is the one file that has any business owning one. ~TermControl
    // is defined out of line, so the unique_ptr below never needs the complete type
    // anywhere else.
    class HyperlinkHtmlHost;

    struct TsfDataProvider : ::Microsoft::Console::TSF::IDataProvider
    {
        explicit TsfDataProvider(TermControl* termControl) noexcept;
        virtual ~TsfDataProvider() = default;

        STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) noexcept override;
        ULONG STDMETHODCALLTYPE AddRef() noexcept override;
        ULONG STDMETHODCALLTYPE Release() noexcept override;

        HWND GetHwnd() override;
        RECT GetViewport() override;
        RECT GetCursorPosition() override;
        void HandleOutput(std::wstring_view text) override;
        ::Microsoft::Console::Render::Renderer* GetRenderer() override;

    private:
        ControlCore* _getCore() const noexcept;

        TermControl* _termControl = nullptr;
        HWND _hwnd = nullptr;
    };

    struct TermControl : TermControlT<TermControl>
    {
        TermControl(Control::ControlInteractivity content);

        TermControl(IControlSettings settings, Control::IControlAppearance unfocusedAppearance, TerminalConnection::ITerminalConnection connection);

        static Control::TermControl NewControlByAttachingContent(Control::ControlInteractivity content);

        void UpdateControlSettings(Control::IControlSettings settings);
        void UpdateControlSettings(Control::IControlSettings settings, Control::IControlAppearance unfocusedAppearance);
        IControlSettings Settings() const;

        void KeyBindings(const Control::IKeyBindings& bindings) { _keyBindings = bindings; }
        void HyperlinkPreviewProvider(const Control::IHyperlinkPreviewProvider& provider) { _hyperlinkPreviewProvider = provider; }
        void HyperlinkTooltipsSuppressed(bool suppressed);

        uint64_t ContentId() const;

        hstring GetStartingTitle() const;

        bool CopySelectionToClipboard(bool dismissSelection, bool singleLine, bool withControlSequences, const CopyFormat formats);
        void PasteTextFromClipboard();
        void SelectAll();
        bool ToggleBlockSelection();
        void ToggleMarkMode();
        bool SwitchSelectionEndpoint();
        bool ExpandSelectionToWord();
        void RestoreFromPath(winrt::hstring path);
        void PersistTo(int64_t handle) const;
        safe_void_coroutine PersistToPathInBackground(winrt::hstring path, bool elevated);
        static int64_t TakePersistMicroseconds() noexcept;
        void OpenCWD();
        void Close();
        Windows::Foundation::Size CharacterDimensions() const;
        Windows::Foundation::Size MinimumSize();
        float SnapDimensionToGrid(const bool widthOrHeight, const float dimension);
        void PreviewInput(const winrt::hstring& text);

        Windows::Foundation::Point CursorPositionInDips();
        double QuickFixButtonWidth();
        double QuickFixButtonCollapsedWidth();

        void WindowVisibilityChanged(const bool showOrHide);

        void ColorSelection(Control::SelectionColor fg, Control::SelectionColor bg, Core::MatchMode matchMode);

#pragma region ICoreState
        const uint64_t TaskbarState() const noexcept;
        const uint64_t TaskbarProgress() const noexcept;

        hstring Title();
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() noexcept;
        hstring WorkingDirectory() const;

        TerminalConnection::ConnectionState ConnectionState() const;

        int ScrollOffset() const;
        Core::Size ViewportSize() const;
        int BufferHeight() const;

        bool HasSelection() const;
        bool HasMultiLineSelection() const;
        winrt::hstring SelectedText(bool trimTrailingWhitespace) const;

        bool BracketedPasteEnabled() const noexcept;

        float BackgroundOpacity() const;

        uint64_t OwningHwnd();
        void OwningHwnd(uint64_t owner);

        Windows::Foundation::Collections::IVector<Control::ScrollMark> ScrollMarks() const;
        void AddMark(const Control::ScrollMark& mark);
        void ClearMark();
        void ClearAllMarks();
        void ScrollToMark(const Control::ScrollToMarkDirection& direction);
        void SelectCommand(const bool goUp);
        void SelectOutput(const bool goUp);
#pragma endregion

        void ScrollViewport(int viewTop);

        void AdjustFontSize(float fontSizeDelta);
        void ResetFontSize();
        winrt::Windows::Foundation::Size GetFontSize() const;

        void SendInput(const winrt::hstring& input);
        void ClearBuffer(Control::ClearBufferType clearType);

        void ToggleShaderEffects();

        void RenderEngineSwapChainChanged(IInspectable sender, IInspectable args);
        void _AttachDxgiSwapChainToXaml(HANDLE swapChainHandle);
        safe_void_coroutine _RendererEnteredErrorState(IInspectable sender, IInspectable args);

        void _RenderRetryButton_Click(const IInspectable& button, const IInspectable& args);
        safe_void_coroutine _RendererWarning(IInspectable sender,
                                             Control::RendererWarningArgs args);

        void CreateSearchBoxControl();

        void SearchMatch(const bool goForward);

        bool SearchBoxEditInFocus() const;

        bool OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down);

        bool OnMouseWheel(const Windows::Foundation::Point location, const winrt::Microsoft::Terminal::Core::Point delta, const bool leftButtonDown, const bool midButtonDown, const bool rightButtonDown);

        ~TermControl();

        Windows::UI::Xaml::Automation::Peers::AutomationPeer OnCreateAutomationPeer();
        const Windows::UI::Xaml::Thickness GetPadding();

        static Windows::Foundation::Size GetProposedDimensions(const IControlSettings& settings,
                                                               const uint32_t dpi,
                                                               int32_t commandlineCols,
                                                               int32_t commandlineRows);
        static Windows::Foundation::Size GetProposedDimensions(const IControlSettings& settings, const uint32_t dpi, const winrt::Windows::Foundation::Size& initialSizeInChars);

        winrt::Windows::Foundation::Size GetNewDimensions(const winrt::Windows::Foundation::Size& initialSizeInChars);

        void BellLightOn();

        bool ReadOnly() const noexcept;
        void ToggleReadOnly();
        void SetReadOnly(const bool readOnlyState);

        static Control::MouseButtonState GetPressedMouseButtons(const winrt::Windows::UI::Input::PointerPoint point);
        static unsigned int GetPointerUpdateKind(const winrt::Windows::UI::Input::PointerPoint point);
        static Windows::UI::Xaml::Thickness ParseThicknessFromPadding(const hstring padding);

        hstring ReadEntireBuffer() const;
        hstring ReadViewportText(int32_t maxRows) const;
        bool ViewportContains(const hstring& needle) const;
        Control::CommandHistoryContext CommandHistory() const;
        void UpdateWinGetSuggestions(Windows::Foundation::Collections::IVector<hstring> suggestions);

        void AdjustOpacity(const float opacity, const bool relative);

        bool RawWriteKeyEvent(const WORD vkey, const WORD scanCode, const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers, const bool keyDown);
        bool RawWriteChar(const wchar_t character, const WORD scanCode, const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers);
        void RawWriteString(const winrt::hstring& text);

        void ShowContextMenu();
        bool OpenQuickFixMenu();
        void RefreshQuickFixMenu();
        void ClearQuickFix();

        void Detach();

        TerminalConnection::ITerminalConnection Connection();
        void Connection(const TerminalConnection::ITerminalConnection& connection);
        void HardResetWithoutErase();

        Control::CursorDisplayState CursorVisibility() const noexcept;
        void CursorVisibility(Control::CursorDisplayState cursorVisibility);

        void ApplyPreviewColorScheme(const Core::ICoreScheme& scheme) { _core.ApplyPreviewColorScheme(scheme); }
        void ResetPreviewColorScheme() { _core.ResetPreviewColorScheme(); }
        void SetOverrideColorScheme(const Core::ICoreScheme& scheme) { _core.SetOverrideColorScheme(scheme); }

        // -------------------------------- WinRT Events ---------------------------------
        // clang-format off
        til::property_changed_event PropertyChanged;

        til::typed_event<IInspectable, Control::OpenHyperlinkEventArgs> OpenHyperlink;
        til::typed_event<IInspectable, Control::HyperlinkTooltipActionInvokedEventArgs> HyperlinkTooltipActionInvoked;
        til::typed_event<IInspectable, Control::ShowHyperlinkPreviewRequestedEventArgs> ShowHyperlinkPreviewRequested;
        til::typed_event<IInspectable, Control::EditHyperlinkRuleRequestedEventArgs> EditHyperlinkRuleRequested;
        til::typed_event<IInspectable, Control::NoticeEventArgs> RaiseNotice;
        til::typed_event<> HidePointerCursor;
        til::typed_event<> RestorePointerCursor;
        til::typed_event<> ReadOnlyChanged;
        til::typed_event<IInspectable, IInspectable> FocusFollowMouseRequested;
        til::typed_event<Control::TermControl, Windows::UI::Xaml::RoutedEventArgs> Initialized;
        til::typed_event<> WarningBell;
        til::typed_event<IInspectable, Control::KeySentEventArgs> KeySent;
        til::typed_event<IInspectable, Control::CharSentEventArgs> CharSent;
        til::typed_event<IInspectable, Control::StringSentEventArgs> StringSent;
        til::typed_event<IInspectable, Control::SearchMissingCommandEventArgs> SearchMissingCommand;
        til::typed_event<IInspectable, Control::WindowSizeChangedEventArgs> WindowSizeChanged;

        // UNDER NO CIRCUMSTANCES SHOULD YOU ADD A (PROJECTED_)FORWARDED_TYPED_EVENT HERE
        // Those attach the handler to the core directly, and will explode if
        // the core ever gets detached & reattached to another window.
        BUBBLED_FORWARDED_TYPED_EVENT(TitleChanged,             IInspectable, Control::TitleChangedEventArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(TabColorChanged,          IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(SetTaskbarProgress,       IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(ConnectionStateChanged,   IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(ShowWindowChanged,        IInspectable, Control::ShowWindowArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(CloseTerminalRequested,   IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(CompletionsChanged,       IInspectable, Control::CompletionsChangedEventArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(RestartTerminalRequested, IInspectable, IInspectable);
        BUBBLED_FORWARDED_TYPED_EVENT(WriteToClipboard,         IInspectable, Control::WriteToClipboardEventArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(PasteFromClipboard,       IInspectable, Control::PasteFromClipboardEventArgs);
        BUBBLED_FORWARDED_TYPED_EVENT(ShowNotification,         IInspectable, Control::ShowNotificationEventArgs);

        // clang-format on

        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::Media::Brush, BackgroundBrush, PropertyChanged.raise, nullptr);

    private:
        friend struct TermControlT<TermControl>; // friend our parent so it can bind private event handlers
        friend struct TsfDataProvider;

        // NOTE: _uiaEngine must be ordered before _core.
        //
        // ControlCore::AttachUiaEngine receives a IRenderEngine as a raw pointer, which we own.
        // We must ensure that we first destroy the ControlCore before the UiaEngine instance
        // in order to safely resolve this unsafe pointer dependency. Otherwise, a deallocated
        // IRenderEngine is accessed when ControlCore calls Renderer::TriggerTeardown.
        // (C++ class members are destroyed in reverse order.)
        // Further, the TermControlAutomationPeer must be destructed after _uiaEngine!
        Control::TermControlAutomationPeer _automationPeer{ nullptr };
        Control::ControlInteractivity _interactivity{ nullptr };
        Control::ControlCore _core{ nullptr };
        Control::IKeyBindings _keyBindings{ nullptr };
        Control::IHyperlinkPreviewProvider _hyperlinkPreviewProvider{ nullptr };
        TsfDataProvider _tsfDataProvider{ this };
        winrt::com_ptr<SearchBoxControl> _searchBox;

        enum class AltNumpadEncoding
        {
            OEM,
            ANSI,
            Unicode,
        };
        struct AltNumpadState
        {
            struct CachedKey
            {
                WORD vkey;
                WORD scanCode;
                ::Microsoft::Terminal::Core::ControlKeyStates modifiers;
                bool keyDown;
            };
            AltNumpadEncoding encoding = AltNumpadEncoding::OEM;
            uint32_t accumulator = 0;
            // Checking for accumulator != 0 to see if we have an ongoing Alt+Numpad composition is insufficient.
            // The state can be active, while the accumulator is 0, if the user pressed Alt+Numpad0 which enabled
            // the OEM encoding mode (= active), and then pressed Numpad0 again (= accumulator is still 0).
            bool active = false;
            til::small_vector<CachedKey, 4> cachedKeyEvents;
        };
        AltNumpadState _altNumpadState;

        bool _closing{ false };
        bool _focused{ false };
        bool _initializedTerminal{ false };
        bool _quickFixButtonCollapsible{ false };
        bool _quickFixesAvailable{ false };
        til::CoordType _quickFixBufferPos{};

        std::shared_ptr<ThrottledFunc<>> _playWarningBell;

        struct ScrollBarUpdate
        {
            std::optional<double> newValue;
            double newMaximum;
            double newMinimum;
            double newViewportSize;
        };

        std::shared_ptr<ThrottledFunc<ScrollBarUpdate>> _updateScrollBar;

        bool _isInternalScrollBarUpdate;

        winrt::Windows::UI::Composition::ScalarKeyFrameAnimation _bellLightAnimation{ nullptr };
        winrt::Windows::UI::Composition::ScalarKeyFrameAnimation _bellDarkAnimation{ nullptr };
        SafeDispatcherTimer _bellLightTimer;

        winrt::Windows::UI::Xaml::Controls::SwapChainPanel::LayoutUpdated_revoker _layoutUpdatedRevoker;
        winrt::hstring _restorePath;
        // Guards against a second periodic capture starting while the last one
        // is still writing this pane's file. Both would be writing the same
        // path, and the loser would leave a half-serialized buffer behind.
        std::atomic<bool> _persistInFlight{ false };
        // Process-wide, because the cost worth reporting is what a whole
        // persistence pass took across every pane, not what one of them did.
        static inline std::atomic<int64_t> _persistMicroseconds{ 0 };
        winrt::hstring _followLinkHintCtrlClick;

        // The hyperlink card. _hoveredUri is the URI as the buffer holds it: the Run in the
        // card may carry a punycode annotation beside it, which is there to be read and not
        // to be acted on. The timers are the ones GH#8242 said we would have to write
        // ourselves, and _pointerInHyperlinkCard is what stops the terminal's own
        // PointerExited from pulling the card out from under the pointer heading for it.
        winrt::hstring _hoveredUri;
        SafeDispatcherTimer _hyperlinkShowTimer;
        SafeDispatcherTimer _hyperlinkHideTimer;
        bool _pointerInHyperlinkCard{ false };
        bool _showMarksInScrollbar{ false };

        // The show/hide delay, max width, built-in button visibility and custom action
        // list actually in effect for the currently hovered link -- the global settings,
        // unless a HyperlinkTooltipRule matched and overrode some of them. Recomputed once
        // per _hoveredHyperlinkChanged and read from everywhere the four globals used to be
        // read directly, so a rule's overrides apply consistently everywhere they're used.
        EffectiveHyperlinkTooltipSettings _currentHyperlinkTooltipSettings;
        EffectiveHyperlinkTooltipSettings _effectiveHyperlinkTooltipSettings(std::wstring_view uri, bool isFileLink) const;

        // The integration preview for the hovered link. The generation counter is what
        // discards a fetch that finishes after the pointer has moved on.
        uint32_t _hyperlinkPreviewGeneration{ 0 };
        Control::HyperlinkPreview _currentHyperlinkPreview{ nullptr };
        Windows::Foundation::IAsyncOperation<Control::HyperlinkPreview> _filePreviewOperation{ nullptr };
        winrt::hstring _hoveredLinkTarget() const;
        void _setHyperlinkPreviewLoading(bool loading);
        safe_void_coroutine _requestHyperlinkPreview(uint32_t generation, winrt::hstring text, winrt::hstring integration);
        void _applyHyperlinkPreview(const Control::HyperlinkPreview& preview);

        // While a link preview pane is showing "pane only", hovering does nothing at all.
        bool _hyperlinkTooltipsSuppressed{ false };
        // The link the pane was last asked to show, so that hovering along a link --
        // which reports a change on every pointer move -- asks exactly once.
        winrt::hstring _lastPanePreviewUri;

        // The card's tab strip. -1 is the built-in field list, 0.. index into the
        // preview's own Tabs. Rebuilt from scratch for every preview, because the
        // tabs an integration offers belong to that one result.
        int32_t _hyperlinkSelectedTab{ -1 };
        void _rebuildHyperlinkTabStrip(const Control::HyperlinkPreview& preview);
        void _showHyperlinkTab(int32_t index);
        // A tab the integration named but did not fill in, asked for on the first
        // visit to it. Takes the preview by value: it is what the answer is
        // written into, and it has to outlive the fetch.
        safe_void_coroutine _requestHyperlinkTab(uint32_t generation, Control::HyperlinkPreview preview, int32_t index, winrt::hstring tabKey);
        void _HyperlinkTabClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _fillHyperlinkFields(const Control::HyperlinkPreview& preview);
        void _fillHyperlinkComments(const Control::HyperlinkPreviewTab& tab);

        // The action the card is currently offering, plus the id that would put the
        // last applied one back. Only the first action is shown: the card is small,
        // and no integration declares more than one today.
        Control::HyperlinkPreviewAction _currentHyperlinkAction{ nullptr };
        winrt::hstring _hyperlinkUndoChoiceId;
        // The Undo button's own localized label, kept so that relabelling it with the
        // far end's wording for one action can be undone for the next link.
        Windows::Foundation::IInspectable _hyperlinkUndoDefaultContent{ nullptr };
        void _rebuildHyperlinkActions(const Control::HyperlinkPreview& preview);
        void _updateHyperlinkActionFields();
        void _updateHyperlinkActionApplyState();
        // Not const: the XAML-generated element accessors these read are not.
        Control::HyperlinkPreviewActionOption _selectedHyperlinkActionOption();
        Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring> _collectHyperlinkActionFields();
        void _setHyperlinkActionBusy(bool busy);
        safe_void_coroutine _invokeHyperlinkAction(uint32_t generation, winrt::hstring choiceId);
        void _HyperlinkActionOptionChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::SelectionChangedEventArgs& e);
        // The options dropdown is a popup outside the card, so the pointer reaching
        // the list counts as leaving the card. These hold the card open across that.
        bool _hyperlinkActionDropDownOpen{ false };
        void _HyperlinkActionDropDownOpened(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& e);
        void _HyperlinkActionDropDownClosed(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& e);
        void _HyperlinkActionApplyClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkActionUndoClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkShowInPaneClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkRuleInfoClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _raiseShowHyperlinkPreviewRequested(bool hover = false, bool createPane = true);

        // A preview whose integration returned rendered HTML is drawn by a WebView2
        // parented to the owning window rather than by the field list. It is created
        // lazily and only when HyperlinkHtmlHost::IsAvailable() -- which needs both
        // Feature_HyperlinkPreviewHtml and a WebView2Loader.dll that no build ships yet --
        // so in every build today this pointer stays null and every path below it is a
        // null test that falls through to the field list exactly as before.
        std::unique_ptr<HyperlinkHtmlHost> _hyperlinkHtmlHost;
        int32_t _hyperlinkHtmlHeight{ 120 };
        void _ensureHyperlinkHtmlHost();
        void _hideHyperlinkHtml();
        void _setHyperlinkHtmlHeight(int32_t height);
        RECT _htmlHostRect();
        std::optional<SafeDispatcherTimer> _resizeOverlayTimer;
        Core::Size _lastResizeOverlaySize{};
        void _ShowResizeOverlay();

        bool _isBackgroundLight{ false };
        bool _detached{ false };
        til::CoordType _searchScrollOffset = 0;

        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalPrimaryElements{ nullptr };
        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalSecondaryElements{ nullptr };
        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalSelectedPrimaryElements{ nullptr };
        Windows::Foundation::Collections::IObservableVector<Windows::UI::Xaml::Controls::ICommandBarElement> _originalSelectedSecondaryElements{ nullptr };

        Control::CursorDisplayState _cursorVisibility{ Control::CursorDisplayState::Default };

        inline bool _IsClosing() const noexcept
        {
#ifndef NDEBUG
            // _closing isn't atomic and may only be accessed from the main thread.
            if (const auto dispatcher = Dispatcher())
            {
                assert(dispatcher.HasThreadAccess());
            }
#endif
            return _closing;
        }

        void _initializeForAttach();

        void _UpdateSettingsFromUIThread();
        void _UpdateAppearanceFromUIThread(Control::IControlAppearance newAppearance);

        void _ApplyUISettings();
        void UpdateAppearance(Control::IControlAppearance newAppearance);
        void _SetBackgroundImage(const IControlAppearance& newAppearance);

        void _InitializeBackgroundBrush();
        safe_void_coroutine _coreBackgroundColorChanged(const IInspectable& sender, const IInspectable& args);
        void _changeBackgroundColor(til::color bg);
        static bool _isColorLight(til::color bg) noexcept;
        void _changeBackgroundOpacity();

        enum InitializeReason : bool
        {
            Create,
            Reattach
        };
        bool _InitializeTerminal(const InitializeReason reason);
        safe_void_coroutine _restoreInBackground();
        void _TappedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::TappedRoutedEventArgs& e);
        void _KeyDownHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _KeyUpHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e);
        void _CharacterHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::CharacterReceivedRoutedEventArgs& e);
        void _PointerPressedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _PointerMovedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _PointerReleasedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _PointerExitedHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        void _HyperlinkCardPointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _HyperlinkCardPointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _HyperlinkCardPointerPressed(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _HyperlinkCardPointerReleased(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _HyperlinkCardSizeChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::SizeChangedEventArgs& e);
        // The one place that decides what an action id means, so a card button and
        // a click chord bound to the same id behave identically.
        void _invokeHyperlinkActionById(const winrt::hstring& actionId);
        safe_void_coroutine _invokeHyperlinkOpenAction(winrt::hstring text, winrt::hstring integration, winrt::hstring openAction, winrt::hstring fallbackUri);
        winrt::hstring _followLinkHintText(const Control::IControlSettings& settings) const;
        void _HyperlinkOpenClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkCopyLinkClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkCopyPathClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkRevealClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkCustomActionClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _HyperlinkCommitCopyShaClick(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _formatHighlightedUri(const winrt::hstring& uriText);
        void _applyActionPlacement();
        void _applyRuleInfo();
        void _applyIntegrationDisplayMode();
        // Which way the card was placed last time it was shown. The button row
        // is asked to sit near the link or far from it, and the card opens below
        // the link but flips above it when there is short of room -- so this is
        // what turns that request into one of the two hosts.
        bool _hyperlinkCardAboveLine{ false };
        std::wstring _currentCommitFullSha;
        void _showHyperlinkCard();
        void _hideHyperlinkCard();
        void _scheduleHyperlinkCardHide();
        std::wstring _resolvedHyperlinkTarget() const;
        void _MouseWheelHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _ScrollbarChangeHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs& e);

        void _QuickFixButton_PointerEntered(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        void _QuickFixButton_PointerExited(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        void _GotFocusHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);
        void _LostFocusHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& e);

        safe_void_coroutine _DragDropHandler(Windows::Foundation::IInspectable sender, Windows::UI::Xaml::DragEventArgs e);
        void _DragOverHandler(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::DragEventArgs& e);

        safe_void_coroutine _HyperlinkHandler(Windows::Foundation::IInspectable sender, Control::OpenHyperlinkEventArgs e);

        void _BellLightOff(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& e);

        void _SwapChainSizeChanged(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::SizeChangedEventArgs& e);
        void _SwapChainScaleChanged(const Windows::UI::Xaml::Controls::SwapChainPanel& sender, const Windows::Foundation::IInspectable& args);

        void _ScrollPositionChanged(const IInspectable& sender, const Control::ScrollPositionChangedArgs& args);

        bool _CapturePointer(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);
        bool _ReleasePointerCapture(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        void _KeyHandler(const Windows::UI::Xaml::Input::KeyRoutedEventArgs& e, const bool keyDown);
        bool _KeyHandler(WORD vkey, WORD scanCode, ::Microsoft::Terminal::Core::ControlKeyStates modifiers, bool keyDown);
        static ::Microsoft::Terminal::Core::ControlKeyStates _GetPressedModifierKeys() noexcept;
        bool _TryHandleKeyBinding(const WORD vkey, const WORD scanCode, ::Microsoft::Terminal::Core::ControlKeyStates modifiers) const;
        static void _ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept;
        bool _TrySendKeyEvent(const WORD vkey, const WORD scanCode, ::Microsoft::Terminal::Core::ControlKeyStates modifiers, const bool keyDown);

        winrt::Windows::Foundation::Point _toControlOrigin(const til::point terminalPosition);
        Core::Point _toTerminalOrigin(winrt::Windows::Foundation::Point cursorPosition);

        void _Search(const winrt::hstring& text, const bool goForward, const bool caseSensitive, const bool regularExpression);
        void _SearchChanged(const winrt::hstring& text, const bool goForward, const bool caseSensitive, const bool regularExpression);
        void _CloseSearchBoxControl(const winrt::Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _refreshSearch();
        void _handleSearchResults(SearchResults results);

        void _hoveredHyperlinkChanged(const IInspectable& sender, const IInspectable& args);
        safe_void_coroutine _updateSelectionMarkers(IInspectable sender, Control::UpdateSelectionMarkersEventArgs args);

        void _coreFontSizeChanged(const IInspectable& s, const Control::FontSizeChangedArgs& args);
        void _coreTransparencyChanged(IInspectable sender, Control::TransparencyChangedEventArgs args);
        void _coreRaisedNotice(const IInspectable& s, const Control::NoticeEventArgs& args);
        void _coreWarningBell(const IInspectable& sender, const IInspectable& args);
        void _coreOutputIdle(const IInspectable& sender, const IInspectable& args);

        winrt::Windows::Foundation::Point _toPosInDips(const Core::Point terminalCellPos);
        void _throttledUpdateScrollbar(const ScrollBarUpdate& update);

        void _pasteTextWithBroadcast(const winrt::hstring& text);

        void _contextMenuHandler(IInspectable sender, Control::ContextMenuRequestedEventArgs args);
        void _showContextMenuAt(const winrt::Windows::Foundation::Point& controlRelativePos);

        void _bubbleSearchMissingCommand(const IInspectable& sender, const Control::SearchMissingCommandEventArgs& args);
        winrt::fire_and_forget _bubbleWindowSizeChanged(const IInspectable& sender, Control::WindowSizeChangedEventArgs args);
        til::CoordType _calculateSearchScrollOffset() const;

        void _PasteCommandHandler(const IInspectable& sender, const IInspectable& args);
        void _CopyCommandHandler(const IInspectable& sender, const IInspectable& args);
        void _SearchCommandHandler(const IInspectable& sender, const IInspectable& args);

        void _SelectCommandHandler(const IInspectable& sender, const IInspectable& args);
        void _SelectOutputHandler(const IInspectable& sender, const IInspectable& args);

        struct Revokers
        {
            Control::ControlCore::ScrollPositionChanged_revoker coreScrollPositionChanged;
            Control::ControlCore::WarningBell_revoker WarningBell;
            Control::ControlCore::RendererEnteredErrorState_revoker RendererEnteredErrorState;
            Control::ControlCore::BackgroundColorChanged_revoker BackgroundColorChanged;
            Control::ControlCore::FontSizeChanged_revoker FontSizeChanged;
            Control::ControlCore::TransparencyChanged_revoker TransparencyChanged;
            Control::ControlCore::RaiseNotice_revoker RaiseNotice;
            Control::ControlCore::HoveredHyperlinkChanged_revoker HoveredHyperlinkChanged;
            Control::ControlCore::OutputIdle_revoker OutputIdle;
            Control::ControlCore::UpdateSelectionMarkers_revoker UpdateSelectionMarkers;
            Control::ControlCore::OpenHyperlink_revoker coreOpenHyperlink;
            Control::ControlCore::TitleChanged_revoker TitleChanged;
            Control::ControlCore::WriteToClipboard_revoker WriteToClipboard;
            Control::ControlCore::TabColorChanged_revoker TabColorChanged;
            Control::ControlCore::TaskbarProgressChanged_revoker TaskbarProgressChanged;
            Control::ControlCore::ConnectionStateChanged_revoker ConnectionStateChanged;
            Control::ControlCore::ShowWindowChanged_revoker ShowWindowChanged;
            Control::ControlCore::CloseTerminalRequested_revoker CloseTerminalRequested;
            Control::ControlCore::CompletionsChanged_revoker CompletionsChanged;
            Control::ControlCore::RestartTerminalRequested_revoker RestartTerminalRequested;
            Control::ControlCore::SearchMissingCommand_revoker SearchMissingCommand;
            Control::ControlCore::ShowNotification_revoker ShowNotification;
            Control::ControlCore::RefreshQuickFixUI_revoker RefreshQuickFixUI;
            Control::ControlCore::WindowSizeChanged_revoker WindowSizeChanged;

            // These are set up in _InitializeTerminal
            Control::ControlCore::RendererWarning_revoker RendererWarning;
            Control::ControlCore::SwapChainChanged_revoker SwapChainChanged;
            Windows::UI::ViewManagement::AccessibilitySettings::HighContrastChanged_revoker HighContrastChanged;

            Control::ControlInteractivity::OpenHyperlink_revoker interactivityOpenHyperlink;
            Control::ControlInteractivity::ScrollPositionChanged_revoker interactivityScrollPositionChanged;
            Control::ControlInteractivity::PasteFromClipboard_revoker PasteFromClipboard;
            Control::ControlInteractivity::ContextMenuRequested_revoker ContextMenuRequested;
        } _revokers{};
    };
}

namespace winrt::Microsoft::Terminal::Control::factory_implementation
{
    BASIC_FACTORY(TermControl);
}
