// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../inc/PreviewPresentation.h"
#include "../../inc/LintelPaths.h"
#include "../../inc/LintelHomes.h"
#include "TermControl.h"

#include <DefaultSettings.h>
#include <inputpaneinterop.h>
// For the mandatory-integrity label an elevated window's buffer file needs.
#include <sddl.h>
#include <winnetwk.h>
#pragma comment(lib, "mpr.lib") // WNetGetConnectionW, for WSL-mapped drives in dropped paths

#include <regex>
#include <til/regex.h>

#include "TermControlAutomationPeer.h"
#include "../../renderer/atlas/AtlasEngine.h"
#include "../../types/inc/utils.hpp"
#include "../../tsf/Handle.h"
#include "HyperlinkFileTypeGroups.h"
#include "HyperlinkPreview.h"
#include "HyperlinkHtmlHost.h"

#include "TermControl.g.cpp"

using namespace ::Microsoft::Console::Types;
using namespace ::Microsoft::Console::VirtualTerminal;
using namespace ::Microsoft::Terminal::Core;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Automation::Peers;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::UI::Input;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Storage::Streams;

// The minimum delay between updates to the scroll bar's values.
// The updates are throttled to limit power usage.
constexpr const auto ScrollBarUpdateInterval = std::chrono::milliseconds(8);

// The minimum delay between updating the TSF input control.
// This is already throttled primarily in the ControlCore, with a timeout of 100ms. We're adding another smaller one here, as the (potentially x-proc) call will come in off the UI thread
constexpr const auto TsfRedrawInterval = std::chrono::milliseconds(8);

// The minimum delay between updating the locations of regex patterns
constexpr const auto UpdatePatternLocationsInterval = std::chrono::milliseconds(500);

// The minimum delay between emitting warning bells
constexpr const auto TerminalWarningBellInterval = std::chrono::milliseconds(1000);

constexpr std::wstring_view StateNormal{ L"Normal" };
constexpr std::wstring_view StateCollapsed{ L"Collapsed" };

DEFINE_ENUM_FLAG_OPERATORS(winrt::Microsoft::Terminal::Control::CopyFormat);
DEFINE_ENUM_FLAG_OPERATORS(winrt::Microsoft::Terminal::Control::MouseButtonState);

// InputPane::GetForCurrentView() does not reliably work for XAML islands,
// as it assumes that there's a 1:1 relationship between windows and threads.
//
// During testing, I found that the input pane shows up when touching into the terminal even if
// TryShow is never called. This surprised me, but I figured it's worth trying it without this.
static void setInputPaneVisibility(HWND hwnd, bool visible)
{
    static const auto inputPaneInterop = []() {
        return winrt::try_get_activation_factory<InputPane, IInputPaneInterop>();
    }();

    if (!inputPaneInterop)
    {
        return;
    }

    winrt::com_ptr<IInputPane2> inputPane;
    if (FAILED(inputPaneInterop->GetForWindow(hwnd, winrt::guid_of<IInputPane2>(), inputPane.put_void())))
    {
        return;
    }

    bool result;
    if (visible)
    {
        std::ignore = inputPane->TryShow(&result);
    }
    else
    {
        std::ignore = inputPane->TryHide(&result);
    }
}

static Microsoft::Console::TSF::Handle& GetTSFHandle()
{
    // NOTE: If we ever go back to 1 thread per 1 window,
    // you need to swap the `static` with a `thread_local`.
    static auto s_tsf = ::Microsoft::Console::TSF::Handle::Create();
    return s_tsf;
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    // A drive letter mapped to a WSL share (Z: -> \\wsl.localhost\Ubuntu) would otherwise
    // translate to /mnt/z/..., which does not exist inside WSL. Expand it to the share so the
    // UNC branch of _translatePathInPlace strips it to the distro's own path.
    static void _expandWslMappedDrive(std::wstring& fullPath)
    {
        if (fullPath.size() < 2 || fullPath.at(1) != L':')
        {
            return;
        }

        const wchar_t drive[]{ fullPath.at(0), L':', L'\0' };
        wchar_t remote[MAX_PATH];
        DWORD remoteLength = ARRAYSIZE(remote);
        if (WNetGetConnectionW(&drive[0], &remote[0], &remoteLength) != NO_ERROR)
        {
            return;
        }

        // The UNC branch matches its prefixes case-sensitively, so substitute the canonical spelling.
        static constexpr std::wstring_view wslSharePrefixes[] = { L"\\\\wsl.localhost\\", L"\\\\wsl$\\" };
        const std::wstring_view share{ &remote[0] };
        for (const auto prefix : wslSharePrefixes)
        {
            if (til::starts_with_insensitive_ascii(share, prefix))
            {
                fullPath.replace(0, 2, std::wstring{ prefix }.append(share.substr(prefix.size())));
                return;
            }
        }
    }

    static void _translatePathInPlace(std::wstring& fullPath, PathTranslationStyle translationStyle)
    {
        static constexpr wil::zwstring_view s_pathPrefixes[] = {
            {},
            /* WSL */ L"/mnt/",
            /* Cygwin */ L"/cygdrive/",
            /* MSYS2 */ L"/",
            /* MinGW */ {},
        };
        static constexpr wil::zwstring_view sSingleQuoteEscape = L"'\\''";
        static constexpr auto cchSingleQuoteEscape = sSingleQuoteEscape.size();

        if (translationStyle == PathTranslationStyle::None)
        {
            return;
        }

        if (translationStyle == PathTranslationStyle::WSL)
        {
            _expandWslMappedDrive(fullPath);
        }

        // All of the other path translation modes current result in /-delimited paths
        std::replace(fullPath.begin(), fullPath.end(), L'\\', L'/');

        // Escape single quotes, assuming translated paths are always quoted by a pair of single quotes.
        size_t pos = 0;
        while ((pos = fullPath.find(L'\'', pos)) != std::wstring::npos)
        {
            // ' -> '\'' (for POSIX shell)
            fullPath.replace(pos, 1, sSingleQuoteEscape);
            // Arithmetic overflow cannot occur here.
            pos += cchSingleQuoteEscape;
        }

        if (translationStyle == PathTranslationStyle::MinGW)
        {
            return;
        }

        if (fullPath.size() >= 2 && fullPath.at(1) == L':')
        {
            // C:/foo/bar -> Cc/foo/bar
            fullPath.at(1) = til::tolower_ascii(fullPath.at(0));
            // Cc/foo/bar -> [PREFIX]c/foo/bar
            fullPath.replace(0, 1, s_pathPrefixes[static_cast<int>(translationStyle)]);
        }
        else if (translationStyle == PathTranslationStyle::WSL)
        {
            // Stripping the UNC name and distribution prefix only applies to WSL.
            static constexpr std::wstring_view wslPathPrefixes[] = { L"//wsl.localhost/", L"//wsl$/" };
            for (auto prefix : wslPathPrefixes)
            {
                if (til::starts_with(fullPath, prefix))
                {
                    if (const auto idx = fullPath.find(L'/', prefix.size()); idx != std::wstring::npos)
                    {
                        // //wsl.localhost/Ubuntu-18.04/foo/bar -> /foo/bar
                        fullPath.erase(0, idx);
                    }
                    else
                    {
                        // //wsl.localhost/Ubuntu-18.04 -> /
                        fullPath = L"/";
                    }
                    break;
                }
            }
        }
    }

    TsfDataProvider::TsfDataProvider(TermControl* termControl) noexcept :
        _termControl{ termControl }
    {
    }

    STDMETHODIMP TsfDataProvider::QueryInterface(REFIID, void**) noexcept
    {
        return E_NOTIMPL;
    }

    ULONG STDMETHODCALLTYPE TsfDataProvider::AddRef() noexcept
    {
        return 1;
    }

    ULONG STDMETHODCALLTYPE TsfDataProvider::Release() noexcept
    {
        return 1;
    }

    HWND TsfDataProvider::GetHwnd()
    {
        if (!_hwnd)
        {
            // WinUI's WinRT based TSF runs in its own window "Windows.UI.Input.InputSite.WindowClass" (..."great")
            // and in order for us to snatch the focus away from that one we need to find its HWND.
            // The way we do it here is by finding the existing, active TSF context and getting the HWND from it.
            _hwnd = GetTSFHandle().FindWindowOfActiveTSF();
            if (!_hwnd)
            {
                _hwnd = reinterpret_cast<HWND>(_termControl->OwningHwnd());
            }
        }
        return _hwnd;
    }

    RECT TsfDataProvider::GetViewport()
    {
        const auto hwnd = reinterpret_cast<HWND>(_termControl->OwningHwnd());
        RECT clientRect;
        GetWindowRect(hwnd, &clientRect);
        return clientRect;
    }

    RECT TsfDataProvider::GetCursorPosition()
    {
        const auto core = _getCore();
        if (!core)
        {
            return {};
        }

        // TSF wants screen coordinates, and localOrigin below is relative to the root of our visual tree,
        // which sits at the origin of the owning window's client area. GH#20318: GetWindowRect() is not
        // that origin. While the window is maximized the client area starts below the resize borders, so
        // the rect ended up a resize handle height too high, which is enough for the IME candidate window
        // to cover the text being composed.
        const auto hwnd = reinterpret_cast<HWND>(_termControl->OwningHwnd());
        POINT windowOrigin{};
        ClientToScreen(hwnd, &windowOrigin);

        const auto scaleFactor = _termControl->SwapChainPanel().CompositionScaleX();
        const auto localOrigin = _termControl->TransformToVisual(nullptr).TransformPoint({});
        const auto padding = _termControl->GetPadding();
        const auto cursorPosition = core->CursorPosition();
        const auto fontSize = core->FontSize();

        // fontSize is not in DIPs, so we need to first multiply by scaleFactor and then do the rest.
        const auto left = windowOrigin.x + (localOrigin.X + static_cast<float>(padding.Left)) * scaleFactor + cursorPosition.X * fontSize.Width;
        const auto top = windowOrigin.y + (localOrigin.Y + static_cast<float>(padding.Top)) * scaleFactor + cursorPosition.Y * fontSize.Height;
        const auto right = left + fontSize.Width;
        const auto bottom = top + fontSize.Height;

        return {
            lroundf(left),
            lroundf(top),
            lroundf(right),
            lroundf(bottom),
        };
    }

    void TsfDataProvider::HandleOutput(std::wstring_view text)
    {
        const auto core = _getCore();
        if (!core)
        {
            return;
        }
        core->SendInput(text);
    }

    ::Microsoft::Console::Render::Renderer* TsfDataProvider::GetRenderer()
    {
        const auto core = _getCore();
        if (!core)
        {
            return nullptr;
        }
        return core->GetRenderer();
    }

    ControlCore* TsfDataProvider::_getCore() const noexcept
    {
        return get_self<ControlCore>(_termControl->_core);
    }

    static Windows::UI::ViewManagement::AccessibilitySettings& _GetAccessibilitySettings()
    {
        static Windows::UI::ViewManagement::AccessibilitySettings accessibilitySettings;
        return accessibilitySettings;
    }

    TermControl::TermControl(IControlSettings settings,
                             Control::IControlAppearance unfocusedAppearance,
                             TerminalConnection::ITerminalConnection connection) :
        TermControl{ winrt::make<implementation::ControlInteractivity>(settings, unfocusedAppearance, connection) }
    {
    }

    TermControl::TermControl(Control::ControlInteractivity content) :
        _interactivity{ content },
        _isInternalScrollBarUpdate{ false },
        _searchBox{ nullptr }
    {
        InitializeComponent();

        _core = _interactivity.Core();

        // If high contrast mode was changed, update the appearance appropriately.
        _core.SetHighContrastMode(_GetAccessibilitySettings().HighContrast());
        _revokers.HighContrastChanged = _GetAccessibilitySettings().HighContrastChanged(winrt::auto_revoke, [weakThis{ get_weak() }](const Windows::UI::ViewManagement::AccessibilitySettings& a11ySettings, auto&&) {
            if (auto termControl = weakThis.get())
            {
                termControl->_core.SetHighContrastMode(a11ySettings.HighContrast());
                termControl->_core.ApplyAppearance(termControl->_focused);
            }
        });

        // This event is specifically triggered by the renderer thread, a BG thread. Use a weak ref here.
        _revokers.RendererEnteredErrorState = _core.RendererEnteredErrorState(winrt::auto_revoke, { get_weak(), &TermControl::_RendererEnteredErrorState });

        // IMPORTANT! Set this callback up sooner rather than later. If we do it
        // after Enable, then it'll be possible to paint the frame once
        // _before_ the warning handler is set up, and then warnings from
        // the first paint will be ignored!
        _revokers.RendererWarning = _core.RendererWarning(winrt::auto_revoke, { get_weak(), &TermControl::_RendererWarning });
        // ALSO IMPORTANT: Make sure to set this callback up in the ctor, so
        // that we won't miss any swap chain changes.
        _revokers.SwapChainChanged = _core.SwapChainChanged(winrt::auto_revoke, { get_weak(), &TermControl::RenderEngineSwapChainChanged });

        // These callbacks can only really be triggered by UI interactions. So
        // they don't need weak refs - they can't be triggered unless we're
        // alive.
        _revokers.BackgroundColorChanged = _core.BackgroundColorChanged(winrt::auto_revoke, { get_weak(), &TermControl::_coreBackgroundColorChanged });
        _revokers.FontSizeChanged = _core.FontSizeChanged(winrt::auto_revoke, { get_weak(), &TermControl::_coreFontSizeChanged });
        _revokers.TransparencyChanged = _core.TransparencyChanged(winrt::auto_revoke, { get_weak(), &TermControl::_coreTransparencyChanged });
        _revokers.RaiseNotice = _core.RaiseNotice(winrt::auto_revoke, { get_weak(), &TermControl::_coreRaisedNotice });
        _revokers.HoveredHyperlinkChanged = _core.HoveredHyperlinkChanged(winrt::auto_revoke, { get_weak(), &TermControl::_hoveredHyperlinkChanged });
        _revokers.OutputIdle = _core.OutputIdle(winrt::auto_revoke, { get_weak(), &TermControl::_coreOutputIdle });
        _revokers.UpdateSelectionMarkers = _core.UpdateSelectionMarkers(winrt::auto_revoke, { get_weak(), &TermControl::_updateSelectionMarkers });
        _revokers.coreOpenHyperlink = _core.OpenHyperlink(winrt::auto_revoke, { get_weak(), &TermControl::_HyperlinkHandler });
        _revokers.interactivityOpenHyperlink = _interactivity.OpenHyperlink(winrt::auto_revoke, { get_weak(), &TermControl::_HyperlinkHandler });
        _revokers.interactivityScrollPositionChanged = _interactivity.ScrollPositionChanged(winrt::auto_revoke, { get_weak(), &TermControl::_ScrollPositionChanged });
        _revokers.ContextMenuRequested = _interactivity.ContextMenuRequested(winrt::auto_revoke, { get_weak(), &TermControl::_contextMenuHandler });

        // "Bubbled" events - ones we want to handle, by raising our own event.
        _revokers.TitleChanged = _core.TitleChanged(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleTitleChanged });
        _revokers.TabColorChanged = _core.TabColorChanged(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleTabColorChanged });
        _revokers.TaskbarProgressChanged = _core.TaskbarProgressChanged(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleSetTaskbarProgress });
        _revokers.ConnectionStateChanged = _core.ConnectionStateChanged(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleConnectionStateChanged });
        _revokers.ShowWindowChanged = _core.ShowWindowChanged(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleShowWindowChanged });
        _revokers.CloseTerminalRequested = _core.CloseTerminalRequested(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleCloseTerminalRequested });
        _revokers.CompletionsChanged = _core.CompletionsChanged(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleCompletionsChanged });
        _revokers.RestartTerminalRequested = _core.RestartTerminalRequested(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleRestartTerminalRequested });
        _revokers.SearchMissingCommand = _core.SearchMissingCommand(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleSearchMissingCommand });
        _revokers.ShowNotification = _core.ShowNotification(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleShowNotification });
        _revokers.WindowSizeChanged = _core.WindowSizeChanged(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleWindowSizeChanged });
        _revokers.WriteToClipboard = _core.WriteToClipboard(winrt::auto_revoke, { get_weak(), &TermControl::_bubbleWriteToClipboard });

        _revokers.PasteFromClipboard = _interactivity.PasteFromClipboard(winrt::auto_revoke, { get_weak(), &TermControl::_bubblePasteFromClipboard });

        _revokers.RefreshQuickFixUI = _core.RefreshQuickFixUI(winrt::auto_revoke, [this](auto /*s*/, auto /*e*/) {
            RefreshQuickFixMenu();
        });

        // Initialize the terminal only once the swapchainpanel is loaded - that
        //      way, we'll be able to query the real pixel size it got on layout
        _layoutUpdatedRevoker = SwapChainPanel().LayoutUpdated(winrt::auto_revoke, [this](auto /*s*/, auto /*e*/) {
            // This event fires every time the layout changes, but it is always the last one to fire
            // in any layout change chain. That gives us great flexibility in finding the right point
            // at which to initialize our renderer (and our terminal).
            // Any earlier than the last layout update and we may not know the terminal's starting size.
            if (_InitializeTerminal(InitializeReason::Create))
            {
                // Only let this succeed once.
                _layoutUpdatedRevoker.revoke();
            }
        });

        // Get our dispatcher. This will get us the same dispatcher as
        // TermControl::Dispatcher().
        auto dispatcher = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();

        // These three throttled functions are triggered by terminal output and interact with the UI.
        // Since Close() is the point after which we are removed from the UI, but before the
        // destructor has run, we MUST check control->_IsClosing() before actually doing anything.
        _playWarningBell = std::make_shared<ThrottledFunc<>>(
            dispatcher,
            til::throttled_func_options{
                .delay = TerminalWarningBellInterval,
                .leading = true,
            },
            [weakThis = get_weak()]() {
                if (auto control{ weakThis.get() }; control && !control->_IsClosing())
                {
                    control->WarningBell.raise(*control, nullptr);
                }
            });

        _updateScrollBar = std::make_shared<ThrottledFunc<ScrollBarUpdate>>(
            dispatcher,
            til::throttled_func_options{
                .delay = ScrollBarUpdateInterval,
                .trailing = true,
            },
            [weakThis = get_weak()](const auto& update) {
                if (auto control{ weakThis.get() }; control && !control->_IsClosing())
                {
                    control->_throttledUpdateScrollbar(update);
                }
            });

        // These events might all be triggered by the connection, but that
        // should be drained and closed before we complete destruction. So these
        // are safe.
        //
        // NOTE: _ScrollPositionChanged has to be registered after we set up the
        // _updateScrollBar func. Otherwise, we could get a callback from an
        // attached content before we set up the throttled func, and that'll A/V
        _revokers.coreScrollPositionChanged = _core.ScrollPositionChanged(winrt::auto_revoke, { get_weak(), &TermControl::_ScrollPositionChanged });
        _revokers.WarningBell = _core.WarningBell(winrt::auto_revoke, { get_weak(), &TermControl::_coreWarningBell });

        _ApplyUISettings();

        _originalPrimaryElements = winrt::single_threaded_observable_vector<Controls::ICommandBarElement>();
        _originalSecondaryElements = winrt::single_threaded_observable_vector<Controls::ICommandBarElement>();
        _originalSelectedPrimaryElements = winrt::single_threaded_observable_vector<Controls::ICommandBarElement>();
        _originalSelectedSecondaryElements = winrt::single_threaded_observable_vector<Controls::ICommandBarElement>();
        for (const auto& e : ContextMenu().PrimaryCommands())
        {
            _originalPrimaryElements.Append(e);
        }
        for (const auto& e : ContextMenu().SecondaryCommands())
        {
            _originalSecondaryElements.Append(e);
        }
        for (const auto& e : SelectionContextMenu().PrimaryCommands())
        {
            _originalSelectedPrimaryElements.Append(e);
        }
        for (const auto& e : SelectionContextMenu().SecondaryCommands())
        {
            _originalSelectedSecondaryElements.Append(e);
        }
        ContextMenu().Closed([weakThis = get_weak()](auto&&, auto&&) {
            if (auto control{ weakThis.get() }; control && !control->_IsClosing())
            {
                const auto& menu{ control->ContextMenu() };
                menu.PrimaryCommands().Clear();
                menu.SecondaryCommands().Clear();
                for (const auto& e : control->_originalPrimaryElements)
                {
                    menu.PrimaryCommands().Append(e);
                }
                for (const auto& e : control->_originalSecondaryElements)
                {
                    menu.SecondaryCommands().Append(e);
                }
            }
        });
        SelectionContextMenu().Closed([weakThis = get_weak()](auto&&, auto&&) {
            if (auto control{ weakThis.get() }; control && !control->_IsClosing())
            {
                const auto& menu{ control->SelectionContextMenu() };
                menu.PrimaryCommands().Clear();
                menu.SecondaryCommands().Clear();
                for (const auto& e : control->_originalSelectedPrimaryElements)
                {
                    menu.PrimaryCommands().Append(e);
                }
                for (const auto& e : control->_originalSelectedSecondaryElements)
                {
                    menu.SecondaryCommands().Append(e);
                }
            }
        });
        if constexpr (Feature_QuickFix::IsEnabled())
        {
            QuickFixMenu().Closed([weakThis = get_weak()](auto&&, auto&&) {
                if (auto control{ weakThis.get() }; control && !control->_IsClosing())
                {
                    // Expand the quick fix button if it's collapsed (looks nicer)
                    if (control->_quickFixButtonCollapsible)
                    {
                        VisualStateManager::GoToState(*control, StateCollapsed, false);
                    }
                }
            });
        }
    }

    void TermControl::_QuickFixButton_PointerEntered(const IInspectable& /*sender*/, const PointerRoutedEventArgs& /*e*/)
    {
        if (!_IsClosing() && _quickFixButtonCollapsible)
        {
            VisualStateManager::GoToState(*this, StateNormal, false);
        }
    }

    void TermControl::_QuickFixButton_PointerExited(const IInspectable& /*sender*/, const PointerRoutedEventArgs& /*e*/)
    {
        if (!_IsClosing() && _quickFixButtonCollapsible)
        {
            VisualStateManager::GoToState(*this, StateCollapsed, false);
        }
    }

    // Function Description:
    // - Static helper for building a new TermControl from an already existing
    //   content. We'll attach the existing swapchain to this new control's
    //   SwapChainPanel.
    // Arguments:
    // - content: The preexisting ControlInteractivity to connect to.
    // Return Value:
    // - The newly constructed TermControl.
    Control::TermControl TermControl::NewControlByAttachingContent(Control::ControlInteractivity content)
    {
        const auto term{ winrt::make_self<TermControl>(content) };
        term->_initializeForAttach();
        return *term;
    }

    void TermControl::_initializeForAttach()
    {
        _AttachDxgiSwapChainToXaml(reinterpret_cast<HANDLE>(_core.SwapChainHandle()));
        _interactivity.AttachToNewControl();

        // Initialize the terminal only once the swapchainpanel is loaded - that
        //      way, we'll be able to query the real pixel size it got on layout
        auto r = SwapChainPanel().LayoutUpdated(winrt::auto_revoke, [this](auto /*s*/, auto /*e*/) {
            // Replace the normal initialize routine with one that will allow up
            // to complete initialization even though the Core was already
            // initialized.
            if (_InitializeTerminal(InitializeReason::Reattach))
            {
                // Only let this succeed once.
                _layoutUpdatedRevoker.revoke();
            }
        });
        _layoutUpdatedRevoker.swap(r);
    }

    uint64_t TermControl::ContentId() const
    {
        return _interactivity.Id();
    }

    TerminalConnection::ITerminalConnection TermControl::Connection()
    {
        return _core.Connection();
    }
    void TermControl::Connection(const TerminalConnection::ITerminalConnection& newConnection)
    {
        _core.Connection(newConnection);
    }

    void TermControl::HardResetWithoutErase()
    {
        _core.HardResetWithoutErase();
    }

    void TermControl::_throttledUpdateScrollbar(const ScrollBarUpdate& update)
    {
        if (!_initializedTerminal)
        {
            return;
        }

        // Assumptions:
        // * we're already not closing
        // * caller already checked weak ptr to make sure we're still alive

        _isInternalScrollBarUpdate = true;

        auto scrollBar = ScrollBar();
        if (update.newValue)
        {
            scrollBar.Value(*update.newValue);
        }
        scrollBar.Maximum(update.newMaximum);
        scrollBar.Minimum(update.newMinimum);
        scrollBar.ViewportSize(update.newViewportSize);
        // scroll one full screen worth at a time when the scroll bar is clicked
        scrollBar.LargeChange(std::max(update.newViewportSize - 1, 0.));

        _isInternalScrollBarUpdate = false;

        if (_showMarksInScrollbar)
        {
            const auto scaleFactor = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
            const auto scrollBarWidthInDIP = scrollBar.ActualWidth();
            const auto scrollBarHeightInDIP = scrollBar.ActualHeight();
            const auto scrollBarWidthInPx = gsl::narrow_cast<int32_t>(lrint(scrollBarWidthInDIP * scaleFactor));
            const auto scrollBarHeightInPx = gsl::narrow_cast<int32_t>(lrint(scrollBarHeightInDIP * scaleFactor));

            const auto canvas = FindName(L"ScrollBarCanvas").as<Controls::Image>();
            auto source = canvas.Source().try_as<Media::Imaging::WriteableBitmap>();

            if (!source || scrollBarWidthInPx != source.PixelWidth() || scrollBarHeightInPx != source.PixelHeight())
            {
                source = Media::Imaging::WriteableBitmap{ scrollBarWidthInPx, scrollBarHeightInPx };
                canvas.Source(source);
                canvas.Width(scrollBarWidthInDIP);
                canvas.Height(scrollBarHeightInDIP);
            }

            const auto buffer = source.PixelBuffer();
            const auto data = buffer.data();
            const auto stride = scrollBarWidthInPx * sizeof(til::color);

            // The bitmap has the size of the entire scrollbar, but we want the marks to only show in the range that the "thumb"
            // (the scroll indicator) can move. That's why we need to add an offset to the start of the drawable bitmap area
            // (to offset the decrease button) and subtract twice that (to offset the increase button as well).
            //
            // The WinUI standard scrollbar defines a Margin="2,0,2,0" for the "VerticalPanningThumb" and a Padding="0,4,0,0"
            // for the "VerticalDecrementTemplate" (and similar for the increment), but it seems neither of those is correct,
            // because a padding for 3 DIPs seem to be the exact right amount to add.
            const auto increaseDecreaseButtonHeight = scrollBarWidthInPx + lround(3 * scaleFactor);
            const auto drawableDataStart = data + stride * increaseDecreaseButtonHeight;
            const auto drawableRange = scrollBarHeightInPx - 2 * increaseDecreaseButtonHeight;

            // Protect the remaining code against negative offsets. This normally can't happen
            // and this code just exists so it doesn't crash if I'm ever wrong about this.
            // (The window has a min. size that ensures that there's always a scrollbar thumb.)
            if (drawableRange < 0)
            {
                return;
            }

            // The scrollbar bitmap is divided into 3 evenly sized stripes:
            // Left: Regular marks
            // Center: nothing
            // Right: Search marks
            const auto pipWidth = (scrollBarWidthInPx + 1) / 3;
            const auto pipHeight = lround(1 * scaleFactor);

            const auto maxOffsetY = drawableRange - pipHeight;
            const auto offsetScale = maxOffsetY / gsl::narrow_cast<float>(update.newMaximum + update.newViewportSize);
            // A helper to turn a TextBuffer row offset into a bitmap offset.
            const auto dataAt = [&](til::CoordType row) [[msvc::forceinline]] {
                const auto y = std::clamp<long>(lrintf(row * offsetScale), 0, maxOffsetY);
                return drawableDataStart + stride * y;
            };
            // A helper to draw a single pip (mark) at the given location.
            const auto drawPip = [&](uint8_t* beg, til::color color) [[msvc::forceinline]] {
                const auto end = beg + pipHeight * stride;
                for (; beg < end; beg += stride)
                {
                    // a til::color does NOT have the same RGBA format as the bitmap.
#pragma warning(suppress : 26490) // Don't use reinterpret_cast (type.1).
                    const DWORD c = 0xff << 24 | color.r << 16 | color.g << 8 | color.b;
                    std::fill_n(reinterpret_cast<DWORD*>(beg), pipWidth, c);
                }
            };

            memset(data, 0, buffer.Length());

            if (const auto marks = _core.ScrollMarks())
            {
                for (const auto& m : marks)
                {
                    const auto row = m.Row;
                    const til::color color{ m.Color.Color };
                    const auto base = dataAt(row);
                    drawPip(base, color);
                }
            }

            if (_searchBox && _searchBox->IsOpen())
            {
                const auto core = winrt::get_self<ControlCore>(_core);
                const auto& searchMatches = core->SearchResultRows();
                const auto color = core->ForegroundColor();
                const auto rightAlignedOffset = (scrollBarWidthInPx - pipWidth) * sizeof(til::color);
                til::CoordType lastRow = til::CoordTypeMin;

                for (const auto& span : searchMatches)
                {
                    if (lastRow != span.start.y)
                    {
                        lastRow = span.start.y;
                        const auto base = dataAt(lastRow) + rightAlignedOffset;
                        drawPip(base, color);
                    }
                }
            }

            source.Invalidate();
            canvas.Visibility(Visibility::Visible);
        }
    }

    // Method Description:
    // - Loads the search box from the xaml UI and focuses it.
    void TermControl::CreateSearchBoxControl()
    {
        // Lazy load the search box control.
        if (auto loadedSearchBox{ FindName(L"SearchBox") })
        {
            if (auto searchBox{ loadedSearchBox.try_as<::winrt::Microsoft::Terminal::Control::SearchBoxControl>() })
            {
                // get at its private implementation
                _searchBox.copy_from(winrt::get_self<implementation::SearchBoxControl>(searchBox));

                // If a text is selected inside terminal, use it to populate the search box.
                // If the search box already contains a value, it will be overridden.
                if (_core.HasSelection())
                {
                    // Currently we populate the search box only if a single line is selected.
                    // Empirically, multi-line selection works as well on sample scenarios,
                    // but since code paths differ, extra work is required to ensure correctness.
                    if (!_core.HasMultiLineSelection())
                    {
                        const auto selectedLine{ _core.SelectedText(true) };
                        _searchBox->PopulateTextbox(selectedLine);
                    }
                }

                _searchBox->Open([weakThis = get_weak()]() {
                    if (const auto self = weakThis.get(); self && !self->_IsClosing())
                    {
                        self->_searchScrollOffset = self->_calculateSearchScrollOffset();
                        self->_searchBox->SetFocusOnTextbox();
                        self->_refreshSearch();
                    }
                });
            }
        }
    }

    // This is called when a Find Next/Previous Match action is triggered.
    void TermControl::SearchMatch(const bool goForward)
    {
        if (_IsClosing())
        {
            return;
        }
        if (!_searchBox || !_searchBox->IsOpen())
        {
            CreateSearchBoxControl();
        }
        else
        {
            _handleSearchResults(_core.Search(SearchRequest{
                .Text = _searchBox->Text(),
                .GoForward = goForward,
                .CaseSensitive = _searchBox->CaseSensitive(),
                .RegularExpression = _searchBox->RegularExpression(),
                .ExecuteSearch = true,
                .ScrollIntoView = true,
                .ScrollOffset = _searchScrollOffset,
            }));
        }
    }

    // Method Description:
    // Find if search box text edit currently is in focus
    // Return Value:
    // - true, if search box text edit is in focus
    bool TermControl::SearchBoxEditInFocus() const
    {
        if (!_searchBox)
        {
            return false;
        }

        return _searchBox->TextBox().FocusState() == FocusState::Keyboard;
    }

    // Method Description:
    // - Search text in text buffer. This is triggered if the user clicks the
    //   search button, presses enter, or changes the search criteria.
    // Arguments:
    // - text: the text to search
    // - goForward: boolean that represents if the current search direction is forward
    // - caseSensitive: boolean that represents if the current search is case-sensitive
    // Return Value:
    // - <none>
    void TermControl::_Search(const winrt::hstring& text,
                              const bool goForward,
                              const bool caseSensitive,
                              const bool regularExpression)
    {
        if (_searchBox && _searchBox->IsOpen())
        {
            _handleSearchResults(_core.Search(SearchRequest{
                .Text = text,
                .GoForward = goForward,
                .CaseSensitive = caseSensitive,
                .RegularExpression = regularExpression,
                .ExecuteSearch = true,
                .ScrollIntoView = true,
                .ScrollOffset = _searchScrollOffset,
            }));
        }
    }

    // Method Description:
    // - The handler for the "search criteria changed" event. Initiates a new search.
    // Arguments:
    // - text: the text to search
    // - goForward: indicates whether the search should be performed forward (if set to true) or backward
    // - caseSensitive: boolean that represents if the current search is case-sensitive
    // Return Value:
    // - <none>
    void TermControl::_SearchChanged(const winrt::hstring& text,
                                     const bool goForward,
                                     const bool caseSensitive,
                                     const bool regularExpression)
    {
        if (_searchBox && _searchBox->IsOpen())
        {
            _handleSearchResults(_core.Search(SearchRequest{
                .Text = text,
                .GoForward = goForward,
                .CaseSensitive = caseSensitive,
                .RegularExpression = regularExpression,
                .ExecuteSearch = false,
                .ScrollIntoView = true,
                .ScrollOffset = _searchScrollOffset,
            }));
        }
    }

    // Method Description:
    // - The handler for the close button or pressing "Esc" when focusing on the
    //   search dialog.
    // Arguments:
    // - IInspectable: not used
    // - RoutedEventArgs: not used
    // Return Value:
    // - <none>
    void TermControl::_CloseSearchBoxControl(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                             const RoutedEventArgs& /*args*/)
    {
        _searchBox->Close();
        _core.ClearSearch();

        // Clear search highlights scroll marks (by triggering an update after closing the search box)
        if (_showMarksInScrollbar)
        {
            const auto scrollBar = ScrollBar();
            ScrollBarUpdate update{
                .newValue = scrollBar.Value(),
                .newMaximum = scrollBar.Maximum(),
                .newMinimum = scrollBar.Minimum(),
                .newViewportSize = scrollBar.ViewportSize(),
            };
            _updateScrollBar->Run(update);
        }

        // Set focus back to terminal control
        this->Focus(FocusState::Programmatic);
    }

    void TermControl::UpdateControlSettings(IControlSettings settings)
    {
        UpdateControlSettings(settings, _core.UnfocusedAppearance());
    }
    // Method Description:
    // - Given Settings having been updated, applies the settings to the current terminal.
    // Return Value:
    // - <none>
    void TermControl::UpdateControlSettings(IControlSettings settings, IControlAppearance unfocusedAppearance)
    {
        _core.UpdateSettings(settings, unfocusedAppearance);

        _UpdateSettingsFromUIThread();

        _UpdateAppearanceFromUIThread(_focused ? _core.FocusedAppearance() : _core.UnfocusedAppearance());
    }

    // Method Description:
    // - Dispatches a call to the UI thread and updates the appearance
    // Arguments:
    // - newAppearance: the new appearance to set
    void TermControl::UpdateAppearance(IControlAppearance newAppearance)
    {
        _UpdateAppearanceFromUIThread(newAppearance);
    }

    // Method Description:
    // - Updates the settings of the current terminal.
    // - This method is separate from UpdateSettings because there is an apparent optimizer
    //   issue that causes one of our hstring -> wstring_view conversions to result in garbage,
    //   but only from a coroutine context. See GH#8723.
    // - INVARIANT: This method must be called from the UI thread.
    // Arguments:
    // - newSettings: the new settings to set
    void TermControl::_UpdateSettingsFromUIThread()
    {
        if (_IsClosing())
        {
            return;
        }

        // Update our control settings
        _ApplyUISettings();
    }

    // Method Description:
    // - Updates the appearance
    // - INVARIANT: This method must be called from the UI thread.
    // Arguments:
    // - newAppearance: the new appearance to set
    void TermControl::_UpdateAppearanceFromUIThread(Control::IControlAppearance newAppearance)
    {
        if (_IsClosing())
        {
            return;
        }

        _SetBackgroundImage(newAppearance);

        // Update our control settings
        const auto bg = newAppearance.DefaultBackground();

        // In the future, this might need to be changed to a
        // _InitializeBackgroundBrush call instead, because we may need to
        // switch from a solid color brush to an acrylic one.
        _changeBackgroundColor(bg);

        // Update selection markers
        Windows::UI::Xaml::Media::SolidColorBrush cursorColorBrush{ til::color{ newAppearance.CursorColor() } };
        SelectionStartMarker().Fill(cursorColorBrush);
        SelectionEndMarker().Fill(cursorColorBrush);

        _core.ApplyAppearance(_focused);
    }

    // Method Description:
    // - Writes the given sequence as input to the active terminal connection,
    // Arguments:
    // - wstr: the string of characters to write to the terminal connection.
    // Return Value:
    // - <none>
    void TermControl::SendInput(const winrt::hstring& wstr)
    {
        // Dismiss any previewed input.
        PreviewInput(hstring{});

        // only broadcast if there's an actual listener. Saves the overhead of some object creation.
        if (StringSent)
        {
            StringSent.raise(*this, winrt::make<StringSentEventArgs>(wstr));
        }

        RawWriteString(wstr);
    }
    void TermControl::ClearBuffer(Control::ClearBufferType clearType)
    {
        _core.ClearBuffer(clearType);
    }

    void TermControl::ToggleShaderEffects()
    {
        _core.ToggleShaderEffects();
    }

    // Method Description:
    // - Style our UI elements based on the values in our settings, and set up
    //   other control-specific settings. This method will be called whenever
    //   the settings are reloaded.
    //   * Calls _InitializeBackgroundBrush to set up the Xaml brush responsible
    //     for the control's background
    //   * Calls _BackgroundColorChanged to style the background of the control
    // - Core settings will be passed to the terminal in _InitializeTerminal
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TermControl::_ApplyUISettings()
    {
        _InitializeBackgroundBrush();

        // settings might be out-of-proc in the future
        auto settings{ _core.Settings() };

        // Learn this pane's WSL home before anyone hovers a "~/..." path in it. Hovering
        // resolves on this thread and so only reads the cache; the read itself goes to
        // the thread pool. The distribution is already running -- this pane runs in it --
        // so reading its /etc/passwd starts nothing.
        ::Microsoft::Console::Utils::PrewarmWslHomeDirectory(::Microsoft::Console::Utils::WslDistroForCommandline(
            settings.Commandline(),
            settings.PathTranslationStyle() == PathTranslationStyle::WSL));

        // Apply padding as swapChainPanel's margin
        const auto newMargin = StringToXamlThickness(settings.Padding());
        SwapChainPanel().Margin(newMargin);

        // Apply settings for scrollbar
        if (settings.ScrollState() == ScrollbarState::Hidden)
        {
            // In the scenario where the user has turned off the OS setting to automatically hide scrollbars, the
            // Terminal scrollbar would still be visible; so, we need to set the control's visibility accordingly to
            // achieve the intended effect.
            ScrollBar().IndicatorMode(Controls::Primitives::ScrollingIndicatorMode::None);
            ScrollBar().Visibility(Visibility::Collapsed);
        }
        else // (default or Visible)
        {
            // Default behavior
            ScrollBar().IndicatorMode(Controls::Primitives::ScrollingIndicatorMode::MouseIndicator);
            ScrollBar().Visibility(Visibility::Visible);
        }

        // The hyperlink tooltip tells you which gesture opens the link, so it has to
        // follow the chord that decides it. The XAML's x:Uid seeds the Run with the
        // localized Ctrl+Click wording; keep a copy of that so a chord of Ctrl+click
        // restores it in the user's language, and not in English.
        if (_followLinkHintCtrlClick.empty())
        {
            _followLinkHintCtrlClick = HowToOpenRun().Text();
        }
        HowToOpenRun().Text(_followLinkHintText(settings));

        // Zero or less means "don't wrap at all", which is only sensible on a very wide
        // window but is the honest reading of "no maximum".
        const auto tooltipMaxWidth = settings.HyperlinkTooltipMaxWidth();
        HyperlinkCard().MaxWidth(tooltipMaxWidth > 0 ? static_cast<double>(tooltipMaxWidth) :
                                                       std::numeric_limits<double>::infinity());
        // HyperlinkCardActions' own children's Visibility -- and HyperlinkCardCustomActions'
        // contents -- are recomputed per-hover in _hoveredHyperlinkChanged instead of once
        // here, since a HyperlinkTooltipRule can only be evaluated once a specific link is
        // known.

        _interactivity.UpdateSettings();
        {
            const auto inputScope = settings.DefaultInputScope();
            const auto alpha = inputScope == DefaultInputScope::AlphanumericHalfWidth;
            ::Microsoft::Console::TSF::Handle::SetDefaultScopeAlphanumericHalfWidth(alpha);
        }
        if (_automationPeer)
        {
            _automationPeer.SetControlPadding(Core::Padding{
                static_cast<float>(newMargin.Left),
                static_cast<float>(newMargin.Top),
                static_cast<float>(newMargin.Right),
                static_cast<float>(newMargin.Bottom),
            });
        }

        _showMarksInScrollbar = settings.ShowMarks();
        // Hide all scrollbar marks since they might be disabled now.
        if (const auto canvas = ScrollBarCanvas())
        {
            canvas.Visibility(Visibility::Collapsed);
        }
        // When we hot reload the settings, the core will send us a scrollbar
        // update. If we enabled scrollbar marks, then great, when we handle
        // that message, we'll redraw them.

        // update the position of the quick fix menu (in case we changed the padding)
        RefreshQuickFixMenu();
    }

    // Method Description:
    // - Sets background image and applies its settings (stretch, opacity and alignment)
    // - Checks path validity
    // Arguments:
    // - newAppearance
    // Return Value:
    // - <none>
    void TermControl::_SetBackgroundImage(const IControlAppearance& newAppearance)
    {
        if (newAppearance.BackgroundImage().empty() || _core.Settings().UseBackgroundImageForWindow())
        {
            BackgroundImage().Source(nullptr);
            return;
        }

        Windows::Foundation::Uri imageUri{ nullptr };
        try
        {
            imageUri = Windows::Foundation::Uri{ newAppearance.BackgroundImage() };
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            BackgroundImage().Source(nullptr);
            return;
        }

        // Check if the image brush is already pointing to the image
        // in the modified settings; if it isn't (or isn't there),
        // set a new image source for the brush
        auto imageSource = BackgroundImage().Source().try_as<Media::Imaging::BitmapImage>();

        if (imageSource == nullptr ||
            imageSource.UriSource() == nullptr ||
            !imageSource.UriSource().Equals(imageUri))
        {
            // Note that BitmapImage handles the image load asynchronously,
            // which is especially important since the image
            // may well be both large and somewhere out on the
            // internet.
            Media::Imaging::BitmapImage image(imageUri);
            BackgroundImage().Source(image);
        }

        // Apply stretch, opacity and alignment settings
        BackgroundImage().Stretch(newAppearance.BackgroundImageStretchMode());
        BackgroundImage().Opacity(newAppearance.BackgroundImageOpacity());
        BackgroundImage().HorizontalAlignment(newAppearance.BackgroundImageHorizontalAlignment());
        BackgroundImage().VerticalAlignment(newAppearance.BackgroundImageVerticalAlignment());
    }

    // Method Description:
    // - Set up each layer's brush used to display the control's background.
    // - Respects the settings for acrylic, background image and opacity from
    //   _settings.
    //   * If acrylic is not enabled, set up a solid color background; otherwise,
    //       use bgcolor as acrylic's tint
    // - Avoids image flickering and acrylic brush redraw if settings are changed
    //   but the appropriate brush is still in place.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void TermControl::_InitializeBackgroundBrush()
    {
        auto settings{ _core.Settings() };
        auto bgColor = til::color{ _core.FocusedAppearance().DefaultBackground() };

        auto transparentBg = settings.UseBackgroundImageForWindow();
        if (transparentBg)
        {
            bgColor = Windows::UI::Colors::Transparent();
        }
        // GH#11743: Make sure to use the Core's current UseAcrylic value, not
        // the one from the settings. The Core's runtime UseAcrylic may have
        // changed from what was in the original settings.
        if (_core.UseAcrylic() && !transparentBg)
        {
            // See if we've already got an acrylic background brush
            // to avoid the flicker when setting up a new one
            auto acrylic = RootGrid().Background().try_as<Media::AcrylicBrush>();

            // Instantiate a brush if there's not already one there
            if (acrylic == nullptr)
            {
                acrylic = Media::AcrylicBrush{};
            }

            const auto backdropStyle =
                _core.Settings().EnableUnfocusedAcrylic() ? Media::AcrylicBackgroundSource::Backdrop : Media::AcrylicBackgroundSource::HostBackdrop;
            acrylic.BackgroundSource(backdropStyle);

            // see GH#1082: Initialize background color so we don't get a
            // fade/flash when _BackgroundColorChanged is called
            acrylic.FallbackColor(bgColor);
            acrylic.TintColor(bgColor);

            // Apply brush settings
            acrylic.TintOpacity(_core.Opacity());

            // Apply brush to control if it's not already there
            if (RootGrid().Background() != acrylic)
            {
                RootGrid().Background(acrylic);
            }
        }
        else
        {
            Media::SolidColorBrush solidColor{};
            solidColor.Opacity(_core.Opacity());
            solidColor.Color(bgColor);

            RootGrid().Background(solidColor);
        }

        BackgroundBrush(RootGrid().Background());
    }

    // Method Description:
    // - Handler for the core's BackgroundColorChanged event. Updates the color
    //   of our background brush to match.
    // - Hops over to the UI thread to do this work.
    // Arguments:
    // <unused>
    // Return Value:
    // - <none>
    safe_void_coroutine TermControl::_coreBackgroundColorChanged(const IInspectable& /*sender*/,
                                                                 const IInspectable& /*args*/)
    {
        auto weakThis{ get_weak() };
        co_await wil::resume_foreground(Dispatcher());
        if (auto control{ weakThis.get() })
        {
            til::color newBgColor{ _core.BackgroundColor() };
            _changeBackgroundColor(newBgColor);
        }
    }

    // Method Description:
    // - Update the color of the background brush we're using. This does _not_
    //   update the opacity, or what type of brush it is.
    // - INVARIANT: This needs to be called on the UI thread.
    // Arguments:
    // - bg: the new color to use as the background color.
    void TermControl::_changeBackgroundColor(til::color bg)
    {
        auto transparent_bg = _core.Settings().UseBackgroundImageForWindow();
        if (transparent_bg)
        {
            bg = Windows::UI::Colors::Transparent();
        }

        if (auto acrylic = RootGrid().Background().try_as<Media::AcrylicBrush>())
        {
            acrylic.FallbackColor(bg);
            acrylic.TintColor(bg);
        }
        else if (auto solidColor = RootGrid().Background().try_as<Media::SolidColorBrush>())
        {
            solidColor.Color(bg);
        }

        BackgroundBrush(RootGrid().Background());

        // Don't use the normal BackgroundBrush() Observable Property setter
        // here. (e.g. `BackgroundBrush()`). The one from the macro will
        // automatically ignore changes where the value doesn't _actually_
        // change. In our case, most of the time when changing the colors of the
        // background, the _Brush_ itself doesn't change, we simply change the
        // Color() of the brush. This results in the event not getting bubbled
        // up.
        //
        // Firing it manually makes sure it does.
        _BackgroundBrush = RootGrid().Background();
        PropertyChanged.raise(*this, Data::PropertyChangedEventArgs{ L"BackgroundBrush" });

        _isBackgroundLight = _isColorLight(bg);
    }

    bool TermControl::_isColorLight(til::color bg) noexcept
    {
        // Checks if the current background color is light enough
        // to need a dark version of the visual bell indicator
        // This is a poor man's Rec. 601 luma.
        const auto l = 30 * bg.r + 59 * bg.g + 11 * bg.b;
        return l > 12750;
    }

    // Method Description:
    // - Update the opacity of the background brush we're using. This does _not_
    //   update the color, or what type of brush it is.
    // - INVARIANT: This needs to be called on the UI thread.
    void TermControl::_changeBackgroundOpacity()
    {
        const auto opacity{ _core.Opacity() };
        const auto useAcrylic{ _core.UseAcrylic() };
        auto changed = false;
        // GH#11743, #11619: If we're changing whether or not acrylic is used,
        // then just entirely reinitialize the brush. The primary way that this
        // happens is on Windows 10, where we need to enable acrylic when the
        // user asks for <100% opacity. Even when we remove this Windows 10
        // fallback, we may still need this for something like changing if
        // acrylic is enabled at runtime (GH#2531)
        if (auto acrylic = RootGrid().Background().try_as<Media::AcrylicBrush>())
        {
            if (!useAcrylic)
            {
                _InitializeBackgroundBrush();
                return;
            }
            changed = acrylic.TintOpacity() != opacity;
            acrylic.TintOpacity(opacity);
        }
        else if (auto solidColor = RootGrid().Background().try_as<Media::SolidColorBrush>())
        {
            if (useAcrylic)
            {
                _InitializeBackgroundBrush();
                return;
            }
            changed = solidColor.Opacity() != opacity;
            solidColor.Opacity(opacity);
        }
        // Send a BG brush changed event, so you can mouse wheel the
        // transparency of the titlebar too.
        if (changed)
        {
            PropertyChanged.raise(*this, Data::PropertyChangedEventArgs{ L"BackgroundBrush" });
        }
    }

    TermControl::~TermControl()
    {
        Close();
    }

    // Method Description:
    // - Creates an automation peer for the Terminal Control, enabling accessibility on our control.
    // Arguments:
    // - None
    // Return Value:
    // - The automation peer for our control
    Windows::UI::Xaml::Automation::Peers::AutomationPeer TermControl::OnCreateAutomationPeer()
    {
        // MSFT 33353327: We're purposefully not using _initializedTerminal to ensure we're fully initialized.
        // Doing so makes us return nullptr when XAML requests an automation peer.
        // Instead, we need to give XAML an automation peer, then fix it later.
        if (!_IsClosing() && !_detached)
        {
            // It's unexpected that interactivity is null even when we're not closing or in detached state.
            THROW_HR_IF_NULL(E_UNEXPECTED, _interactivity);

            // create a custom automation peer with this code pattern:
            // (https://docs.microsoft.com/en-us/windows/uwp/design/accessibility/custom-automation-peers)
            if (const auto& interactivityAutoPeer{ _interactivity.OnCreateAutomationPeer() })
            {
                const auto margins{ SwapChainPanel().Margin() };
                const Core::Padding padding{
                    static_cast<float>(margins.Left),
                    static_cast<float>(margins.Top),
                    static_cast<float>(margins.Right),
                    static_cast<float>(margins.Bottom),
                };
                _automationPeer = winrt::make<implementation::TermControlAutomationPeer>(get_strong(), padding, interactivityAutoPeer);
                return _automationPeer;
            }
        }
        return nullptr;
    }

    // This is needed for TermControlAutomationPeer. We probably could find a
    // clever way around asking the core for this.
    winrt::Windows::Foundation::Size TermControl::GetFontSize() const
    {
        return _core.FontSize();
    }

    const Windows::UI::Xaml::Thickness TermControl::GetPadding()
    {
        return SwapChainPanel().Margin();
    }

    TerminalConnection::ConnectionState TermControl::ConnectionState() const
    {
        return _core.ConnectionState();
    }

    void TermControl::RenderEngineSwapChainChanged(IInspectable /*sender*/, IInspectable args)
    {
        // This event comes in on the UI thread
        HANDLE h = reinterpret_cast<HANDLE>(winrt::unbox_value<uint64_t>(args));
        _AttachDxgiSwapChainToXaml(h);
    }

    // Method Description:
    // - Called when the renderer triggers a warning. It might do this when it
    //   fails to find a shader file, or fails to compile a shader. We'll take
    //   that renderer warning, and display a dialog to the user with and
    //   appropriate error message. WE'll display the dialog with our
    //   RaiseNotice event.
    // Arguments:
    // - hr: an  HRESULT describing the warning
    // Return Value:
    // - <none>
    safe_void_coroutine TermControl::_RendererWarning(IInspectable /*sender*/, Control::RendererWarningArgs args)
    {
        auto weakThis{ get_weak() };
        co_await wil::resume_foreground(Dispatcher());

        const auto control = weakThis.get();
        if (!control)
        {
            co_return;
        }

        // HRESULT is a signed 32-bit integer which would result in a hex output like "-0x7766FFF4",
        // but canonically HRESULTs are displayed unsigned as "0x8899000C". See GH#11556.
        const auto hr = std::bit_cast<uint32_t>(args.Result());
        const auto parameter = args.Parameter();
        winrt::hstring message;

        switch (hr)
        {
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
        case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND):
            message = RS_fmt(L"PixelShaderNotFound", parameter);
            break;
        case D2DERR_SHADER_COMPILE_FAILED:
            message = RS_fmt(L"PixelShaderCompileFailed", parameter);
            break;
        case DWRITE_E_NOFONT:
            message = RS_fmt(L"RendererErrorFontNotFound", parameter);
            break;
        case ATLAS_ENGINE_ERROR_MAC_TYPE:
            message = RS_(L"RendererErrorMacType");
            break;
        default:
        {
            wchar_t buf[512];
            const auto len = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), &buf[0], ARRAYSIZE(buf), nullptr);
            const std::wstring_view msg{ &buf[0], len };
            //conditional message construction
            auto partialMessage = RS_fmt(L"RendererErrorOther", hr, msg);
            if (!parameter.empty())
            {
                fmt::format_to(std::back_inserter(partialMessage), LR"( "{0}")", parameter);
            }
            message = winrt::hstring{ partialMessage };
            break;
        }
        }

        auto noticeArgs = winrt::make<NoticeEventArgs>(NoticeLevel::Warning, std::move(message));
        control->RaiseNotice.raise(*control, std::move(noticeArgs));
    }

    void TermControl::_AttachDxgiSwapChainToXaml(HANDLE swapChainHandle)
    {
        auto nativePanel = SwapChainPanel().as<ISwapChainPanelNative2>();
        nativePanel->SetSwapChainHandle(swapChainHandle);
    }

    bool TermControl::_InitializeTerminal(const InitializeReason reason)
    {
        if (_initializedTerminal)
        {
            return false;
        }

        const auto panelWidth = static_cast<float>(SwapChainPanel().ActualWidth());
        const auto panelHeight = static_cast<float>(SwapChainPanel().ActualHeight());
        const auto panelScaleX = SwapChainPanel().CompositionScaleX();
        const auto panelScaleY = SwapChainPanel().CompositionScaleY();

        const auto windowWidth = panelWidth * panelScaleX;
        const auto windowHeight = panelHeight * panelScaleY;

        if (windowWidth == 0 || windowHeight == 0)
        {
            return false;
        }

        // If we're re-attaching an existing content, then we want to proceed even though the Terminal was already initialized.
        if (reason == InitializeReason::Create)
        {
            const auto coreInitialized = _core.Initialize(panelWidth,
                                                          panelHeight,
                                                          panelScaleX);
            if (!coreInitialized)
            {
                return false;
            }

            _interactivity.Initialize();

            if (!_restorePath.empty())
            {
                _restoreInBackground();
            }
            else
            {
                _core.Connection().Start();
            }
        }
        else
        {
            _core.SizeOrScaleChanged(panelWidth, panelHeight, panelScaleX);
        }

        _core.EnablePainting();

        auto bufferHeight = _core.BufferHeight();

        ScrollBar().Maximum(0);
        ScrollBar().Minimum(0);
        ScrollBar().Value(0);
        ScrollBar().ViewportSize(bufferHeight);
        ScrollBar().LargeChange(bufferHeight); // scroll one "screenful" at a time when the scroll bar is clicked

        // Now that the renderer is set up, update the appearance for initialization
        _UpdateAppearanceFromUIThread(_core.FocusedAppearance());

        _initializedTerminal = true;

        // MSFT 33353327: If the AutomationPeer was created before we were done initializing,
        // make sure it's properly set up now.
        if (_automationPeer)
        {
            _automationPeer.UpdateControlBounds();
            const auto margins{ GetPadding() };
            _automationPeer.SetControlPadding(Core::Padding{
                static_cast<float>(margins.Left),
                static_cast<float>(margins.Top),
                static_cast<float>(margins.Right),
                static_cast<float>(margins.Bottom),
            });
        }

        // Likewise, run the event handlers outside of lock (they could
        // be reentrant)
        Initialized.raise(*this, nullptr);
        return true;
    }

    safe_void_coroutine TermControl::_restoreInBackground()
    {
        const auto path = std::exchange(_restorePath, {});
        const auto weakSelf = get_weak();
        winrt::apartment_context uiThread;

        try
        {
            co_await winrt::resume_background();

            const auto self = weakSelf.get();
            if (!self)
            {
                co_return;
            }

            winrt::get_self<ControlCore>(_core)->RestoreFromPath(path.c_str());
        }
        CATCH_LOG();

        try
        {
            co_await uiThread;

            const auto self = weakSelf.get();
            if (!self)
            {
                co_return;
            }

            if (const auto connection = _core.Connection())
            {
                connection.Start();
            }
        }
        CATCH_LOG();
    }

    void TermControl::_CharacterHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                        const Input::CharacterReceivedRoutedEventArgs& e)
    {
        if (_IsClosing())
        {
            return;
        }

        const auto ch = e.Character();
        const auto keyStatus = e.KeyStatus();
        const auto scanCode = gsl::narrow_cast<WORD>(keyStatus.ScanCode);
        auto modifiers = _GetPressedModifierKeys();

        if (keyStatus.IsExtendedKey)
        {
            modifiers |= ControlKeyStates::EnhancedKey;
        }

        // Broadcast the character to all listeners
        // only broadcast if there's an actual listener. Saves the overhead of some object creation.
        if (CharSent)
        {
            auto charSentArgs = winrt::make<CharSentEventArgs>(ch, scanCode, modifiers);
            CharSent.raise(*this, charSentArgs);
        }

        const auto handled = RawWriteChar(ch, scanCode, modifiers);

        e.Handled(handled);
    }

    bool TermControl::RawWriteChar(const wchar_t character,
                                   const WORD scanCode,
                                   const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers)
    {
        return _core.SendCharEvent(character, scanCode, modifiers);
    }

    void TermControl::RawWriteString(const winrt::hstring& text)
    {
        _core.SendInput(text);
    }

    // Method Description:
    // - Manually handles key events for certain keys that can't be passed to us
    //   normally. Namely, the keys we're concerned with are F7 down and Alt up.
    // Return value:
    // - Whether the key was handled.
    bool TermControl::OnDirectKeyEvent(const uint32_t vkey, const uint8_t scanCode, const bool down)
    {
        const auto modifiers{ _GetPressedModifierKeys() };
        return _KeyHandler(gsl::narrow_cast<WORD>(vkey), gsl::narrow_cast<WORD>(scanCode), modifiers, down);
    }

    void TermControl::_KeyDownHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                      const Input::KeyRoutedEventArgs& e)
    {
        _KeyHandler(e, true);
    }

    void TermControl::_KeyUpHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                    const Input::KeyRoutedEventArgs& e)
    {
        _KeyHandler(e, false);
    }

    void TermControl::_KeyHandler(const Input::KeyRoutedEventArgs& e, const bool keyDown)
    {
        const auto keyStatus = e.KeyStatus();
        const auto vkey = gsl::narrow_cast<WORD>(e.OriginalKey());
        const auto scanCode = gsl::narrow_cast<WORD>(keyStatus.ScanCode);
        auto modifiers = _GetPressedModifierKeys();

        if (keyStatus.IsExtendedKey)
        {
            modifiers |= ControlKeyStates::EnhancedKey;
        }

        e.Handled(_KeyHandler(vkey, scanCode, modifiers, keyDown));
    }

    bool TermControl::_KeyHandler(WORD vkey, WORD scanCode, ControlKeyStates modifiers, bool keyDown)
    {
        // If the current focused element is a child element of searchbox,
        // we do not send this event up to terminal
        if (_searchBox && _searchBox->ContainsFocus())
        {
            return false;
        }

        // GH#11076:
        // For some weird reason we sometimes receive a WM_KEYDOWN
        // message without vkey or scanCode if a user drags a tab.
        // The KeyChord constructor has a debug assertion ensuring that all KeyChord
        // either have a valid vkey/scanCode. This is important, because this prevents
        // accidental insertion of invalid KeyChords into classes like ActionMap.
        if (!vkey && !scanCode)
        {
            return true;
        }

        // Mark the event as handled and do nothing if we're closing, or the key
        // was the Windows key.
        //
        // NOTE: for key combos like CTRL + C, two events are fired (one for
        // CTRL, one for 'C'). Since it's possible the terminal is in
        // win32-input-mode, then we'll send all these keystrokes to the
        // terminal - it's smart enough to ignore the keys it doesn't care
        // about.
        if (_IsClosing() || vkey == VK_LWIN || vkey == VK_RWIN)
        {
            return true;
        }

        // Short-circuit isReadOnly check to avoid warning dialog
        if (_core.IsInReadOnlyMode())
        {
            return !keyDown || _TryHandleKeyBinding(vkey, scanCode, modifiers);
        }

        // Our custom TSF input control doesn't receive Alt+Numpad inputs,
        // and we don't receive any via WM_CHAR as a xaml island app either.
        // So, we simply implement our own Alt-Numpad handling here.
        //
        // This handles the case where the Alt key is released.
        // We'll flush any ongoing composition in that case.
        if (vkey == VK_MENU && !keyDown && _altNumpadState.active)
        {
            auto& s = _altNumpadState;
            auto encoding = s.encoding;
            wchar_t buf[4]{};
            size_t buf_len = 0;
            bool handled = true;

            if (encoding == AltNumpadEncoding::Unicode)
            {
                // UTF-32 -> UTF-16
                if (s.accumulator == 0)
                {
                    // If the user pressed Alt + VK_ADD, then released Alt, they probably didn't intend to insert a numpad character at all.
                    // Send any accumulated key events instead.
                    for (auto&& e : _altNumpadState.cachedKeyEvents)
                    {
                        handled = handled && _TrySendKeyEvent(e.vkey, e.scanCode, e.modifiers, e.keyDown);
                    }
                    // Send the alt keyup we are currently processing
                    handled = handled && _TrySendKeyEvent(vkey, scanCode, modifiers, keyDown);
                    // do not accumulate into the buffer
                }
                else if (s.accumulator <= 0xffff)
                {
                    buf[buf_len++] = static_cast<uint16_t>(s.accumulator);
                }
                else
                {
                    buf[buf_len++] = static_cast<uint16_t>((s.accumulator >> 10) + 0xd7c0);
                    buf[buf_len++] = static_cast<uint16_t>((s.accumulator & 0x3ff) | 0xdc00);
                }
            }
            else
            {
                const auto ansi = encoding == AltNumpadEncoding::ANSI;
                const auto acp = GetACP();
                auto codepage = ansi ? acp : CP_OEMCP;

                // Alt+Numpad inputs are always a single codepoint, be it UTF-32 or ANSI.
                // Since DBCS code pages by definition are >1 codepoint, we can't encode those.
                // Traditionally, the OS uses the Latin1 or IBM code page instead.
                if (acp == CP_JAPANESE ||
                    acp == CP_CHINESE_SIMPLIFIED ||
                    acp == CP_KOREAN ||
                    acp == CP_CHINESE_TRADITIONAL ||
                    acp == CP_UTF8)
                {
                    codepage = ansi ? 1252 : 437;
                }

                // The OS code seemed to also simply cut off the last byte in the accumulator.
                const auto ch = gsl::narrow_cast<char>(s.accumulator & 0xff);
                const auto len = MultiByteToWideChar(codepage, 0, &ch, 1, &buf[0], 2);
                buf_len = gsl::narrow_cast<size_t>(std::max(0, len));
            }

            if (buf_len != 0)
            {
                // WinRT always needs null-terminated strings, because HSTRING is dumb.
                // If it encounters a string that isn't, cppwinrt will abort().
                // It should already be null-terminated, but let's make sure to not crash.
                buf[buf_len] = L'\0';
                _core.SendInput(std::wstring_view{ &buf[0], buf_len });
            }

            s = {};
            return handled;
        }
        // As a continuation of the above, this handles the key-down case.
        if (modifiers.IsAltPressed())
        {
            // The OS code seems to reset the composition if shift is pressed, but I couldn't
            // figure out how exactly it worked. We'll simply ignore any such inputs.
            static constexpr DWORD permittedModifiers =
                RIGHT_ALT_PRESSED |
                LEFT_ALT_PRESSED |
                NUMLOCK_ON |
                SCROLLLOCK_ON |
                CAPSLOCK_ON;

            if ((modifiers.Value() & ~permittedModifiers) == 0)
            {
                auto& s = _altNumpadState;

                if (keyDown)
                {
                    if (vkey == VK_ADD)
                    {
                        static const auto enabled = []() {
                            wchar_t buffer[4]{};
                            DWORD size = sizeof(buffer);
                            RegGetValueW(
                                HKEY_CURRENT_USER,
                                L"Control Panel\\Input Method",
                                L"EnableHexNumpad",
                                RRF_RT_REG_SZ,
                                nullptr,
                                &buffer[0],
                                &size);
                            return size == 4 && memcmp(&buffer[0], L"1", 4) == 0;
                        }();

                        if (enabled)
                        {
                            // Alt '+' <number> is used to input Unicode code points.
                            // Every time you press + it resets the entire state
                            // in the original OS implementation as well.
                            s.encoding = AltNumpadEncoding::Unicode;
                            s.accumulator = 0;
                            s.active = true;
                        }
                    }
                    else if (vkey == VK_NUMPAD0 && s.encoding == AltNumpadEncoding::OEM && s.accumulator == 0)
                    {
                        // Alt '0' <number> is used to input ANSI code points.
                        // Otherwise, they're OEM codepoints.
                        s.encoding = AltNumpadEncoding::ANSI;
                        s.active = true;
                    }
                    else
                    {
                        // Otherwise, append the pressed key to the accumulator.
                        const uint32_t base = s.encoding == AltNumpadEncoding::Unicode ? 16 : 10;
                        uint32_t add = 0xffffff;

                        if (vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9)
                        {
                            add = vkey - VK_NUMPAD0;
                        }
                        else if (vkey >= 'A' && vkey <= 'F')
                        {
                            add = vkey - 'A' + 10;
                        }

                        // Pressing Alt + <not a number> should not activate the Alt+Numpad input, however.
                        if (add < base)
                        {
                            s.accumulator = std::min(s.accumulator * base + add, 0x10FFFFu);
                            s.active = true;
                        }
                    }
                }

                // If someone pressed Alt + <not a number>, we'll skip the early
                // return and send the Alt key combination as per usual.
                if (s.active)
                {
                    // Cache it in case we have to emit it after alt is released
                    _altNumpadState.cachedKeyEvents.emplace_back(vkey, scanCode, modifiers, keyDown);
                    return true;
                }

                // Unless I didn't code the above correctly, active == false should imply
                // that _altNumpadState is in the (default constructed) base state.
                assert(s.encoding == AltNumpadEncoding::OEM);
                assert(s.accumulator == 0);
            }
        }
        else if (_altNumpadState.active)
        {
            // If the user Alt+Tabbed in the middle of an Alt+Numpad sequence, we'll not receive a key-up event for
            // the Alt key. There are several ways to detect this. Here, we simply check if the user typed another
            // character, it's not an alt-up event, and we still have an ongoing composition.
            _altNumpadState = {};
        }

        // If a composition just ended, its text is still queued up inside TSF. Hand it to
        // the connection now, so that a key the IME passed on to us can't reach the
        // connection before the text it finalized - neither by being forwarded to the PTY,
        // nor by triggering an action bound to it. GH#20244
        GetTSFHandle().FlushPendingComposition();

        // GH#2235: Terminal::Settings hasn't been modified to differentiate
        // between AltGr and Ctrl+Alt yet.
        // -> Don't check for key bindings if this is an AltGr key combination.
        //
        // GH#4999: Only process keybindings on the keydown. If we don't check
        // this at all, we'll process the keybinding twice. If we only process
        // keybindings on the keyUp, then we'll still send the keydown to the
        // connected terminal application, and something like ctrl+shift+T will
        // emit a ^T to the pipe.
        if (!modifiers.IsAltGrPressed() &&
            keyDown &&
            _TryHandleKeyBinding(vkey, scanCode, modifiers))
        {
            return true;
        }

        // While TSF has an active composition, key events must not be forwarded
        // to the PTY directly. The IME re-injects navigation/confirmation keys
        // after composition ends, at which point HasActiveComposition() == false
        // and they are forwarded normally. This mirrors conhost v1 behavior in
        // src/interactivity/win32/windowio.cpp.
        if (GetTSFHandle().HasActiveComposition())
        {
            return true;
        }

        if (_TrySendKeyEvent(vkey, scanCode, modifiers, keyDown))
        {
            return true;
        }

        // Manually prevent keyboard navigation with tab. We want to send tab to
        // the terminal, and we don't want to be able to escape focus of the
        // control with tab.
        return vkey == VK_TAB;
    }

    // Method Description:
    // - Attempt to handle this key combination as a key binding
    // Arguments:
    // - vkey: The vkey of the key pressed.
    // - scanCode: The scan code of the key pressed.
    // - modifiers: The ControlKeyStates representing the modifier key states.
    bool TermControl::_TryHandleKeyBinding(const WORD vkey, const WORD scanCode, ::Microsoft::Terminal::Core::ControlKeyStates modifiers) const
    {
        // Mark mode has a specific set of pre-defined key bindings.
        // If we're in mark mode, we should be prioritizing those over
        // the custom defined key bindings.
        if (_core.TryMarkModeKeybinding(vkey, modifiers))
        {
            return true;
        }

        if (!_keyBindings)
        {
            return false;
        }

        auto success = _keyBindings.TryKeyChord({
            modifiers.IsCtrlPressed(),
            modifiers.IsAltPressed(),
            modifiers.IsShiftPressed(),
            modifiers.IsWinPressed(),
            vkey,
            scanCode,
        });
        if (!success)
        {
            return false;
        }

        // Let's assume the user has bound the dead key "^" to a sendInput command that sends "b".
        // If the user presses the two keys "^a" it'll produce "bâ", despite us marking the key event as handled.
        // The following is used to manually "consume" such dead keys and clear them from the keyboard state.
        _ClearKeyboardState(vkey, scanCode);
        return true;
    }

    // Method Description:
    // - Discards currently pressed dead keys.
    // Arguments:
    // - vkey: The vkey of the key pressed.
    // - scanCode: The scan code of the key pressed.
    void TermControl::_ClearKeyboardState(const WORD vkey, const WORD scanCode) noexcept
    {
        std::array<BYTE, 256> keyState;
        if (!GetKeyboardState(keyState.data()))
        {
            return;
        }

        // As described in "Sometimes you *want* to interfere with the keyboard's state buffer":
        //   http://archives.miloush.net/michkap/archive/2006/09/10/748775.html
        // > "The key here is to keep trying to pass stuff to ToUnicode until -1 is not returned."
        std::array<wchar_t, 16> buffer;
        while (ToUnicodeEx(vkey, scanCode, keyState.data(), buffer.data(), gsl::narrow_cast<int>(buffer.size()), 0b1, nullptr) < 0)
        {
        }
    }

    // Method Description:
    // - Send this particular key event to the terminal.
    //   See Terminal::SendKeyEvent for more information.
    // - Clears the current selection.
    // - Makes the cursor briefly visible during typing.
    // Arguments:
    // - vkey: The vkey of the key pressed.
    // - scanCode: The scan code of the key pressed.
    // - states: The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
    // - keyDown: If true, the key was pressed; otherwise, the key was released.
    bool TermControl::_TrySendKeyEvent(const WORD vkey,
                                       const WORD scanCode,
                                       const ControlKeyStates modifiers,
                                       const bool keyDown)
    {
        // Broadcast the key  to all listeners
        // only broadcast if there's an actual listener. Saves the overhead of some object creation.
        if (KeySent)
        {
            auto keySentArgs = winrt::make<KeySentEventArgs>(vkey, scanCode, modifiers, keyDown);
            KeySent.raise(*this, keySentArgs);
        }

        return RawWriteKeyEvent(vkey, scanCode, modifiers, keyDown);
    }

    bool TermControl::RawWriteKeyEvent(const WORD vkey,
                                       const WORD scanCode,
                                       const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers,
                                       const bool keyDown)
    {
        // If the terminal translated the key, mark the event as handled.
        // This will prevent the system from trying to get the character out
        // of it and sending us a CharacterReceived event.
        const auto handled = vkey ?
                                 _core.TrySendKeyEvent(vkey,
                                                       scanCode,
                                                       modifiers,
                                                       keyDown) :
                                 true;

        if (vkey && keyDown && _automationPeer)
        {
            get_self<TermControlAutomationPeer>(_automationPeer)->RecordKeyEvent(vkey);
        }

        return handled;
    }

    // Method Description:
    // - handle a tap event by taking focus
    // Arguments:
    // - sender: the XAML element responding to the tap event
    // - args: event data
    void TermControl::_TappedHandler(const IInspectable& /*sender*/, const TappedRoutedEventArgs& e)
    {
        if (e.PointerDeviceType() == Windows::Devices::Input::PointerDeviceType::Touch)
        {
            // Normally TSF would be responsible for showing the touch keyboard, but it's buggy for us:
            // If you have focus on a TermControl and type on your physical keyboard then touching
            // the TermControl will not show the touch keyboard ever again unless you focus another app.
            // Why that happens is unclear, but it can be fixed by us showing it manually.
            setInputPaneVisibility(reinterpret_cast<HWND>(OwningHwnd()), true);
        }

        e.Handled(true);
    }

    // Method Description:
    // - handle a mouse click event. Begin selection process.
    // Arguments:
    // - sender: the XAML element responding to the pointer input
    // - args: event data
    void TermControl::_PointerPressedHandler(const Windows::Foundation::IInspectable& sender,
                                             const Input::PointerRoutedEventArgs& args)
    {
        if (_IsClosing())
        {
            return;
        }

        RestorePointerCursor.raise(*this, nullptr);

        _CapturePointer(sender, args);

        const auto ptr = args.Pointer();
        const auto point = args.GetCurrentPoint(*this);
        const auto type = ptr.PointerDeviceType();

        // GH#19908: _focused can be true even when the search box has
        // keyboard focus, because GotFocus bubbles from the search box
        // child and _GotFocusHandler sets _focused=true. If the user
        // click-drags in the terminal while the search box is focused,
        // we need to explicitly call Focus() to move keyboard focus back
        // to the terminal so that Ctrl+C copies the selection.
        if (!_focused || (_searchBox && _searchBox->ContainsFocus()))
        {
            Focus(FocusState::Pointer);
        }

        if (type == Windows::Devices::Input::PointerDeviceType::Touch)
        {
            // NB: I don't think this is correct because the touch should be in the center of the rect.
            //     I suspect the point.Position() would be correct.
            const auto contactRect = point.Properties().ContactRect();
            til::point newTouchPoint{ til::math::rounding, contactRect.X, contactRect.Y };
            _interactivity.TouchPressed(newTouchPoint.to_core_point());
        }
        else
        {
            const auto cursorPosition = point.Position();
            _interactivity.PointerPressed(point.PointerId(),
                                          TermControl::GetPressedMouseButtons(point),
                                          TermControl::GetPointerUpdateKind(point),
                                          point.Timestamp(),
                                          ControlKeyStates{ args.KeyModifiers() },
                                          _toTerminalOrigin(cursorPosition));
        }

        args.Handled(true);
    }

    // Method Description:
    // - handle a mouse moved event. Specifically handling mouse drag to update selection process.
    // Arguments:
    // - sender: not used
    // - args: event data
    void TermControl::_PointerMovedHandler(const Windows::Foundation::IInspectable& /*sender*/,
                                           const Input::PointerRoutedEventArgs& args)
    {
        if (_IsClosing())
        {
            return;
        }

        RestorePointerCursor.raise(*this, nullptr);

        const auto ptr = args.Pointer();
        const auto point = args.GetCurrentPoint(*this);
        const auto cursorPosition = point.Position();
        const auto pixelPosition = _toTerminalOrigin(cursorPosition);
        const auto type = ptr.PointerDeviceType();

        if (!_focused && _core.Settings().FocusFollowMouse())
        {
            FocusFollowMouseRequested.raise(*this, nullptr);
        }

        if (type == Windows::Devices::Input::PointerDeviceType::Mouse ||
            type == Windows::Devices::Input::PointerDeviceType::Pen)
        {
            _interactivity.PointerMoved(point.PointerId(),
                                        TermControl::GetPressedMouseButtons(point),
                                        TermControl::GetPointerUpdateKind(point),
                                        ControlKeyStates(args.KeyModifiers()),
                                        pixelPosition);
        }
        else if (type == Windows::Devices::Input::PointerDeviceType::Touch)
        {
            const auto contactRect = point.Properties().ContactRect();
            til::point newTouchPoint{ til::math::rounding, contactRect.X, contactRect.Y };

            _interactivity.TouchMoved(newTouchPoint.to_core_point());
        }

        args.Handled(true);
    }

    // Method Description:
    // - Event handler for the PointerReleased event. We use this to de-anchor
    //   touch events, to stop scrolling via touch.
    // Arguments:
    // - sender: the XAML element responding to the pointer input
    // - args: event data
    void TermControl::_PointerReleasedHandler(const Windows::Foundation::IInspectable& sender,
                                              const Input::PointerRoutedEventArgs& args)
    {
        if (_IsClosing())
        {
            return;
        }

        const auto ptr = args.Pointer();
        const auto point = args.GetCurrentPoint(*this);
        const auto cursorPosition = point.Position();
        const auto pixelPosition = _toTerminalOrigin(cursorPosition);
        const auto type = ptr.PointerDeviceType();

        _ReleasePointerCapture(sender, args);

        if (type == Windows::Devices::Input::PointerDeviceType::Mouse ||
            type == Windows::Devices::Input::PointerDeviceType::Pen)
        {
            _interactivity.PointerReleased(point.PointerId(),
                                           TermControl::GetPressedMouseButtons(point),
                                           TermControl::GetPointerUpdateKind(point),
                                           ControlKeyStates(args.KeyModifiers()),
                                           pixelPosition);
        }
        else if (type == Windows::Devices::Input::PointerDeviceType::Touch)
        {
            _interactivity.TouchReleased();
        }

        args.Handled(true);
    }

    // Method Description:
    // - Event handler for the PointerWheelChanged event. This is raised in
    //   response to mouse wheel changes. Depending upon what modifier keys are
    //   pressed, different actions will take place.
    // - Primarily just takes the data from the PointerRoutedEventArgs and uses
    //   it to call _DoMouseWheel, see _DoMouseWheel for more details.
    // Arguments:
    // - args: the event args containing information about t`he mouse wheel event.
    void TermControl::_MouseWheelHandler(const Windows::Foundation::IInspectable& /*sender*/,
                                         const Input::PointerRoutedEventArgs& args)
    {
        if (_IsClosing())
        {
            return;
        }

        RestorePointerCursor.raise(*this, nullptr);

        const auto point = args.GetCurrentPoint(*this);
        auto delta = point.Properties().MouseWheelDelta();
        auto result = _interactivity.MouseWheel(ControlKeyStates{ args.KeyModifiers() },
                                                point.Properties().IsHorizontalMouseWheel() ?
                                                    Core::Point{ delta, 0 } :
                                                    Core::Point{ 0, delta },
                                                _toTerminalOrigin(point.Position()),
                                                TermControl::GetPressedMouseButtons(point));
        if (result)
        {
            args.Handled(true);
        }
    }

    // Method Description:
    // - This is part of the solution to GH#979
    // - Manually handle a scrolling event. This is used to help support
    //   scrolling on devices where the touchpad doesn't correctly handle
    //   scrolling inactive windows.
    // Arguments:
    // - location: the location of the mouse during this event. This location is
    //   relative to the origin of the control
    // - delta: the mouse wheel delta that triggered this event.
    // - state: the state for each of the mouse buttons individually (pressed/unpressed)
    bool TermControl::OnMouseWheel(const Windows::Foundation::Point location,
                                   const Core::Point delta,
                                   const bool leftButtonDown,
                                   const bool midButtonDown,
                                   const bool rightButtonDown)
    {
        const auto modifiers = _GetPressedModifierKeys();

        Control::MouseButtonState state{};
        WI_SetFlagIf(state, Control::MouseButtonState::IsLeftButtonDown, leftButtonDown);
        WI_SetFlagIf(state, Control::MouseButtonState::IsMiddleButtonDown, midButtonDown);
        WI_SetFlagIf(state, Control::MouseButtonState::IsRightButtonDown, rightButtonDown);

        return _interactivity.MouseWheel(modifiers, delta, _toTerminalOrigin(location), state);
    }

    // Method Description:
    // - Called in response to the core's TransparencyChanged event. We'll use
    //   this to update our background brush.
    // - The Core should have already updated the TintOpacity and UseAcrylic
    //   properties in the _settings->
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void TermControl::_coreTransparencyChanged(IInspectable /*sender*/, Control::TransparencyChangedEventArgs /*args*/)
    {
        try
        {
            _changeBackgroundOpacity();
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Reset the font size of the terminal to its default size.
    // Arguments:
    // - none
    void TermControl::ResetFontSize()
    {
        _core.ResetFontSize();
    }

    // Method Description:
    // - Adjust the font size of the terminal control.
    // Arguments:
    // - fontSizeDelta: The amount to increase or decrease the font size by.
    void TermControl::AdjustFontSize(float fontSizeDelta)
    {
        _core.AdjustFontSize(fontSizeDelta);
    }

    void TermControl::_ScrollbarChangeHandler(const Windows::Foundation::IInspectable& /*sender*/,
                                              const Controls::Primitives::RangeBaseValueChangedEventArgs& args)
    {
        if (_isInternalScrollBarUpdate || _IsClosing())
        {
            // The update comes from ourselves, more specifically from the
            // terminal. So we don't have to update the terminal because it
            // already knows.
            return;
        }

        const auto newValue = args.NewValue();
        _interactivity.UpdateScrollbar(static_cast<float>(newValue));

        // User input takes priority over terminal events so cancel
        // any pending scroll bar update if the user scrolls.
        _updateScrollBar->ModifyPending([](auto& update) {
            update.newValue.reset();
        });
    }

    // Method Description:
    // - captures the pointer so that none of the other XAML elements respond to pointer events
    // Arguments:
    // - sender: XAML element that is interacting with pointer
    // - args: pointer data (i.e.: mouse, touch)
    // Return Value:
    // - true if we successfully capture the pointer, false otherwise.
    bool TermControl::_CapturePointer(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args)
    {
        IUIElement uielem;
        if (sender.try_as(uielem))
        {
            uielem.CapturePointer(args.Pointer());
            return true;
        }
        return false;
    }

    // Method Description:
    // - releases the captured pointer because we're done responding to XAML pointer events
    // Arguments:
    // - sender: XAML element that is interacting with pointer
    // - args: pointer data (i.e.: mouse, touch)
    // Return Value:
    // - true if we release capture of the pointer, false otherwise.
    bool TermControl::_ReleasePointerCapture(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Input::PointerRoutedEventArgs& args)
    {
        IUIElement uielem;
        if (sender.try_as(uielem))
        {
            uielem.ReleasePointerCapture(args.Pointer());
            return true;
        }
        return false;
    }

    // Method Description:
    // - Event handler for the GotFocus event. This is used to...
    //   - enable accessibility notifications for this TermControl
    //   - start blinking the cursor when the window is focused
    //   - update the number of lines to scroll to the value set in the system
    void TermControl::_GotFocusHandler(const Windows::Foundation::IInspectable& /* sender */,
                                       const RoutedEventArgs& /* args */)
    {
        if (_IsClosing())
        {
            return;
        }

        _focused = true;

        // GH#5421: Enable the UiaEngine before checking for the SearchBox
        // That way, new selections are notified to automation clients.
        // The _uiaEngine lives in _interactivity, so call into there to enable it.

        if (_interactivity)
        {
            _interactivity.GotFocus();
        }

        // If the searchbox is focused, we don't want TSFInputControl to think
        // it has focus so it doesn't intercept IME input. We also don't want the
        // terminal's cursor to start blinking. So, we'll just return quickly here.
        if (_searchBox && _searchBox->ContainsFocus())
        {
            return;
        }

        // Only update the appearance here if an unfocused config exists - if an
        // unfocused config does not exist then we never would have switched
        // appearances anyway so there's no need to switch back upon gaining
        // focus
        if (_core.HasUnfocusedAppearance())
        {
            UpdateAppearance(_core.FocusedAppearance());
        }

        GetTSFHandle().Focus(&_tsfDataProvider);
    }

    // Method Description:
    // - Event handler for the LostFocus event. This is used to...
    //   - disable accessibility notifications for this TermControl
    //   - hide and stop blinking the cursor when the window loses focus.
    void TermControl::_LostFocusHandler(const Windows::Foundation::IInspectable& /* sender */,
                                        const RoutedEventArgs& /* args */)
    {
        if (_IsClosing())
        {
            return;
        }

        RestorePointerCursor.raise(*this, nullptr);

        _focused = false;

        // This will disable the accessibility notifications, because the
        // UiaEngine lives in ControlInteractivity
        if (_interactivity)
        {
            _interactivity.LostFocus();
        }

        // Check if there is an unfocused config we should set the appearance to
        // upon losing focus
        if (_core.HasUnfocusedAppearance())
        {
            UpdateAppearance(_core.UnfocusedAppearance());
        }

        GetTSFHandle().Unfocus(&_tsfDataProvider);
    }

    // Method Description:
    // - Triggered when the swapchain changes size. We use this to resize the
    //      terminal buffers to match the new visible size.
    // Arguments:
    // - e: a SizeChangedEventArgs with the new dimensions of the SwapChainPanel
    void TermControl::_SwapChainSizeChanged(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                            const SizeChangedEventArgs& e)
    {
        if (!_initializedTerminal || _IsClosing())
        {
            return;
        }

        const auto newSize = e.NewSize();
        if (newSize.Width <= 0 || newSize.Height <= 0)
        {
            return;
        }

        _core.SizeChanged(newSize.Width, newSize.Height);
        _ShowResizeOverlay();

        if (_automationPeer)
        {
            _automationPeer.UpdateControlBounds();
        }
    }

    // Shows an overlay with the current terminal dimensions (columns x rows).
    void TermControl::_ShowResizeOverlay()
    {
        // Don't show the overlay in the Settings preview control.
        if (!IsEnabled())
        {
            return;
        }

        const auto coreImpl = winrt::get_self<ControlCore>(_core);
        const auto size = coreImpl->ViewportSize();

        // Sometimes _SwapChainSizeChanged is called despite no actual size change.
        // This happens, e.g., when switching tabs. Ignore such "updates".
        if (size == _lastResizeOverlaySize)
        {
            return;
        }

        const auto isInitialSize = _lastResizeOverlaySize == Core::Size{};
        _lastResizeOverlaySize = size;
        if (isInitialSize)
        {
            return;
        }

        ResizeOverlayText().Text(fmt::format(FMT_COMPILE(L"{} \u00D7 {}"), size.Width, size.Height));
        ResizeOverlay().Visibility(Visibility::Visible);

        if (!_resizeOverlayTimer)
        {
            _resizeOverlayTimer.emplace();
            _resizeOverlayTimer->Interval(std::chrono::milliseconds(750));
            _resizeOverlayTimer->Tick([weakThis = get_weak()](auto&&, auto&&) {
                if (auto self = weakThis.get())
                {
                    self->ResizeOverlay().Visibility(Visibility::Collapsed);
                    self->_resizeOverlayTimer->Stop();
                }
            });
        }

        _resizeOverlayTimer->Start();
    }

    // Method Description:
    // - Triggered when the swapchain changes DPI. When this happens, we're
    //   going to receive 3 events:
    //   - 1. First, a CompositionScaleChanged _for the original scale_. I don't
    //     know why this event happens first. **It also doesn't always happen.**
    //     However, when it does happen, it doesn't give us any useful
    //     information.
    //   - 2. Then, a SizeChanged. During that SizeChanged, either:
    //      - the CompositionScale will still be the original DPI. This happens
    //        when the control is visible as the DPI changes.
    //      - The CompositionScale will be the new DPI. This happens when the
    //        control wasn't focused as the window's DPI changed, so it only got
    //        these messages after XAML updated its scaling.
    //   - 3. Finally, a CompositionScaleChanged with the _new_ DPI.
    //   - 4. We'll usually get another SizeChanged some time after this last
    //     ScaleChanged. This usually seems to happen after something triggers
    //     the UI to re-layout, like hovering over the scrollbar. This event
    //     doesn't reliably happen immediately after a scale change, so we can't
    //     depend on it (despite the fact that both the scale and size state is
    //     definitely correct in it)
    // - In the 3rd event, we're going to update our font size for the new DPI.
    //   At that point, we know how big the font should be for the new DPI, and
    //   how big the SwapChainPanel will be. If these sizes are different, we'll
    //   need to resize the buffer to fit in the new window.
    // Arguments:
    // - sender: The SwapChainPanel who's DPI changed. This is our _swapchainPanel.
    // - args: This param is unused in the CompositionScaleChanged event.
    void TermControl::_SwapChainScaleChanged(const Windows::UI::Xaml::Controls::SwapChainPanel& sender,
                                             const Windows::Foundation::IInspectable& /*args*/)
    {
        const auto scaleX = sender.CompositionScaleX();

        _core.ScaleChanged(scaleX);
    }

    // Method Description:
    // - Update the position and size of the scrollbar to match the given
    //      viewport top, viewport height, and buffer size.
    //   Additionally fires a ScrollPositionChanged event for anyone who's
    //      registered an event handler for us.
    // Arguments:
    // - viewTop: the top of the visible viewport, in rows. 0 indicates the top
    //      of the buffer.
    // - viewHeight: the height of the viewport in rows.
    // - bufferSize: the length of the buffer, in rows
    void TermControl::_ScrollPositionChanged(const IInspectable& /*sender*/,
                                             const Control::ScrollPositionChangedArgs& args)
    {
        ScrollBarUpdate update;
        const auto hiddenContent = args.BufferSize() - args.ViewHeight();
        update.newMaximum = hiddenContent;
        update.newMinimum = 0;
        update.newViewportSize = args.ViewHeight();
        update.newValue = args.ViewTop();

        _updateScrollBar->Run(update);

        // If we have a selection with markers (exposed via selection mode),
        //   update the position of the markers
        if (_core.HasSelection() && _core.SelectionMode() >= SelectionInteractionMode::Keyboard)
        {
            _updateSelectionMarkers(nullptr, winrt::make<UpdateSelectionMarkersEventArgs>(false));
        }

        RefreshQuickFixMenu();
    }

    hstring TermControl::Title()
    {
        return _core.Title();
    }

    hstring TermControl::GetStartingTitle() const
    {
        return _core.Settings().StartingTitle();
    }

    hstring TermControl::WorkingDirectory() const
    {
        return _core.WorkingDirectory();
    }

    bool TermControl::BracketedPasteEnabled() const noexcept
    {
        return _core.BracketedPasteEnabled();
    }

    // Method Description:
    // - Given a copy-able selection, get the selected text from the buffer and send it to the
    //     Windows Clipboard (CascadiaWin32:main.cpp).
    // - CopyOnSelect does NOT clear the selection
    // Arguments:
    // - dismissSelection: dismiss the text selection after copy
    // - singleLine: collapse all of the text to one line
    // - withControlSequences: if enabled, the copied plain text contains color/style ANSI escape codes from the selection
    // - formats: which formats to copy (defined by action's CopyFormatting arg). nullptr
    //             if we should defer which formats are copied to the global setting
    bool TermControl::CopySelectionToClipboard(bool dismissSelection, bool singleLine, bool withControlSequences, const CopyFormat formats)
    {
        if (_IsClosing())
        {
            return false;
        }

        const auto successfulCopy = _interactivity.CopySelectionToClipboard(singleLine, withControlSequences, formats);

        if (dismissSelection)
        {
            _core.ClearSelection();
        }

        return successfulCopy;
    }

    // Method Description:
    // - Initiate a paste operation.
    void TermControl::PasteTextFromClipboard()
    {
        _interactivity.RequestPasteTextFromClipboard();
    }

    void TermControl::SelectAll()
    {
        _core.SelectAll();
    }

    bool TermControl::ToggleBlockSelection()
    {
        return _core.ToggleBlockSelection();
    }

    void TermControl::ToggleMarkMode()
    {
        _core.ToggleMarkMode();
    }

    bool TermControl::SwitchSelectionEndpoint()
    {
        return _core.SwitchSelectionEndpoint();
    }

    bool TermControl::ExpandSelectionToWord()
    {
        return _core.ExpandSelectionToWord();
    }

    void TermControl::RestoreFromPath(winrt::hstring path)
    {
        _restorePath = std::move(path);
    }

    void TermControl::PersistTo(int64_t handle) const
    {
        // Don't persist us if we weren't ever initialized. In that case, we
        // never got an initial size, never instantiated a buffer, and didn't
        // start the connection yet, so there's nothing for us to add here.
        //
        // If we were supposed to be restored from a path, then we don't need to
        // do anything special here. We'll leave the original file untouched,
        // and the next time we actually are initialized, we'll just use that
        // file then.
        if (_initializedTerminal)
        {
            winrt::get_self<ControlCore>(_core)->PersistTo(reinterpret_cast<HANDLE>(handle));
        }
    }

    // The periodic counterpart to PersistTo. Serializing a pane writes its
    // WHOLE scrollback -- uncapped, and megabytes for a busy pane -- so doing
    // it on a timer the way the exit path does it would be a visible hitch
    // every few minutes. This mirrors _restoreInBackground exactly: hop to a
    // background thread and talk to ControlCore directly, which takes the
    // terminal's read lock and is safe to call from off the UI thread.
    //
    // Fire and forget, so it is only ever used for the periodic save. The
    // exit path stays synchronous -- there, the process is about to end and
    // work that hasn't finished is work that never happens.
    safe_void_coroutine TermControl::PersistToPathInBackground(winrt::hstring path, bool elevated)
    {
        // See PersistTo: a control that never got a size has no buffer worth
        // writing, and one waiting to be restored still owns its old file.
        if (!_initializedTerminal || !_restorePath.empty())
        {
            co_return;
        }
        if (_persistInFlight.exchange(true))
        {
            co_return;
        }

        // Strong, not weak: the scope_exit below has to clear the flag, and a
        // control that went away mid-write would take the flag's storage with
        // it. _restoreInBackground can afford a weak ref because it only ever
        // reads through it; this one has cleanup to do.
        const auto strongSelf = get_strong();
        const auto done = wil::scope_exit([strongSelf]() { strongSelf->_persistInFlight.store(false); });

        try
        {
            co_await winrt::resume_background();

            wil::unique_hlocal_security_descriptor sd;
            SECURITY_ATTRIBUTES sa{};
            if (elevated)
            {
                unsigned long cb;
                THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NRNW;;;HI)", SDDL_REVISION_1, wil::out_param_ptr<PSECURITY_DESCRIPTOR*>(sd), &cb));
                sa.nLength = sizeof(SECURITY_ATTRIBUTES);
                sa.lpSecurityDescriptor = sd.get();
            }

            if (wil::unique_hfile file{ CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) })
            {
                const auto started = std::chrono::steady_clock::now();
                winrt::get_self<ControlCore>(_core)->PersistTo(file.get());
                const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
                _persistMicroseconds.fetch_add(spent, std::memory_order_relaxed);
            }
        }
        CATCH_LOG();
    }

    // How long the serializing has cost across every pane since the last time
    // anyone asked, in microseconds. Accumulated here because this is where the
    // work actually happens: the emperor only dispatches these and returns, so
    // timing its own loop would report ~0 forever and tell nobody anything.
    // Reading resets, so each persistence pass gets the cost of that pass.
    int64_t TermControl::TakePersistMicroseconds() noexcept
    {
        return _persistMicroseconds.exchange(0, std::memory_order_relaxed);
    }

    void TermControl::OpenCWD()
    {
        _core.OpenCWD();
    }

    void TermControl::Close()
    {
        if (!_IsClosing())
        {
            _closing = true;
            if (_automationPeer)
            {
                auto autoPeerImpl{ winrt::get_self<implementation::TermControlAutomationPeer>(_automationPeer) };
                autoPeerImpl->Close();
            }

            RestorePointerCursor.raise(*this, nullptr);

            _revokers = {};

            // At the time of writing, closing the last tab of a window inexplicably
            // does not lead to the destruction of the remaining TermControl instance(s).
            // On Win10 we don't destroy window threads due to bugs in DesktopWindowXamlSource.
            // In turn, we leak TermControl instances. This results in constant HWND messages
            // while the thread is supposed to be idle. Stop these timers avoids this.
            _bellLightTimer.Stop();

            // This is absolutely crucial, as the TSF code tries to hold a strong reference to _tsfDataProvider,
            // but right now _tsfDataProvider implements IUnknown as a no-op. This ensures that TSF stops referencing us.
            // ~TermControl() calls Close() so this should be safe.
            GetTSFHandle().Unfocus(&_tsfDataProvider);

            if (!_detached)
            {
                _interactivity.Close();
            }
        }
    }

    void TermControl::Detach()
    {
        _revokers = {};

        Control::ControlInteractivity old{ nullptr };
        std::swap(old, _interactivity);
        old.Detach();

        _detached = true;
    }

    // Method Description:
    // - Scrolls the viewport of the terminal and updates the scroll bar accordingly
    // Arguments:
    // - viewTop: the viewTop to scroll to
    void TermControl::ScrollViewport(int viewTop)
    {
        ScrollBar().Value(viewTop);
    }

    int TermControl::ScrollOffset() const
    {
        return _core.ScrollOffset();
    }

    // Gets the size of the terminal in cells.
    Core::Size TermControl::ViewportSize() const
    {
        return _core.ViewportSize();
    }

    int TermControl::BufferHeight() const
    {
        return _core.BufferHeight();
    }

    // Function Description:
    // - Determines how much space (in pixels) an app would need to reserve to
    //   create a control with the settings stored in the settings param. This
    //   accounts for things like the font size and face, the initialRows and
    //   initialCols, and scrollbar visibility. The returned sized is based upon
    //   the provided DPI value
    // Arguments:
    // - settings: A IControlSettings with the settings to get the pixel size of.
    // - dpi: The DPI we should create the terminal at. This affects things such
    //   as font size, scrollbar and other control scaling, etc. Make sure the
    //   caller knows what monitor the control is about to appear on.
    // - commandlineCols: Number of cols specified on the commandline
    // - commandlineRows: Number of rows specified on the commandline
    // Return Value:
    // - a size containing the requested dimensions in pixels.
    winrt::Windows::Foundation::Size TermControl::GetProposedDimensions(const IControlSettings& settings,
                                                                        const uint32_t dpi,
                                                                        int32_t commandlineCols,
                                                                        int32_t commandlineRows)
    {
        // If the settings have negative or zero row or column counts, ignore those counts.
        // Floor at MINIMUM_VISIBLE_CELLS so wt --size 1,1 / initialCols:1 cannot
        // open a 1-cell viewport (GH#19996).
        // (The lower TerminalCore layer also has upper bounds as well, but at this layer
        //  we may eventually impose different ones depending on how many pixels we can address.)
        const auto cols = static_cast<float>(std::max(commandlineCols > 0 ?
                                                          commandlineCols :
                                                          settings.InitialCols(),
                                                      MINIMUM_VISIBLE_CELLS));
        const auto rows = static_cast<float>(std::max(commandlineRows > 0 ?
                                                          commandlineRows :
                                                          settings.InitialRows(),
                                                      MINIMUM_VISIBLE_CELLS));

        const winrt::Windows::Foundation::Size initialSize{ cols, rows };

        return GetProposedDimensions(settings, dpi, initialSize);
    }

    // Function Description:
    // - Determines how much space (in pixels) an app would need to reserve to
    //   create a control with the settings stored in the settings param. This
    //   accounts for things like the font size and face, the initialRows and
    //   initialCols, and scrollbar visibility. The returned sized is based upon
    //   the provided DPI value
    // Arguments:
    // - initialSizeInChars: The size to get the proposed dimensions for.
    // - fontHeight: The font height to use to calculate the proposed size for.
    // - fontWeight: The font weight to use to calculate the proposed size for.
    // - fontFace: The font name to use to calculate the proposed size for.
    // - scrollState: The ScrollbarState to use to calculate the proposed size for.
    // - padding: The padding to use to calculate the proposed size for.
    // - dpi: The DPI we should create the terminal at. This affects things such
    //   as font size, scrollbar and other control scaling, etc. Make sure the
    //   caller knows what monitor the control is about to appear on.
    // Return Value:
    // - a size containing the requested dimensions in pixels.
    winrt::Windows::Foundation::Size TermControl::GetProposedDimensions(const IControlSettings& settings, const uint32_t dpi, const winrt::Windows::Foundation::Size& initialSizeInChars)
    {
        const auto cols = ::base::saturated_cast<int>(initialSizeInChars.Width);
        const auto rows = ::base::saturated_cast<int>(initialSizeInChars.Height);
        const auto fontSize = settings.FontSize();
        const auto fontWeight = settings.FontWeight();
        const auto fontFace = settings.FontFace();
        const auto cellWidth = CSSLengthPercentage::FromString(settings.CellWidth().c_str());
        const auto cellHeight = CSSLengthPercentage::FromString(settings.CellHeight().c_str());
        const auto scrollState = settings.ScrollState();
        const auto padding = settings.Padding();

        // Initialize our font information.
        // The font width doesn't terribly matter, we'll only be using the
        //      height to look it up
        // The other params here also largely don't matter.
        //      The family is only used to determine if the font is truetype or
        //      not, but DX doesn't use that info at all.
        //      The Codepage is additionally not actually used by the DX engine at all.
        FontInfoDesired desiredFont{ fontFace, 0, fontWeight.Weight, fontSize, CP_UTF8 };
        desiredFont.SetCellSize(cellWidth, cellHeight);
        FontInfo actualFont{ fontFace, 0, fontWeight.Weight, desiredFont.GetEngineSize(), CP_UTF8, false };

        // Create a DX engine and initialize it with our font and DPI. We'll
        // then use it to measure how much space the requested rows and columns
        // will take up.
        // TODO: MSFT:21254947 - use a static function to do this instead of
        // instantiating a AtlasEngine.
        // GH#10211 - UNDER NO CIRCUMSTANCE should this fail. If it does, the
        // whole app will crash instantaneously on launch, which is no good.
        const auto engine = std::make_unique<::Microsoft::Console::Render::AtlasEngine>();
        LOG_IF_FAILED(engine->UpdateDpi(dpi));
        LOG_IF_FAILED(engine->UpdateFont(desiredFont, actualFont));

        const auto scale = dpi / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
        const auto actualFontSize = actualFont.GetSize();

        // UWP XAML scrollbars aren't guaranteed to be the same size as the
        // ComCtl scrollbars, but it's certainly close enough.
        const auto scrollbarSize = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);

        float width = cols * static_cast<float>(actualFontSize.width);

        // Reserve additional space if scrollbar is intended to be visible
        if (scrollState != ScrollbarState::Hidden)
        {
            width += scrollbarSize;
        }

        float height = rows * static_cast<float>(actualFontSize.height);
        const auto thickness = StringToXamlThickness(padding);
        // GH#2061 - make sure to account for the size the padding _will be_ scaled to
        width += scale * static_cast<float>(thickness.Left + thickness.Right);
        height += scale * static_cast<float>(thickness.Top + thickness.Bottom);

        return { width, height };
    }

    // Function Description:
    // - Calculates new dimensions (in pixels) from row and column counts.
    // Arguments:
    // - dpi: The dpi value.
    // - sizeInChars: The size to get the new dimensions for.
    // Return Value:
    // - a size containing the requested dimensions in pixels.
    winrt::Windows::Foundation::Size TermControl::GetNewDimensions(const winrt::Windows::Foundation::Size& sizeInChars)
    {
        const auto cols = std::max(::base::saturated_cast<int32_t>(sizeInChars.Width), MINIMUM_VISIBLE_CELLS);
        const auto rows = std::max(::base::saturated_cast<int32_t>(sizeInChars.Height), MINIMUM_VISIBLE_CELLS);
        const auto fontSize = _core.FontSize();
        const auto scrollState = _core.Settings().ScrollState();
        const auto padding = _core.Settings().Padding();
        const auto scale = static_cast<float>(DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
        float width = cols * static_cast<float>(fontSize.Width);
        float height = rows * static_cast<float>(fontSize.Height);

        // Reserve additional space if scrollbar is intended to be visible
        if (scrollState != ScrollbarState::Hidden)
        {
            // UWP XAML scrollbars aren't guaranteed to be the same size as the
            // ComCtl scrollbars, but it's certainly close enough.
            const auto dpi = ::base::saturated_cast<uint32_t>(USER_DEFAULT_SCREEN_DPI * scale);
            const auto scrollbarSize = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
            width += scrollbarSize;
        }

        const auto thickness = StringToXamlThickness(padding);
        // GH#2061 - make sure to account for the size the padding _will be_ scaled to
        width += scale * static_cast<float>(thickness.Left + thickness.Right);
        height += scale * static_cast<float>(thickness.Top + thickness.Bottom);

        return { width, height };
    }

    // Method Description:
    // - Get the size of a single character of this control. The size is in
    //   _pixels_. If you want it in DIPs, you'll need to DIVIDE by the
    //   current display scaling.
    // Arguments:
    // - <none>
    // Return Value:
    // - The dimensions of a single character of this control, in DIPs
    winrt::Windows::Foundation::Size TermControl::CharacterDimensions() const
    {
        return _core.FontSizeInDips();
    }

    // Method Description:
    // - Get the absolute minimum size that this control can be resized to and
    //   still have 2x2 characters visible. This includes the space needed for
    //   the scrollbar and the padding.
    //   2x2 is the VT theoretical minimum (DECSTBM / DECSLRM). A 1-cell
    //   viewport can hang TextBuffer::Reflow on a wide glyph (GH#19996).
    // Arguments:
    // - <none>
    // Return Value:
    // - The minimum size that this terminal control can be resized to and still
    //   have a usable character grid.
    winrt::Windows::Foundation::Size TermControl::MinimumSize()
    {
        if (_initializedTerminal)
        {
            const auto fontSize = _core.FontSizeInDips();
            auto width = fontSize.Width * MINIMUM_VISIBLE_CELLS;
            auto height = fontSize.Height * MINIMUM_VISIBLE_CELLS;
            // Reserve additional space if scrollbar is intended to be visible
            if (_core.Settings().ScrollState() != ScrollbarState::Hidden)
            {
                width += static_cast<float>(ScrollBar().ActualWidth());
            }

            // Account for the size of any padding
            const auto padding = GetPadding();
            width += static_cast<float>(padding.Left + padding.Right);
            height += static_cast<float>(padding.Top + padding.Bottom);

            return { width, height };
        }
        else
        {
            return { 10, 10 };
        }
    }

    // Method Description:
    // - Adjusts given dimension (width or height) so that it aligns to the character grid.
    //   The snap is always downward.
    // Arguments:
    // - widthOrHeight: if true operates on width; otherwise, on height
    // - dimension: a dimension (width or height) to be snapped
    // Return Value:
    // - A dimension that would be aligned to the character grid.
    float TermControl::SnapDimensionToGrid(const bool widthOrHeight, const float dimension)
    {
        const auto fontSize = _core.FontSizeInDips();
        const auto fontDimension = widthOrHeight ? fontSize.Width : fontSize.Height;

        const auto padding = GetPadding();
        auto nonTerminalArea = gsl::narrow_cast<float>(widthOrHeight ?
                                                           padding.Left + padding.Right :
                                                           padding.Top + padding.Bottom);

        if (widthOrHeight && _core.Settings().ScrollState() != ScrollbarState::Hidden)
        {
            nonTerminalArea += gsl::narrow_cast<float>(ScrollBar().ActualWidth());
        }

        const auto gridSize = dimension - nonTerminalArea;
        const auto cells = floor(gridSize / fontDimension);
        return cells * fontDimension + nonTerminalArea;
    }

    // Method Description:
    // - Forwards window visibility changing event down into the control core
    //   to eventually let the hosting PTY know whether the window is visible or
    //   not (which can be relevant to `::GetConsoleWindow()` calls.)
    // Arguments:
    // - showOrHide: Show is true; hide is false.
    // Return Value:
    // - <none>
    void TermControl::WindowVisibilityChanged(const bool showOrHide)
    {
        _core.WindowVisibilityChanged(showOrHide);
    }

    // Method Description:
    // - Get the modifier keys that are currently pressed. This can be used to
    //   find out which modifiers (ctrl, alt, shift) are pressed in events that
    //   don't necessarily include that state.
    // Return Value:
    // - The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
    ControlKeyStates TermControl::_GetPressedModifierKeys() noexcept
    {
        const auto window = CoreWindow::GetForCurrentThread();
        // DONT USE
        //      != CoreVirtualKeyStates::None
        // OR
        //      == CoreVirtualKeyStates::Down
        // Sometimes with the key down, the state is Down | Locked.
        // Sometimes with the key up, the state is Locked.
        // IsFlagSet(Down) is the only correct solution.

        struct KeyModifier
        {
            VirtualKey vkey;
            ControlKeyStates flags;
        };

        constexpr std::array<KeyModifier, 7> modifiers{ {
            { VirtualKey::RightMenu, ControlKeyStates::RightAltPressed },
            { VirtualKey::LeftMenu, ControlKeyStates::LeftAltPressed },
            { VirtualKey::RightControl, ControlKeyStates::RightCtrlPressed },
            { VirtualKey::LeftControl, ControlKeyStates::LeftCtrlPressed },
            { VirtualKey::Shift, ControlKeyStates::ShiftPressed },
            { VirtualKey::RightWindows, ControlKeyStates::RightWinPressed },
            { VirtualKey::LeftWindows, ControlKeyStates::LeftWinPressed },
        } };

        ControlKeyStates flags;

        for (const auto& mod : modifiers)
        {
            const auto state = window.GetKeyState(mod.vkey);
            const auto isDown = WI_IsFlagSet(state, CoreVirtualKeyStates::Down);

            if (isDown)
            {
                flags |= mod.flags;
            }
        }

        constexpr std::array<KeyModifier, 3> modalities{ {
            { VirtualKey::CapitalLock, ControlKeyStates::CapslockOn },
            { VirtualKey::NumberKeyLock, ControlKeyStates::NumlockOn },
            { VirtualKey::Scroll, ControlKeyStates::ScrolllockOn },
        } };

        for (const auto& mod : modalities)
        {
            const auto state = window.GetKeyState(mod.vkey);
            const auto isLocked = WI_IsFlagSet(state, CoreVirtualKeyStates::Locked);

            if (isLocked)
            {
                flags |= mod.flags;
            }
        }

        return flags;
    }

    winrt::Windows::Foundation::Point TermControl::_toControlOrigin(const til::point terminalPos)
    {
        const auto fontSize{ CharacterDimensions() };
        auto padding{ GetPadding() };

        // Convert text buffer cursor position to client coordinate position within the window.
        return {
            terminalPos.x * fontSize.Width + static_cast<float>(padding.Left),
            terminalPos.y * fontSize.Height + static_cast<float>(padding.Top),
        };
    }

    // Method Description:
    // - Gets the corresponding viewport pixel position for the cursor
    //    by excluding the padding.
    // Arguments:
    // - cursorPosition: the (x,y) position of a given cursor (i.e.: mouse cursor).
    //    NOTE: origin (0,0) is top-left.
    // Return Value:
    // - the corresponding viewport terminal position (in pixels) for the given Point parameter
    Core::Point TermControl::_toTerminalOrigin(winrt::Windows::Foundation::Point cursorPosition)
    {
        // cursorPosition is DIPs, relative to SwapChainPanel origin
        const auto padding = GetPadding();

        // This point is the location of the cursor within the actual grid of characters, in DIPs
        const auto relativeToMarginInDIPsX = cursorPosition.X - static_cast<float>(padding.Left);
        const auto relativeToMarginInDIPsY = cursorPosition.Y - static_cast<float>(padding.Top);

        // Convert it to pixels
        const auto scale = SwapChainPanel().CompositionScaleX();

        return {
            lroundf(relativeToMarginInDIPsX * scale),
            lroundf(relativeToMarginInDIPsY * scale),
        };
    }

    // Method Description:
    // - Async handler for the "Drop" event. If a file was dropped onto our
    //   root, we'll try to get the path of the file dropped onto us, and write
    //   the full path of the file to our terminal connection. Like conhost, if
    //   the path contains a space, we'll wrap the path in quotes.
    // - Unlike conhost, if multiple files are dropped onto the terminal, we'll
    //   write all the paths to the terminal, separated by the configured delimiter.
    // Arguments:
    // - e: The DragEventArgs from the Drop event
    // Return Value:
    // - <none>
    safe_void_coroutine TermControl::_DragDropHandler(Windows::Foundation::IInspectable /*sender*/,
                                                      DragEventArgs e)
    {
        if (_IsClosing())
        {
            co_return;
        }

        if (const auto hwnd = reinterpret_cast<HWND>(OwningHwnd()))
        {
            SetForegroundWindow(hwnd);
        }

        const auto weak = get_weak();

        if (e.DataView().Contains(StandardDataFormats::ApplicationLink()))
        {
            try
            {
                auto link{ co_await e.DataView().GetApplicationLinkAsync() };
                if (const auto strong = weak.get())
                {
                    _pasteTextWithBroadcast(link.AbsoluteUri());
                }
            }
            CATCH_LOG();
        }
        else if (e.DataView().Contains(StandardDataFormats::WebLink()))
        {
            try
            {
                auto link{ co_await e.DataView().GetWebLinkAsync() };
                if (const auto strong = weak.get())
                {
                    _pasteTextWithBroadcast(link.AbsoluteUri());
                }
            }
            CATCH_LOG();
        }
        else if (e.DataView().Contains(StandardDataFormats::Text()))
        {
            try
            {
                auto text{ co_await e.DataView().GetTextAsync() };
                if (const auto strong = weak.get())
                {
                    _pasteTextWithBroadcast(text);
                }
            }
            CATCH_LOG();
        }
        // StorageItem must be last. Some applications put hybrid data format items
        // in a drop message and we'll eat a crash when we request them.
        // Those applications usually include Text as well, so having storage items
        // last makes sure we'll hit text before getting to them.
        else if (e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            Windows::Foundation::Collections::IVectorView<Windows::Storage::IStorageItem> items;
            try
            {
                items = co_await e.DataView().GetStorageItemsAsync();
            }
            CATCH_LOG();

            if (items && items.Size() > 0)
            {
                std::vector<std::wstring> fullPaths;

                // GH#14628: Workaround for GetStorageItemsAsync() only returning 16 items
                // at most when dragging and dropping from archives (zip, 7z, rar, etc.)
                if (items.Size() == 16 && e.DataView().Contains(winrt::hstring{ L"FileDrop" }))
                {
                    auto fileDropData = co_await e.DataView().GetDataAsync(winrt::hstring{ L"FileDrop" });
                    if (fileDropData != nullptr)
                    {
                        auto stream = fileDropData.as<IRandomAccessStream>();
                        stream.Seek(0);

                        const uint32_t streamSize = gsl::narrow_cast<uint32_t>(stream.Size());
                        const Buffer buf(streamSize);
                        const auto buffer = co_await stream.ReadAsync(buf, streamSize, InputStreamOptions::None);

                        const HGLOBAL hGlobal = buffer.data();
                        const auto count = DragQueryFileW(static_cast<HDROP>(hGlobal), 0xFFFFFFFF, nullptr, 0);
                        fullPaths.reserve(count);

                        for (unsigned int i = 0; i < count; i++)
                        {
                            std::wstring path;
                            path.resize(wil::max_path_length);
                            const auto charsCopied = DragQueryFileW(static_cast<HDROP>(hGlobal), i, path.data(), wil::max_path_length);

                            if (charsCopied > 0)
                            {
                                path.resize(charsCopied);
                                fullPaths.emplace_back(std::move(path));
                            }
                        }
                    }
                }
                else
                {
                    fullPaths.reserve(items.Size());
                    for (const auto& item : items)
                    {
                        fullPaths.emplace_back(item.Path());
                    }
                }

                const auto strong = weak.get();
                if (!strong)
                {
                    co_return;
                }

                std::wstring allPathsString;
                const auto delimiter{ _core.Settings().DragDropDelimiter() };
                for (auto& fullPath : fullPaths)
                {
                    // Join the paths with the delimiter
                    if (!allPathsString.empty())
                    {
                        allPathsString += delimiter;
                    }

                    const auto translationStyle{ _core.Settings().PathTranslationStyle() };
                    _translatePathInPlace(fullPath, translationStyle);

                    // All translated paths get quotes, and all strings spaces get quotes; all translated paths get single quotes
                    const auto quotesNeeded = translationStyle != PathTranslationStyle::None || fullPath.find(L' ') != std::wstring::npos;
                    const auto quotesChar = translationStyle != PathTranslationStyle::None ? L'\'' : L'"';

                    // Append fullPath and also wrap it in quotes if needed
                    if (quotesNeeded)
                    {
                        allPathsString.push_back(quotesChar);
                    }
                    allPathsString.append(fullPath);
                    if (quotesNeeded)
                    {
                        allPathsString.push_back(quotesChar);
                    }
                }

                _pasteTextWithBroadcast(winrt::hstring{ allPathsString });
            }
        }
    }

    // Method Description:
    // - Paste this text, and raise a StringSent, to potentially broadcast this
    //   text to other controls in the app. For certain interactions, like
    //   drag/dropping a file, we want to act like we "pasted" the text (even if
    //   the text didn't come from the clipboard). This lets those interactions
    //   broadcast as well.
    void TermControl::_pasteTextWithBroadcast(const winrt::hstring& text)
    {
        // only broadcast if there's an actual listener. Saves the overhead of some object creation.
        if (StringSent)
        {
            StringSent.raise(*this, winrt::make<StringSentEventArgs>(text));
        }
        _core.PasteText(text);
    }

    // Method Description:
    // - Handle the DragOver event. We'll signal that the drag operation we
    //   support is the "copy" operation, and we'll also customize the
    //   appearance of the drag-drop UI, by removing the preview and setting a
    //   custom caption. For more information, see
    //   https://docs.microsoft.com/en-us/windows/uwp/design/input/drag-and-drop#customize-the-ui
    // Arguments:
    // - e: The DragEventArgs from the DragOver event
    // Return Value:
    // - <none>
    void TermControl::_DragOverHandler(const Windows::Foundation::IInspectable& /*sender*/,
                                       const DragEventArgs& e)
    {
        if (_IsClosing())
        {
            return;
        }

        // We can only handle drag/dropping StorageItems (files) and plain Text
        // currently. If the format on the clipboard is anything else, returning
        // early here will prevent the drag/drop from doing anything.
        if (!(e.DataView().Contains(StandardDataFormats::StorageItems()) ||
              e.DataView().Contains(StandardDataFormats::Text())))
        {
            return;
        }

        // Make sure to set the AcceptedOperation, so that we can later receive the path in the Drop event
        e.AcceptedOperation(DataPackageOperation::Copy);

        // Sets custom UI text
        if (e.DataView().Contains(StandardDataFormats::StorageItems()))
        {
            e.DragUIOverride().Caption(RS_(L"DragFileCaption"));
        }
        else if (e.DataView().Contains(StandardDataFormats::Text()))
        {
            e.DragUIOverride().Caption(RS_(L"DragTextCaption"));
        }

        // Sets if the caption is visible
        e.DragUIOverride().IsCaptionVisible(true);
        // Sets if the dragged content is visible
        e.DragUIOverride().IsContentVisible(false);
        // Sets if the glyph is visible
        e.DragUIOverride().IsGlyphVisible(false);
    }

    // Method description:
    // - Checks if the uri is valid and sends an event if so
    // Arguments:
    // - The uri
    safe_void_coroutine TermControl::_HyperlinkHandler(IInspectable /*sender*/,
                                                       Control::OpenHyperlinkEventArgs args)
    {
        // Save things we need to resume later.
        auto strongThis{ get_strong() };

        // Pop the rest of this function to the tail of the UI thread
        // Just in case someone was holding a lock when they called us and
        // the handlers decide to do something that take another lock
        // (like ShellExecute pumping our messaging thread...GH#7994)
        co_await winrt::resume_foreground(Dispatcher());

        // Which action this chord runs for this link. The rule that matched the hovered
        // run decides; _currentHyperlinkTooltipSettings was computed on hover. A link
        // that never came from a hover -- mark mode's Ctrl+Enter -- has no rule context,
        // so the globals are recomputed for it instead of reusing a stale hover.
        auto effective = _currentHyperlinkTooltipSettings;
        if (_hoveredUri.empty())
        {
            effective = _effectiveHyperlinkTooltipSettings(args.Uri(), false);
        }
        const auto actionId = args.IsAlternative() ? effective.alternativeAction : effective.primaryAction;

        // "open" falls through to the resolve-and-raise below, which is the only path
        // that can turn a text match into a real URI. Everything else is dispatched.
        if (!actionId.empty() && actionId != L"open")
        {
            _invokeHyperlinkActionById(actionId);
            _hideHyperlinkCard();
            co_return;
        }

        // A text match arrives here as the plain text it matched -- "CAB-8209", not a URI --
        // because that is what the buffer scanner found and what ControlCore reported. Only
        // an integration knows what it stands for, so give it the chance to say before the
        // event carries on to TerminalPage, which would otherwise refuse it as an invalid
        // URI. An ordinary link resolves to nothing here and passes through untouched.
        if (_hyperlinkPreviewProvider)
        {
            // Before asking what the match means, ask what opening it should DO.
            // Some things are not usefully "opened" at all: a multiplexer pane id
            // names a pane to bring to the front, and a page describing that pane
            // is a consolation prize. A matcher says so with `open`, and the
            // manifest's own action does the work -- no OS protocol handler, and
            // the same behaviour on every platform the manifest is read on.
            hstring openAction;
            try
            {
                openAction = _hyperlinkPreviewProvider.ResolveOpenAction(args.Uri(), effective.integration);
            }
            CATCH_LOG();

            hstring resolved;
            try
            {
                resolved = _hyperlinkPreviewProvider.ResolveLink(args.Uri(), effective.integration);
                if (resolved.empty() && effective.isTextMatch && openAction.empty())
                {
                    if (const auto preview = co_await _hyperlinkPreviewProvider.GetPreviewAsync(args.Uri(), effective.integration)) resolved = preview.ResolvedUri();
                }
            }
            CATCH_LOG();

            if (!openAction.empty())
            {
                // The link goes along as the fallback. An action that cannot
                // reach its far end -- the server is down, the pane is gone --
                // should not leave the click having done nothing at all, and
                // the page about the thing is a worse answer than focusing it
                // but a much better one than silence.
                _invokeHyperlinkOpenAction(args.Uri(), effective.integration, openAction, resolved);
                _hideHyperlinkCard();
                co_return;
            }

            if (!resolved.empty())
            {
                args = winrt::make<OpenHyperlinkEventArgs>(resolved);
            }
            else if (effective.isTextMatch)
            {
                // Nothing claimed it. Handing the raw run on means
                // Uri("ce786f3f-6fdc-4165-b9bd-20ff858844db") throws in
                // TerminalPage and the reader is told "This link is invalid",
                // which blames the text for a rule that matched more than any
                // integration knows how to resolve. Do nothing instead; the
                // card's own buttons are still there.
                _hideHyperlinkCard();
                co_return;
            }
        }

        OpenHyperlink.raise(*strongThis, args);
    }

    // Runs the action a matcher bound to opening. `openAction` is either one of
    // the card's built-in button ids, in which case it means exactly what
    // pressing that button means, or the key of an `actions` entry in the owning
    // manifest, which is invoked the same way the card invokes it.
    //
    // Nothing is refreshed afterwards, unlike the card's own action buttons: the
    // card is dismissed by the click that got here, so there is no preview left
    // on screen for a refresh to correct. A hover after the action fetches
    // again anyway, and these actions are the ones whose effect is somewhere
    // else -- a pane comes forward, a window is raised -- rather than a status
    // badge on the card itself.
    safe_void_coroutine TermControl::_invokeHyperlinkOpenAction(hstring text, hstring integration, hstring openAction, hstring fallbackUri)
    {
        auto strongThis{ get_strong() };

        const std::wstring_view id{ openAction };
        if (til::equals_insensitive_ascii(id, L"open") ||
            til::equals_insensitive_ascii(id, L"copyLink") ||
            til::equals_insensitive_ascii(id, L"copyPath") ||
            til::equals_insensitive_ascii(id, L"reveal") ||
            til::equals_insensitive_ascii(id, L"showInPane"))
        {
            _invokeHyperlinkActionById(openAction);
            co_return;
        }

        Control::HyperlinkActionResult result{ nullptr };
        if (_hyperlinkPreviewProvider)
        {
            try
            {
                const auto fields = winrt::single_threaded_map<hstring, hstring>();
                result = co_await _hyperlinkPreviewProvider.InvokeActionAsync(text, integration, openAction, {}, fields);
            }
            CATCH_LOG();
        }

        if (result && result.Ok())
        {
            co_return;
        }

        if (fallbackUri.empty())
        {
            co_return;
        }

        co_await wil::resume_foreground(Dispatcher());
        OpenHyperlink.raise(*strongThis, winrt::make<OpenHyperlinkEventArgs>(fallbackUri));
    }

    // Method Description:
    // - Produces the error dialog that notifies the user that rendering cannot proceed.
    safe_void_coroutine TermControl::_RendererEnteredErrorState(IInspectable /*sender*/,
                                                                IInspectable /*args*/)
    {
        auto strongThis{ get_strong() };
        co_await winrt::resume_foreground(Dispatcher()); // pop up onto the UI thread

        if (auto loadedUiElement{ FindName(L"RendererFailedNotice") })
        {
            if (auto uiElement{ loadedUiElement.try_as<::winrt::Windows::UI::Xaml::UIElement>() })
            {
                uiElement.Visibility(Visibility::Visible);
            }
        }
    }

    // Method Description:
    // - Responds to the Click event on the button that will re-enable the renderer.
    void TermControl::_RenderRetryButton_Click(const IInspectable& /*sender*/, const IInspectable& /*args*/)
    {
        // It's already loaded if we get here, so just hide it.
        RendererFailedNotice().Visibility(Visibility::Collapsed);
        _core.ResumeRendering();
    }

    IControlSettings TermControl::Settings() const
    {
        return _core.Settings();
    }

    Windows::Foundation::IReference<winrt::Windows::UI::Color> TermControl::TabColor() noexcept
    {
        // NOTE TO FUTURE READERS: TabColor is down in the Core for the
        // hypothetical future where we allow an application to set the tab
        // color with VT sequences like they're currently allowed to with the
        // title.
        return _core.TabColor();
    }

    // Method Description:
    // - Gets the internal taskbar state value
    // Return Value:
    // - The taskbar state of this control
    const uint64_t TermControl::TaskbarState() const noexcept
    {
        return _core.TaskbarState();
    }

    // Method Description:
    // - Gets the internal taskbar progress value
    // Return Value:
    // - The taskbar progress of this control
    const uint64_t TermControl::TaskbarProgress() const noexcept
    {
        return _core.TaskbarProgress();
    }

    void TermControl::BellLightOn()
    {
        // Initialize the animation if it does not exist
        // We only initialize here instead of in the ctor because depending on the bell style setting,
        // we may never need this animation
        if (!_bellLightAnimation && !_isBackgroundLight)
        {
            _bellLightAnimation = Window::Current().Compositor().CreateScalarKeyFrameAnimation();
            // Add key frames and a duration to our bell light animation
            _bellLightAnimation.InsertKeyFrame(0.0f, 4.0f);
            _bellLightAnimation.InsertKeyFrame(1.0f, 1.9f);
            _bellLightAnimation.Duration(winrt::Windows::Foundation::TimeSpan(std::chrono::milliseconds(TerminalWarningBellInterval)));
        }

        // Likewise, initialize the dark version of the animation only if required
        if (!_bellDarkAnimation && _isBackgroundLight)
        {
            _bellDarkAnimation = Window::Current().Compositor().CreateScalarKeyFrameAnimation();
            // reversing the order of the intensity values produces a similar effect as the light version
            _bellDarkAnimation.InsertKeyFrame(0.0f, 1.0f);
            _bellDarkAnimation.InsertKeyFrame(1.0f, 2.0f);
            _bellDarkAnimation.Duration(winrt::Windows::Foundation::TimeSpan(std::chrono::milliseconds(TerminalWarningBellInterval)));
        }

        Windows::Foundation::Numerics::float2 zeroSize{ 0, 0 };
        // If the grid has 0 size or if the bell timer is
        // already active, do nothing
        if (RootGrid().ActualSize() != zeroSize && !_bellLightTimer.IsEnabled())
        {
            _bellLightTimer.Interval(std::chrono::milliseconds(TerminalWarningBellInterval));
            _bellLightTimer.Tick({ get_weak(), &TermControl::_BellLightOff });
            _bellLightTimer.Start();

            // Switch on the light and animate the intensity to fade out
            VisualBellLight::SetIsTarget(RootGrid(), true);

            auto compositionLight{ BellLight().as<Windows::UI::Xaml::Media::IXamlLightProtected>().CompositionLight() };
            if (_isBackgroundLight)
            {
                compositionLight.StartAnimation(L"Intensity", _bellDarkAnimation);
            }
            else
            {
                compositionLight.StartAnimation(L"Intensity", _bellLightAnimation);
            }
        }
    }

    void TermControl::_BellLightOff(const Windows::Foundation::IInspectable& /* sender */,
                                    const Windows::Foundation::IInspectable& /* e */)
    {
        // Stop the timer and switch off the light
        _bellLightTimer.Stop();

        if (!_IsClosing())
        {
            VisualBellLight::SetIsTarget(RootGrid(), false);
        }
    }

    // Method Description:
    // - Checks whether the control is in a read-only mode (in this mode node input is sent to connection).
    // Return Value:
    // - True if the mode is read-only
    bool TermControl::ReadOnly() const noexcept
    {
        return _core.IsInReadOnlyMode();
    }

    // Method Description:
    // - Toggles the read-only flag, raises event describing the value change
    void TermControl::ToggleReadOnly()
    {
        _core.ToggleReadOnlyMode();
        ReadOnlyChanged.raise(*this, winrt::box_value(_core.IsInReadOnlyMode()));
    }

    // Method Description:
    // - Sets the read-only flag, raises event describing the value change
    void TermControl::SetReadOnly(const bool readOnlyState)
    {
        _core.SetReadOnlyMode(readOnlyState);
        ReadOnlyChanged.raise(*this, winrt::box_value(_core.IsInReadOnlyMode()));
    }

    // Method Description:
    // - Handle a mouse exited event, specifically clearing last hovered cell
    // and removing selection from hyper link if exists
    // Arguments:
    // - sender: not used
    // - args: event data
    void TermControl::_PointerExitedHandler(const Windows::Foundation::IInspectable& /*sender*/,
                                            const Windows::UI::Xaml::Input::PointerRoutedEventArgs& /*e*/)
    {
        _core.ClearHoveredCell();

        // The card sits in the overlay canvas rather than inside the element that raises
        // this, so setting out for its buttons leaves the terminal and lands here. Hiding
        // now would make them unreachable by mouse; the grace period is the gap the pointer
        // is crossing.
        _scheduleHyperlinkCardHide();
    }

    // Turns a list of button ids into the five bools the card actually reads. An
    // unknown id is ignored rather than rejected: the settings model is free to grow
    // a sixth button without this becoming the place that refuses to show it.
    EffectiveHyperlinkTooltipSettings TermControl::_effectiveHyperlinkTooltipSettings(std::wstring_view uri, bool isFileLink) const
    {
        return ResolveHyperlinkRules(_core.Settings(), uri, isFileLink);
    }

    void TermControl::_hoveredHyperlinkChanged(const IInspectable& /*sender*/, const IInspectable& /*args*/)
    {
        const auto lastHoveredCell = _core.HoveredCell();
        auto uriText = lastHoveredCell ? _core.HoveredUriText() : hstring{};
        if (uriText.empty())
        {
            // Off the link. Don't hide at once -- the pointer may be on its way to the card.
            _lastPanePreviewUri = {};
            auto ended = winrt::make<ShowHyperlinkPreviewRequestedEventArgs>(L"", L"", L"");
            ended.IsHover(true);
            ended.CreatePane(false);
            ShowHyperlinkPreviewRequested.raise(*this, ended);
            _scheduleHyperlinkCardHide();
            return;
        }

        // PointerMoved is bound on an ancestor of HyperlinkCard, and the card never marks
        // the event handled, so it keeps bubbling up even while the pointer is visually over
        // the card -- e.g. on its way to one of its buttons. When that happens, ControlCore
        // is only reporting whatever hyperlink sits on the terminal cell behind the card, not
        // something the user is actually pointing at. Ignore it entirely -- don't touch the
        // card's text, buttons, or position -- until the pointer actually leaves the card.
        // The options dropdown is the same case: its popup is not part of the card, so
        // the pointer being on the list reads as "not over the card" here too.
        if ((_pointerInHyperlinkCard || _hyperlinkActionDropDownOpen || Control::HyperlinkPreviewHelpers::HasNestedPreview()) && HyperlinkCard().Visibility() == Visibility::Visible)
        {
            return;
        }

        if (_hoveredUri == uriText && HyperlinkCard().Visibility() == Visibility::Visible)
        {
            _hyperlinkHideTimer.Stop();
            _showHyperlinkCard();
            return;
        }

        // Keep the URI as the buffer holds it. The text below may gain a punycode
        // annotation, which is there to be read, not to be opened or copied.
        _hoveredUri = uriText;

        // A matching HyperlinkTooltipRule (if any) can override the delays, max width,
        // built-in button visibility, and custom action list for this specific link. It is
        // computed here rather than further down because it also says whether the hovered
        // run is a URI at all: a text-kind rule matches plain terminal text (an issue key,
        // say), which Windows::Foundation::Uri below would reject outright.
        const auto target = _resolvedHyperlinkTarget();
        _currentHyperlinkTooltipSettings = _effectiveHyperlinkTooltipSettings(_hoveredUri, !target.empty());

        // When the pane is the preferred surface, hovering opens one instead of the
        // card. Nothing below this point runs: there is no card to fill in, and the
        // pane fetches its own copy of the preview from the same provider. The
        // last-raised check matters because this fires on every pointer move across
        // the link, and re-asking would refetch the same thing dozens of times.
        if (_currentHyperlinkTooltipSettings.preferPane || _hyperlinkTooltipsSuppressed)
        {
            _hideHyperlinkCard();
            if (_lastPanePreviewUri != _hoveredUri)
            {
                _lastPanePreviewUri = _hoveredUri;
                _raiseShowHyperlinkPreviewRequested(true, _currentHyperlinkTooltipSettings.preferPane);
            }
            return;
        }
        _lastPanePreviewUri = {};
        // Pinned panes also accept temporary hovers when popovers remain enabled.
        _raiseShowHyperlinkPreviewRequested(true, false);

        // A preview belongs to exactly one hover. Drop the last one before anything below
        // reads it, and move the generation on so a fetch still in flight for the previous
        // link is discarded rather than painted into the card this one is about to show.
        _currentHyperlinkPreview = nullptr;
        const auto previewGeneration = ++_hyperlinkPreviewGeneration;

        // A bare absolute POSIX path (no scheme, e.g. printed by a tool running
        // inside WSL) is never a valid Windows::Foundation::Uri -- schemes are
        // always [a-z][a-z0-9+.-]*:, which can't start with a slash. It's still a
        // perfectly good path that _resolvedHyperlinkTarget below can resolve the
        // same way it already resolves file:// links, so it skips the homoglyph/
        // punycode handling entirely rather than hitting the catch below and
        // being shown as "Invalid URI".
        const auto looksLikeBarePosixPath = !target.empty();

        // Attackers abuse Unicode characters that happen to look similar to ASCII characters. Cyrillic for instance has
        // its own glyphs for а, с, е, о, р, х, and у that look practically identical to their ASCII counterparts.
        // This is called an "IDN homoglyph attack".
        //
        // But outright showing Punycode URIs only is similarly flawed as they can end up looking similar to valid ASCII URIs.
        // xn--cnn.com for instance looks confusingly similar to cnn.com, but actually represents U+407E.
        //
        // An optimal solution would detect any URI that contains homoglyphs and show them in their Punycode form.
        // Such a detector however is not quite trivial and requires constant maintenance, which this project's
        // maintainers aren't currently well equipped to handle. As such we do the next best thing and show the
        // Punycode encoding side-by-side with the Unicode string for any IDN.
        // A text match is skipped for the same reason a bare POSIX path is: it is not a URI,
        // so the Uri constructor below throws and the card would announce the user's own
        // issue key as "Invalid URI".
        if (!looksLikeBarePosixPath && !_currentHyperlinkTooltipSettings.isTextMatch)
        {
            try
            {
                // DisplayUri/Iri drop authentication credentials, which is probably great, but AbsoluteCanonicalUri()
                // is the only getter that returns a punycode encoding of the URL. AbsoluteUri() is the only possible
                // counterpart, but as the name indicates, we'll end up hitting the != below for any non-canonical URL.
                //
                // This issue can be fixed by using the IUrl API from urlmon.h directly, which the WinRT API simply wraps.
                // IUrl is a very complex system with a ton of useful functionality, but we don't rely on it (neither WinRT),
                // so we could alternatively use its underlying API in wininet.h (InternetCrackUrlW, etc.).
                // That API however is rather difficult to use for such seldom executed code.
                const Windows::Foundation::Uri uri{ uriText };
                const auto unicode = uri.AbsoluteUri();
                const auto punycode = uri.AbsoluteCanonicalUri();

                if (punycode != unicode)
                {
                    const auto text = fmt::format(FMT_COMPILE(L"{}\n({})"), punycode, unicode);
                    uriText = winrt::hstring{ text };
                }
            }
            catch (...)
            {
                uriText = RS_(L"InvalidUri");
            }
        }

        _formatHighlightedUri(uriText);

        // Say where a file:// link will really go when that is not what it says. A POSIX
        // path printed inside WSL opens as \\wsl.localhost\<distro>\..., and seeing that
        // before clicking is the difference between trusting a link and finding out after.
        // A text match gets the same line for a different reason: the match itself is not a
        // link, so the link it stands for is the only thing that says what Open will do. It
        // may not be known yet -- an integration that has to resolve it fills the line in
        // when the preview lands.
        const auto linkTarget = _hoveredLinkTarget();
        const auto shownTarget = _currentHyperlinkTooltipSettings.isTextMatch ? std::wstring{ linkTarget } : target;
        const auto showTarget = !shownTarget.empty() && shownTarget != std::wstring_view{ _hoveredUri };
        if (showTarget)
        {
            HyperlinkCardTarget().Text(winrt::hstring{ L"\u2192 " + shownTarget });
        }
        HyperlinkCardTarget().Visibility(showTarget ? Visibility::Visible : Visibility::Collapsed);

        // Open and Copy link need a link to act on, which for a text match only exists once
        // something has resolved it. Copying a path and revealing one only mean anything for
        // a file -- all four on top of whatever the effective settings already suppressed.
        const auto hasLink = !linkTarget.empty();
        HyperlinkOpenButton().Visibility(hasLink && _currentHyperlinkTooltipSettings.showOpen ? Visibility::Visible : Visibility::Collapsed);
        HyperlinkCopyLinkButton().Visibility(hasLink && _currentHyperlinkTooltipSettings.showCopyLink ? Visibility::Visible : Visibility::Collapsed);
        const auto fileActions = !target.empty() && _currentHyperlinkTooltipSettings.showCopyPath;
        const auto fileReveal = !target.empty() && _currentHyperlinkTooltipSettings.showReveal;
        HyperlinkCopyPathButton().Visibility(fileActions ? Visibility::Visible : Visibility::Collapsed);
        HyperlinkRevealButton().Visibility(fileReveal ? Visibility::Visible : Visibility::Collapsed);
        // Show in pane needs somewhere to send the request and something worth
        // sending: a link nobody can preview would open an empty pane.
        HyperlinkShowInPaneButton().Visibility(_currentHyperlinkTooltipSettings.showInPane ? Visibility::Visible : Visibility::Collapsed);

        // Before _applyActionPlacement, which decides whether either host has anything
        // in it worth showing and so needs the rule line's visibility settled first.
        // Placement runs a second time from _showHyperlinkCard once the card has been
        // positioned, because only then is it known which edge is nearest the link.
        _applyRuleInfo();
        _applyActionPlacement();

        // The hint is about following the link, so it only makes sense when there is
        // a link to follow -- and only when the user has asked to keep it.
        HowToOpenRun().Visibility(_currentHyperlinkTooltipSettings.showHint && !_currentHyperlinkTooltipSettings.isTextMatch ?
                                      Visibility::Visible :
                                      Visibility::Collapsed);

        // Ask the integration layer for a preview, if one is possible at all: the provider
        // is only set once the app has integrations, the matched rule may have opted out,
        // and CanPreview is what knows whether any enabled integration claims this text.
        // The answer decides whether the card opens with a spinner or with nothing extra.
        auto wantPreview = false;
        if (_hyperlinkPreviewProvider &&
            _currentHyperlinkTooltipSettings.showPreview &&
            _currentHyperlinkTooltipSettings.integration != L"none")
        {
            try
            {
                wantPreview = (!target.empty() && _currentHyperlinkTooltipSettings.integration.empty()) ||
                              _hyperlinkPreviewProvider.CanPreview(_hoveredUri, _currentHyperlinkTooltipSettings.integration);
            }
            CATCH_LOG();
        }
        _setHyperlinkPreviewLoading(wantPreview);
        if (wantPreview)
        {
            _requestHyperlinkPreview(previewGeneration, _hoveredUri, _currentHyperlinkTooltipSettings.integration);
        }

        // Already up: it has only moved to a different link, so reposition and leave it.
        _hyperlinkHideTimer.Stop();
        if (HyperlinkCard().Visibility() == Visibility::Visible)
        {
            _showHyperlinkCard();
            return;
        }

        const auto delay = _currentHyperlinkTooltipSettings.showDelay;
        if (delay == 0)
        {
            _showHyperlinkCard();
            return;
        }

        if (!_hyperlinkShowTimer)
        {
            _hyperlinkShowTimer.Tick([weakThis = get_weak()](auto&&, auto&&) {
                if (const auto self = weakThis.get(); self && !self->_IsClosing())
                {
                    self->_hyperlinkShowTimer.Stop();
                    self->_showHyperlinkCard();
                }
            });
        }
        _hyperlinkShowTimer.Stop();
        _hyperlinkShowTimer.Interval(std::chrono::milliseconds{ delay });
        _hyperlinkShowTimer.Start();
    }

    // Resolves a file:// link -- or a bare absolute POSIX path with no scheme at
    // all, which a tool running inside WSL prints far more often than an actual
    // file:// URI -- to the path it will actually open. Empty string for
    // anything else. The distro lookup is shared with TerminalPage rather than
    // repeated, so what the card promises and what a click delivers cannot
    // drift apart.
    std::wstring TermControl::_resolvedHyperlinkTarget() const
    {
        if (_hoveredUri.empty())
        {
            return {};
        }

        if (_currentHyperlinkPreview && _currentHyperlinkPreview.SourceText() == _hoveredUri && !_currentHyperlinkPreview.FilePath().empty())
        {
            return std::wstring{ _currentHyperlinkPreview.FilePath() };
        }
        const std::wstring_view hovered{ _hoveredUri };
        if (hovered.starts_with(L"\\\\") || (hovered.size() > 2 && hovered[1] == L':' && (hovered[2] == L'\\' || hovered[2] == L'/')))
        {
            return std::wstring{ hovered };
        }
        const auto isFileUri = til::starts_with_insensitive_ascii(hovered, L"file:");
        const auto isBarePosixPath = hovered[0] == L'/' || hovered.starts_with(L"~/");
        if (!isFileUri && !isBarePosixPath)
        {
            return {};
        }

        const auto settings = _core.Settings();
        if (!settings)
        {
            return {};
        }

        const auto distro = ::Microsoft::Console::Utils::WslDistroForCommandline(
            settings.Commandline(),
            settings.PathTranslationStyle() == PathTranslationStyle::WSL);

        if (!isFileUri)
        {
            // UI thread, so a WSL home is only what is cached; _ApplyUISettings warms it.
            const auto candidates = Lintel::PathCandidates(hovered, true, distro, {}, Mullion::PathHomesFor(distro, false));
            if (candidates.empty() && Lintel::ClassifyPath(hovered) == Lintel::PathKind::Home)
            {
                // Home not known yet. Better no target than "/.claude/..." resolved
                // against the distribution's root, which is what this used to show.
                return {};
            }
            // An unknown distribution is resolved asynchronously by the file provider.
            return candidates.empty() ? std::wstring{ hovered } : candidates.front().path;
        }
        return ::Microsoft::Console::Utils::ResolveFileUriTarget(
            ::Microsoft::Console::Utils::StripUriFragment(_hoveredUri), distro);
    }

    // What Open, Copy link and a click should actually act on. For an ordinary link that is
    // the hovered URI itself. For a text match it is not: the hovered run is a bare piece of
    // terminal text -- an issue key, a session id -- and only an integration knows which URL
    // it stands for. Prefer what the preview already resolved, and fall back to asking the
    // provider directly, which is both how the buttons work before the fetch completes and
    // how they work when there is no preview to fetch at all.
    winrt::hstring TermControl::_hoveredLinkTarget() const
    {
        if (_hoveredUri.empty())
        {
            return {};
        }

        if (Lintel::ClassifyPath(std::wstring_view{ _hoveredUri }) != Lintel::PathKind::None)
        {
            const auto path = _resolvedHyperlinkTarget();
            const auto kind = Lintel::ClassifyPath(path);
            if (path.empty() || kind == Lintel::PathKind::Posix || kind == Lintel::PathKind::Home) return {}; // Still unresolved or ambiguous.
            return winrt::hstring{ ::Microsoft::Console::Utils::FilePathToUri(path) };
        }

        if (_currentHyperlinkPreview)
        {
            if (const auto resolved = _currentHyperlinkPreview.ResolvedUri(); !resolved.empty())
            {
                return resolved;
            }
        }

        // Asked for a link match too, not only a text one. A manifest that owns
        // a scheme declares the http(s) form of it, and that form is what Open
        // and Copy link should act on -- stith://session/<id> is not something
        // the shell can be handed. A matcher that declares no `link` resolves
        // to nothing and the hovered URI is used unchanged, which is every
        // ordinary http link.
        if (_hyperlinkPreviewProvider)
        {
            try
            {
                if (auto resolved = _hyperlinkPreviewProvider.ResolveLink(_hoveredUri, _currentHyperlinkTooltipSettings.integration);
                    !resolved.empty())
                {
                    return resolved;
                }
            }
            CATCH_LOG();
        }

        // A text match nothing claims has no target at all. Returning the raw
        // run would put "CAB-8209" where a URL belongs, and everything
        // downstream would go on to treat it as one.
        return _currentHyperlinkTooltipSettings.isTextMatch ? winrt::hstring{} : _hoveredUri;
    }

    // The preview section exists only while it has something to say: a fetch in flight (the
    // ring), or the result of one. Everything is cleared on the way in either way, because
    // the next thing to arrive belongs to a different link than whatever is on screen.
    void TermControl::_setHyperlinkPreviewLoading(bool loading)
    {
        if (_filePreviewOperation) { _filePreviewOperation.Cancel(); _filePreviewOperation = nullptr; }
        HyperlinkCardBreadcrumbHost().Children().Clear();
        HyperlinkCardTitleHost().Children().Clear();
        HyperlinkCardStatusHost().Children().Clear();
        HyperlinkCardMetadataHost().Children().Clear();
        HyperlinkCardPreviewName().Text(winrt::hstring{});
        HyperlinkCardPreviewIcon().Content(nullptr);
        HyperlinkCardLeftIcon().Content(nullptr);
        HyperlinkCardFilePreview().Content(nullptr);
        HyperlinkCardPreviewError().Text(winrt::hstring{});
        HyperlinkCardPreviewError().Visibility(Visibility::Collapsed);
        HyperlinkCardPreviewFields().Children().Clear();
        HyperlinkCardPreviewFields().RowDefinitions().Clear();
        HyperlinkCardComments().Children().Clear();
        HyperlinkCardBody().Text(winrt::hstring{});

        // Tabs and actions belong to the result that declared them, so both go with it.
        _currentHyperlinkAction = nullptr;
        _hyperlinkUndoChoiceId = {};
        _hyperlinkSelectedTab = -1;
        HyperlinkCardTabStrip().Children().Clear();
        HyperlinkCardTabStrip().Visibility(Visibility::Collapsed);
        HyperlinkCardActionRow().Visibility(Visibility::Collapsed);
        HyperlinkActionOptions().Items().Clear();
        HyperlinkActionFields().Children().Clear();
        HyperlinkActionFields().Visibility(Visibility::Collapsed);
        HyperlinkActionUndoButton().Visibility(Visibility::Collapsed);
        // A previous action may have relabelled Undo with the far end's own wording.
        // The next link is a different thing entirely, so put the button's own label back.
        if (_hyperlinkUndoDefaultContent)
        {
            HyperlinkActionUndoButton().Content(_hyperlinkUndoDefaultContent);
        }
        HyperlinkActionError().Visibility(Visibility::Collapsed);
        _setHyperlinkActionBusy(false);
        _showHyperlinkTab(-1);
        _hideHyperlinkHtml();

        HyperlinkCardPreviewProgress().IsActive(loading);
        HyperlinkCardPreviewProgress().Visibility(loading ? Visibility::Visible : Visibility::Collapsed);
        HyperlinkCardPreview().Visibility(loading ? Visibility::Visible : Visibility::Collapsed);
    }

    // Fetching a preview is the integration layer's business and can take as long as an HTTP
    // round trip, so it happens off this thread and comes back to it. The generation counter
    // is the whole of the correctness argument: by the time a result arrives the pointer may
    // be over a different link or over nothing, and a stale preview painted into the card
    // would be worse than no preview at all.
    safe_void_coroutine TermControl::_requestHyperlinkPreview(uint32_t generation, winrt::hstring text, winrt::hstring integration)
    {
        // Everything the tail of this function needs is taken now, while we are still on the
        // UI thread and the control is still known to be alive.
        const auto weakThis{ get_weak() };
        const auto provider{ _hyperlinkPreviewProvider };
        const auto dispatcher{ Dispatcher() };
        if (!provider || !dispatcher)
        {
            co_return;
        }

        Control::HyperlinkPreview preview{ nullptr };
        try
        {
            const auto path = integration.empty() ? winrt::hstring{ _resolvedHyperlinkTarget() } : winrt::hstring{};
            if (path.empty())
            {
                preview = co_await provider.GetPreviewAsync(text, integration);
            }
            else
            {
                const auto operation = provider.GetFilePreviewAsync(path);
                _filePreviewOperation = operation;
                preview = co_await operation;
            }
        }
        CATCH_LOG();

        co_await winrt::resume_foreground(dispatcher);

        if (const auto self = weakThis.get(); self && !self->_IsClosing() && self->_hyperlinkPreviewGeneration == generation)
        {
            // A null preview means the fetch failed outright; _applyHyperlinkPreview reads
            // that as "nothing to show" and takes the section back down.
            if (preview) preview.SourceText(text);
            self->_applyHyperlinkPreview(preview);
            if (preview && !preview.FilePath().empty())
            {
                self->HyperlinkCardTarget().Text(winrt::hstring{ L"→ " + std::wstring{ preview.FilePath() } });
                self->HyperlinkCardTarget().Visibility(Visibility::Visible);
                self->HyperlinkOpenButton().Visibility(self->_currentHyperlinkTooltipSettings.showOpen ? Visibility::Visible : Visibility::Collapsed);
                self->HyperlinkCopyLinkButton().Visibility(self->_currentHyperlinkTooltipSettings.showCopyLink ? Visibility::Visible : Visibility::Collapsed);
            }
        }
    }

    void TermControl::_applyHyperlinkPreview(const Control::HyperlinkPreview& preview)
    {
        _currentHyperlinkPreview = preview;

        HyperlinkCardPreviewProgress().IsActive(false);
        HyperlinkCardPreviewProgress().Visibility(Visibility::Collapsed);

        if (!preview)
        {
            HyperlinkCardLeftIcon().Content(nullptr);
            HyperlinkCardPreviewIcon().Content(nullptr);
            HyperlinkCardFilePreview().Content(nullptr);
            // Collapsing the section is not enough for the HTML host: its WebView2 is a
            // child of the top-level window, so it outlives the XAML that reserved its space
            // unless it is told to go away.
            _hideHyperlinkHtml();
            HyperlinkCardPreview().Visibility(Visibility::Collapsed);
            return;
        }

        HyperlinkCardFilePreview().Content(Control::HyperlinkPreviewHelpers::CreateFileView(preview, true));
        HyperlinkCardPreviewName().Text(preview.IntegrationName());
        auto iconStr = preview.IntegrationIcon();
        if (iconStr.empty())
        {
            const auto name = preview.IntegrationName();
            const auto id = _currentHyperlinkTooltipSettings.integration;
            auto containsSub = [](std::wstring_view haystack, std::wstring_view needle) {
                if (needle.empty()) return true;
                auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](wchar_t ch1, wchar_t ch2) {
                    return ::towlower(ch1) == ::towlower(ch2);
                });
                return it != haystack.end();
            };

            if (containsSub(name, L"github") || containsSub(id, L"github"))
            {
                iconStr = L"\uE82D";
            }
            else if (containsSub(name, L"jira") || containsSub(id, L"jira"))
            {
                iconStr = L"\uE943";
            }
            else if (containsSub(name, L"slack") || containsSub(id, L"slack"))
            {
                iconStr = L"\uE8BD";
            }
            else if (containsSub(name, L"stith") || containsSub(id, L"stith"))
            {
                iconStr = L"\uE774";
            }
            // GridView -- panes in a layout, which is what a multiplexer address names.
            else if (containsSub(name, L"shefrd") || containsSub(id, L"shefrd") ||
                     containsSub(name, L"herdr") || containsSub(id, L"herdr"))
            {
                iconStr = L"\uF0E2";
            }
        }

        if (!iconStr.empty())
        {
            HyperlinkCardPreviewIcon().Content(winrt::Microsoft::Terminal::UI::IconPathConverter::IconWUX(iconStr));
            HyperlinkCardLeftIcon().Content(winrt::Microsoft::Terminal::UI::IconPathConverter::IconWUX(iconStr));
        }
        else
        {
            HyperlinkCardPreviewIcon().Content(nullptr);
            HyperlinkCardLeftIcon().Content(nullptr);
        }

        // An integration that could not answer says why, in place of the fields it would
        // otherwise have produced. The card stays usable either way.
        const auto error = preview.Error();
        HyperlinkCardPreviewError().Text(error);
        HyperlinkCardPreviewError().Visibility(error.empty() ? Visibility::Collapsed : Visibility::Visible);

        try
        {
            _fillHyperlinkFields(preview);
            _rebuildHyperlinkTabStrip(preview);
            _rebuildHyperlinkActions(preview);
        }
        CATCH_LOG();
        HyperlinkCardPreview().Visibility(Visibility::Visible);
        _applyIntegrationDisplayMode();

        // An integration can send a rendered HTML representation of its own instead of the
        // generic field list. Hosting one needs Feature_HyperlinkPreviewHtml *and* a
        // WebView2Loader.dll that no build ships yet, so IsAvailable() is false in every
        // Terminal today and this is the field list behaving exactly as it did before.
        // The host is only positioned further down, once _showHyperlinkCard has measured
        // and placed the card whose space it borrows.
        const auto html = preview.Html();
        const auto hostingHtml = !html.empty() && HyperlinkHtmlHost::IsAvailable();
        if (hostingHtml)
        {
            _ensureHyperlinkHtmlHost();
            _hyperlinkHtmlHeight = 120;
            const auto htmlHost = HyperlinkCardHtmlHost();
            htmlHost.MinWidth(240);
            htmlHost.Height(static_cast<double>(_hyperlinkHtmlHeight));
            htmlHost.Visibility(Visibility::Visible);
            HyperlinkCardFieldsScroll().Visibility(Visibility::Collapsed);
        }
        else
        {
            _hideHyperlinkHtml();
        }

        // A text match's link only exists once an integration has resolved it, so the target
        // line and the two buttons that need a link have been waiting for this moment.
        if (_currentHyperlinkTooltipSettings.isTextMatch && !preview.ResolvedUri().empty())
        {
            const auto linkTarget = _hoveredLinkTarget();
            const auto hasLink = !linkTarget.empty();
            if (hasLink)
            {
                // Same arrow the file:// target line above uses, spelled the same way.
                HyperlinkCardTarget().Text(winrt::hstring{ L"\x2192 " + std::wstring{ linkTarget } });
            }
            HyperlinkCardTarget().Visibility(hasLink ? Visibility::Visible : Visibility::Collapsed);
            HyperlinkOpenButton().Visibility(hasLink && _currentHyperlinkTooltipSettings.showOpen ? Visibility::Visible : Visibility::Collapsed);
            HyperlinkCopyLinkButton().Visibility(hasLink && _currentHyperlinkTooltipSettings.showCopyLink ? Visibility::Visible : Visibility::Collapsed);
        }

        // The card just grew by a whole section. Show it again so it is measured and placed
        // for the size it is now, rather than staying where it fitted when it held one line.
        if (HyperlinkCard().Visibility() == Visibility::Visible)
        {
            _showHyperlinkCard();
        }

        // Only now does the placeholder Border have a size and a place on screen, which is
        // what the WebView2 needs to be given. If the card is still inside its show delay
        // there is nothing to position yet -- _showHyperlinkCard moves the host itself when
        // the timer eventually fires.
        if (hostingHtml)
        {
            auto uri = _hoveredLinkTarget();
            if (uri.empty())
            {
                uri = _hoveredUri;
            }

            _hyperlinkHtmlHost->Show(reinterpret_cast<HWND>(OwningHwnd()),
                                     _htmlHostRect(),
                                     html,
                                     preview.DataJson(),
                                     uri,
                                     ActualTheme() == ElementTheme::Dark);
        }
    }

    void TermControl::_ensureHyperlinkHtmlHost()
    {
        if (_hyperlinkHtmlHost)
        {
            return;
        }

        _hyperlinkHtmlHost = std::make_unique<HyperlinkHtmlHost>();

        // WebView2 raises its events on the thread that created the controller, which is
        // this UI thread, so neither callback needs a dispatch of its own -- only the usual
        // check that there is still a control here to talk to.
        _hyperlinkHtmlHost->OnContentHeight = [weakThis = get_weak()](int32_t height) {
            if (const auto self = weakThis.get(); self && !self->_IsClosing())
            {
                self->_setHyperlinkHtmlHeight(height);
            }
        };
        _hyperlinkHtmlHost->OnOpenLink = [weakThis = get_weak()](winrt::hstring uri) {
            if (const auto self = weakThis.get(); self && !self->_IsClosing())
            {
                // Down the same path the card's own Open button takes, so the unsafe-URL
                // prompt and the file:// resolution in TerminalPage still apply. A page
                // inside the card gets no rules of its own.
                self->_hideHyperlinkCard();
                self->OpenHyperlink.raise(*self, winrt::make<OpenHyperlinkEventArgs>(uri));
            }
        };
    }

    // Takes the HTML host back down and gives the field list its place back. All of it is
    // behind the null check: nothing creates a host unless a preview actually asked for one
    // and IsAvailable() agreed, so in every build today this is a pointer test and no more.
    void TermControl::_hideHyperlinkHtml()
    {
        if (!_hyperlinkHtmlHost)
        {
            return;
        }

        _hyperlinkHtmlHost->Hide();
        HyperlinkCardHtmlHost().Visibility(Visibility::Collapsed);
        HyperlinkCardFieldsScroll().Visibility(_hyperlinkSelectedTab < 0 ? Visibility::Visible : Visibility::Collapsed);
    }

    // The page measured itself and said how tall it wants to be. Clamped, because a card is
    // a hover affordance: a plugin that asks for a thousand pixels does not get the pane.
    void TermControl::_setHyperlinkHtmlHeight(int32_t height)
    {
        const auto clamped = std::clamp(height, 40, 320);
        if (clamped == _hyperlinkHtmlHeight)
        {
            return;
        }

        _hyperlinkHtmlHeight = clamped;
        HyperlinkCardHtmlHost().Height(static_cast<double>(clamped));

        // The card just changed size, so it has to be measured and placed again -- which is
        // also what moves the WebView2 to follow it.
        if (HyperlinkCard().Visibility() == Visibility::Visible)
        {
            _showHyperlinkCard();
        }
    }

    // Where the HTML host's WebView2 goes: the placeholder Border's rectangle, expressed in
    // the *owning window's client pixels*, because that is the only coordinate space a
    // WebView2 controller's bounds are in.
    //
    // Two approximations live here, both deliberate and both harmless while the feature is
    // off. The XAML island's origin within the owner's client area is taken as (0, 0) --
    // exact for a plain window, out by the non-client inset on one with a custom title bar.
    // And ActualWidth/ActualHeight are zero until a layout pass has run, so on the very
    // first show the card's own measured width less its padding stands in. Both are
    // corrected by the next Move(), which every _showHyperlinkCard issues.
    RECT TermControl::_htmlHostRect()
    {
        const auto host = HyperlinkCardHtmlHost();

        auto scale = 1.0;
        try
        {
            if (const auto root = XamlRoot())
            {
                scale = root.RasterizationScale();
            }
            else
            {
                scale = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
            }
        }
        CATCH_LOG();

        auto width = host.ActualWidth();
        if (width <= 0)
        {
            width = std::max(0.0, static_cast<double>(HyperlinkCard().DesiredSize().Width) - 20.0);
        }
        auto height = host.ActualHeight();
        if (height <= 0)
        {
            height = static_cast<double>(_hyperlinkHtmlHeight);
        }

        const auto origin = host.TransformToVisual(nullptr).TransformPoint({});
        const auto left = static_cast<double>(origin.X) * scale;
        const auto top = static_cast<double>(origin.Y) * scale;

        return RECT{
            std::lround(left),
            std::lround(top),
            std::lround(left + width * scale),
            std::lround(top + height * scale),
        };
    }

    void TermControl::_showHyperlinkCard()
    {
        const auto cell = _core.HoveredCell();
        if (_hoveredUri.empty() || !cell)
        {
            return;
        }

        const auto card = HyperlinkCard();
        const auto swapChain = SwapChainPanel();
        const auto viewportWidth = swapChain ? swapChain.ActualWidth() : ActualWidth();
        const auto viewportHeight = swapChain ? swapChain.ActualHeight() : ActualHeight();
        if (viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        // Cap at the viewport itself, not just the configured setting -- a pane
        // narrower than hyperlink.tooltipMaxWidth (or with wrapping disabled,
        // where the setting is +inf) must still be able to fit the card
        // entirely inside it, or the position clamp below has nothing left to
        // clamp to and the card overflows the pane regardless of where it sits.
        const auto configuredMaxWidth = static_cast<double>(_currentHyperlinkTooltipSettings.maxWidth);
        card.MaxWidth(configuredMaxWidth > 0 ? std::min(configuredMaxWidth, viewportWidth) : viewportWidth);
        card.MinWidth(std::min(200.0, viewportWidth));
        const auto maxHeight = _core.Settings().HyperlinkTooltipMaxHeight();
        const auto heightLimit = maxHeight > 0 ? std::min(static_cast<double>(maxHeight), viewportHeight) : viewportHeight;
        card.MaxHeight(heightLimit);
        HyperlinkCardScroll().MaxHeight(std::max(1.0, heightLimit - 20));

        // A rule's custom actions, if any, alongside the (possibly individually
        // suppressed) built-in buttons above -- see _hoveredHyperlinkChanged.
        //
        // ItemsSource set from code has to be something XAML can enumerate at runtime:
        // IBindableVector, which C++/WinRT's observable vector of IInspectable provides
        // (Windows.UI.Xaml.Interop.h is in the pch). A plain single_threaded_vector<T>
        // of a projected class is not, and XAML rejected it with an exception that
        // surfaced as a fail-fast on every hover -- with or without URL detection,
        // since an OSC 8 link takes exactly the same path.
        try
        {
            auto actions = winrt::single_threaded_observable_vector<IInspectable>();
            for (const auto& action : _currentHyperlinkTooltipSettings.customActions)
            {
                actions.Append(action);
            }
            HyperlinkCardCustomActions().ItemsSource(actions);
        }
        CATCH_LOG();

        // Must be visible before measuring, because XAML skips layout on Collapsed elements
        // and returns {0, 0} for DesiredSize.
        card.Visibility(Visibility::Visible);

        // Measure first: where it goes depends on how big it turned out to be, and it has
        // just been given new text.
        card.Measure({ gsl::narrow_cast<float>(viewportWidth), gsl::narrow_cast<float>(viewportHeight) });
        const auto desired = card.DesiredSize();
        const auto desiredWidth = static_cast<double>(desired.Width);
        const auto desiredHeight = static_cast<double>(desired.Height);

        const auto offset = SwapChainPanel().ActualOffset();
        const auto fontSize = CharacterDimensions();
        const auto pos = _toPosInDips(cell.Value());
        const auto cellLeft = static_cast<double>(pos.X) - offset.x;
        const auto cellTop = static_cast<double>(pos.Y) - offset.y;

        // Under the line by default, flipped above it when there is no room below.
        auto top = cellTop + fontSize.Height;
        auto aboveLine = false;
        if (top + desiredHeight > viewportHeight)
        {
            top = cellTop - desiredHeight;
            aboveLine = true;
        }

        // Which edge is nearest the link is only known now, so the button row may
        // have to change ends. Safe to do after the card was measured: swapping the
        // row between the two hosts moves the same content from one end to the other
        // and cannot change the height the flip decision above was made from.
        if (aboveLine != _hyperlinkCardAboveLine)
        {
            _hyperlinkCardAboveLine = aboveLine;
            _applyActionPlacement();
        }

        // Right of the cursor by default, flipped to grow left of it instead when
        // there is no room that way -- a long URI near the right edge would
        // otherwise still spill past it even after the left edge was clamped in.
        auto left = cellLeft;
        if (left + desiredWidth > viewportWidth)
        {
            left = cellLeft + fontSize.Width - desiredWidth;
        }

        // Final safety net on both axes either way: never let an edge land
        // outside the control's own bounds, however the flip above worked out.
        left = std::clamp(left, 0.0, std::max(0.0, viewportWidth - desiredWidth));
        top = std::clamp(top, 0.0, std::max(0.0, viewportHeight - desiredHeight));

        Controls::Canvas::SetLeft(card, left);
        Controls::Canvas::SetTop(card, top);

        // The HTML host's WebView2 is a child of the top-level window rather than part of
        // the card's visual tree, so it does not travel with the card -- it has to be told,
        // every single time the card is placed.
        if (_hyperlinkHtmlHost && HyperlinkCardHtmlHost().Visibility() == Visibility::Visible)
        {
            _hyperlinkHtmlHost->Move(_htmlHostRect());
        }
    }

    void TermControl::_hideHyperlinkCard()
    {
        if (_filePreviewOperation) { _filePreviewOperation.Cancel(); _filePreviewOperation = nullptr; }
        _hyperlinkShowTimer.Stop();
        _hyperlinkHideTimer.Stop();
        _pointerInHyperlinkCard = false;
        _hyperlinkActionDropDownOpen = false;
        // A preview still in flight belongs to a hover that is over. Moving the generation on
        // is what sends its result to the bin instead of into a card shown for something else.
        ++_hyperlinkPreviewGeneration;
        _currentHyperlinkPreview = nullptr;
        HyperlinkCardFilePreview().Content(nullptr);
        HyperlinkCardLeftIcon().Content(nullptr);
        HyperlinkCardPreviewIcon().Content(nullptr);
        _hideHyperlinkHtml();
        HyperlinkCard().Visibility(Visibility::Collapsed);
    }

    void TermControl::_scheduleHyperlinkCardHide()
    {
        _hyperlinkShowTimer.Stop();

        if (_pointerInHyperlinkCard || _hyperlinkActionDropDownOpen || HyperlinkCard().Visibility() != Visibility::Visible)
        {
            return;
        }

        const auto delay = Control::HyperlinkPreviewHelpers::HasNestedPreview() ? std::max(150, _currentHyperlinkTooltipSettings.hideDelay) : _currentHyperlinkTooltipSettings.hideDelay;
        if (delay == 0)
        {
            _hideHyperlinkCard();
            return;
        }

        if (!_hyperlinkHideTimer)
        {
            _hyperlinkHideTimer.Tick([weakThis = get_weak()](auto&&, auto&&) {
                if (const auto self = weakThis.get(); self && !self->_IsClosing())
                {
                    if (Control::HyperlinkPreviewHelpers::HasNestedPreview()) return;
                    self->_hyperlinkHideTimer.Stop();
                    // Re-check: the pointer may have arrived while the timer was running.
                    if (!self->_pointerInHyperlinkCard && !Control::HyperlinkPreviewHelpers::HasNestedPreview())
                    {
                        self->_hideHyperlinkCard();
                    }
                }
            });
        }
        _hyperlinkHideTimer.Stop();
        _hyperlinkHideTimer.Interval(std::chrono::milliseconds{ delay });
        _hyperlinkHideTimer.Start();
    }

    void TermControl::_HyperlinkCardPointerEntered(const IInspectable& /*sender*/, const PointerRoutedEventArgs& /*e*/)
    {
        _pointerInHyperlinkCard = true;
        _hyperlinkHideTimer.Stop();
    }

    void TermControl::_HyperlinkCardPointerExited(const IInspectable& /*sender*/, const PointerRoutedEventArgs& /*e*/)
    {
        _pointerInHyperlinkCard = false;
        _scheduleHyperlinkCardHide();
    }

    // Swallow both halves of the click so neither reaches the terminal underneath: without
    // this, pressing a button also starts a selection in the buffer behind it. The search
    // box does the same thing for the same reason.
    void TermControl::_HyperlinkCardPointerPressed(const IInspectable& /*sender*/, const PointerRoutedEventArgs& e)
    {
        e.Handled(true);
    }

    void TermControl::_HyperlinkCardPointerReleased(const IInspectable& /*sender*/, const PointerRoutedEventArgs& e)
    {
        e.Handled(true);
    }

    void TermControl::_HyperlinkCardSizeChanged(const Windows::Foundation::IInspectable& /*sender*/, const Windows::UI::Xaml::SizeChangedEventArgs& e)
    {
        const auto card = HyperlinkCard();
        if (card.Visibility() != Visibility::Visible)
        {
            return;
        }

        const auto swapChain = SwapChainPanel();
        const auto viewportWidth = swapChain ? swapChain.ActualWidth() : ActualWidth();
        const auto viewportHeight = swapChain ? swapChain.ActualHeight() : ActualHeight();
        if (viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        const auto newSize = e.NewSize();
        const auto newWidth = static_cast<double>(newSize.Width);
        const auto newHeight = static_cast<double>(newSize.Height);
        const auto currentLeft = Controls::Canvas::GetLeft(card);
        const auto currentTop = Controls::Canvas::GetTop(card);

        const auto clampedLeft = std::clamp(currentLeft, 0.0, std::max(0.0, viewportWidth - newWidth));
        const auto clampedTop = std::clamp(currentTop, 0.0, std::max(0.0, viewportHeight - newHeight));

        if (clampedLeft != currentLeft)
        {
            Controls::Canvas::SetLeft(card, clampedLeft);
        }
        if (clampedTop != currentTop)
        {
            Controls::Canvas::SetTop(card, clampedTop);
        }

        if (_hyperlinkHtmlHost && HyperlinkCardHtmlHost().Visibility() == Visibility::Visible)
        {
            _hyperlinkHtmlHost->Move(_htmlHostRect());
        }
    }

    // The "<chord> to follow link" line under the card. The two chords that have
    // always had their own fully localized string keep using it -- a plain click and
    // Ctrl+click are far and away the common cases, and every locale already carries
    // both. Anything else composes the modifier and gesture names into a format
    // string, which only en-US has; other locales fall back to it rather than losing
    // the two wordings they do have.
    winrt::hstring TermControl::_followLinkHintText(const IControlSettings& settings) const
    {
        const auto modifier = settings.HyperlinkPrimaryClickModifier();
        const auto gesture = settings.HyperlinkPrimaryClickGesture();

        if (gesture == HyperlinkClickGesture::LeftClick)
        {
            if (modifier == HyperlinkClickModifier::None)
            {
                return RS_(L"HowToOpenRunSingleClick");
            }
            if (modifier == HyperlinkClickModifier::Ctrl)
            {
                return _followLinkHintCtrlClick;
            }
        }

        const auto modifierName = [&]() -> std::wstring_view {
            switch (modifier)
            {
            case HyperlinkClickModifier::Ctrl:
                return L"Ctrl+";
            case HyperlinkClickModifier::Alt:
                return L"Alt+";
            case HyperlinkClickModifier::Shift:
                return L"Shift+";
            case HyperlinkClickModifier::Win:
                return L"Win+";
            case HyperlinkClickModifier::CtrlAlt:
                return L"Ctrl+Alt+";
            case HyperlinkClickModifier::CtrlShift:
                return L"Ctrl+Shift+";
            case HyperlinkClickModifier::AltShift:
                return L"Alt+Shift+";
            case HyperlinkClickModifier::CtrlAltShift:
                return L"Ctrl+Alt+Shift+";
            default:
                return L"";
            }
        }();

        const auto gestureName = gesture == HyperlinkClickGesture::MiddleClick ? RS_(L"HowToOpenGestureMiddleClick") :
                                 gesture == HyperlinkClickGesture::DoubleClick ? RS_(L"HowToOpenGestureDoubleClick") :
                                                                                 RS_(L"HowToOpenGestureClick");

        return winrt::hstring{ RS_fmt(L"HowToOpenRunFormat", modifierName, std::wstring_view{ gestureName }) };
    }

    // Runs one of the built-in action ids -- "open", "copyLink", "copyPath",
    // "reveal", "showInPane" -- or hands anything else to TerminalPage as an entry
    // under "actions". Both the card's buttons and the two click chords come
    // through here, so binding a chord to a button's id does exactly what the
    // button does. "" and "none" are no-ops: the former is an unresolved inherit,
    // the latter a rule saying this link has no such click.
    void TermControl::_invokeHyperlinkActionById(const winrt::hstring& actionId)
    {
        if (actionId.empty() || actionId == L"none")
        {
            return;
        }

        if (actionId == L"open")
        {
            // The link, not the hovered run: for a text match the two differ, and it is
            // the link the user means. Down the same path a click takes, so the
            // unsafe-URL prompt and the file:// resolution in TerminalPage still apply.
            if (const auto target = _hoveredLinkTarget(); !target.empty())
            {
                OpenHyperlink.raise(*this, winrt::make<OpenHyperlinkEventArgs>(target));
            }
            return;
        }
        if (actionId == L"copyLink")
        {
            if (const auto target = _hoveredLinkTarget(); !target.empty())
            {
                _core.CopyTextToClipboard(target);
            }
            return;
        }
        if (actionId == L"copyPath")
        {
            if (const auto target = _resolvedHyperlinkTarget(); !target.empty())
            {
                _core.CopyTextToClipboard(winrt::hstring{ target });
            }
            return;
        }
        if (actionId == L"reveal")
        {
            if (const auto target = _resolvedHyperlinkTarget(); !target.empty())
            {
                // /select, so the file itself comes up selected rather than just its folder.
                const auto args = fmt::format(LR"(/select,"{}")", target);
                ShellExecuteW(nullptr, nullptr, L"explorer", args.c_str(), nullptr, SW_SHOW);
            }
            return;
        }
        if (actionId == L"showInPane")
        {
            _raiseShowHyperlinkPreviewRequested();
            return;
        }

        // An entry under "actions"; the dispatch, including the %u URI
        // substitution, happens up in TerminalPage.
        if (!_hoveredUri.empty())
        {
            HyperlinkTooltipActionInvoked.raise(*this, winrt::make<HyperlinkTooltipActionInvokedEventArgs>(actionId, _hoveredUri));
        }
    }

    void TermControl::_HyperlinkOpenClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        _invokeHyperlinkActionById(L"open");
        _hideHyperlinkCard();
    }

    void TermControl::_HyperlinkCopyLinkClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        _invokeHyperlinkActionById(L"copyLink");
        _hideHyperlinkCard();
    }

    void TermControl::_HyperlinkCopyPathClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        _invokeHyperlinkActionById(L"copyPath");
        _hideHyperlinkCard();
    }

    void TermControl::_HyperlinkRevealClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        _invokeHyperlinkActionById(L"reveal");
        _hideHyperlinkCard();
    }

    // Shared Click handler for every button in HyperlinkCardCustomActions -- the button's
    // Tag carries the action id (bound from HyperlinkTooltipAction.ActionId), and the
    // actual dispatch, including the %u URI substitution, happens up in TerminalPage.
    void TermControl::_HyperlinkCustomActionClick(const IInspectable& sender, const RoutedEventArgs& /*e*/)
    {
        if (const auto button = sender.try_as<Controls::Button>())
        {
            if (const auto actionId = button.Tag().try_as<hstring>(); actionId && !actionId->empty() && !_hoveredUri.empty())
            {
                HyperlinkTooltipActionInvoked.raise(*this, winrt::make<HyperlinkTooltipActionInvokedEventArgs>(*actionId, _hoveredUri));
            }
        }
        _hideHyperlinkCard();
    }

    void TermControl::_HyperlinkCommitCopyShaClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        if (!_currentCommitFullSha.empty())
        {
            _core.CopyTextToClipboard(winrt::hstring{ _currentCommitFullSha });
        }
    }

    void TermControl::_formatHighlightedUri(const winrt::hstring& uriText)
    {
        const auto inlines = HoveredUri().Inlines();
        inlines.Clear();

        if (uriText.empty())
        {
            return;
        }

        const std::wstring uriStr{ uriText };

        auto appendRun = [&](std::wstring_view text, bool bold) {
            if (text.empty())
            {
                return;
            }
            Windows::UI::Xaml::Documents::Run run;
            run.Text(winrt::hstring{ text });
            if (bold)
            {
                run.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            }
            inlines.Append(run);
        };

        // 1. GitHub PR/issue: https://github.com/{owner}/{repo}/(pull|issues)/{number}
        static const std::wregex ghPrRe{ LR"(^(https?://github\.com/)([^/]+)/([^/]+)/((?:pull|issues)/)(\d+)(.*)$)", std::regex_constants::ECMAScript | std::regex_constants::icase };
        // 2. GitHub commit: https://github.com/{owner}/{repo}/commit/{sha}
        static const std::wregex ghCommitRe{ LR"(^(https?://github\.com/)([^/]+)/([^/]+)/(commit/)([0-9a-fA-F]+)(.*)$)", std::regex_constants::ECMAScript | std::regex_constants::icase };
        // 3. GitHub repo: https://github.com/{owner}/{repo}
        static const std::wregex ghRepoRe{ LR"(^(https?://github\.com/)([^/]+)/([^/?#]+)(.*)$)", std::regex_constants::ECMAScript | std::regex_constants::icase };
        // 4. repo#number: {repo}#{number}
        static const std::wregex repoNumRe{ LR"(^([A-Za-z0-9_.-]+)(#)(\d+)$)", std::regex_constants::ECMAScript };
        // 5. Jira browse: .../browse/{KEY-123}
        static const std::wregex jiraRe{ LR"(^(.*?/browse/)([A-Z][A-Z0-9]+-\d+)(.*)$)", std::regex_constants::ECMAScript };
        // 6. Bare Jira key: {KEY-123}
        static const std::wregex bareJiraRe{ LR"(^([A-Z][A-Z0-9]+-\d+)$)", std::regex_constants::ECMAScript };

        std::wsmatch match;
        if (std::regex_match(uriStr, match, ghPrRe))
        {
            appendRun(match[1].str(), false);
            appendRun(match[2].str(), true);
            appendRun(L"/", false);
            appendRun(match[3].str(), true);
            appendRun(L"/" + match[4].str(), false);
            appendRun(match[5].str(), true);
            appendRun(match[6].str(), false);
            return;
        }

        if (std::regex_match(uriStr, match, ghCommitRe))
        {
            appendRun(match[1].str(), false);
            appendRun(match[2].str(), true);
            appendRun(L"/", false);
            appendRun(match[3].str(), true);
            appendRun(L"/" + match[4].str(), false);
            appendRun(match[5].str(), true);
            appendRun(match[6].str(), false);
            return;
        }

        if (std::regex_match(uriStr, match, ghRepoRe))
        {
            appendRun(match[1].str(), false);
            appendRun(match[2].str(), true);
            appendRun(L"/", false);
            appendRun(match[3].str(), true);
            appendRun(match[4].str(), false);
            return;
        }

        if (std::regex_match(uriStr, match, repoNumRe))
        {
            appendRun(match[1].str(), true);
            appendRun(match[2].str(), false);
            appendRun(match[3].str(), true);
            return;
        }

        if (std::regex_match(uriStr, match, jiraRe))
        {
            appendRun(match[1].str(), false);
            appendRun(match[2].str(), true);
            appendRun(match[3].str(), false);
            return;
        }

        if (std::regex_match(uriStr, match, bareJiraRe))
        {
            appendRun(match[1].str(), true);
            return;
        }

        appendRun(uriStr, false);
    }

    // Moves the button row, and the matched-rule line, into whichever of the card's
    // two hosts the settings ask for.
    //
    // The setting names an edge relative to the *link*, not to the card, because the
    // card is not always the same way up: _showHyperlinkCard puts it under the line
    // and flips it above when there is no room below. Under the line, the card's top
    // is the edge next to the link; above the line, its bottom is. So the host to use
    // is the exclusive-or of "the user asked for near" and "the card is above the
    // line" -- which is the whole reason this cannot be decided once from the setting
    // alone, and why _showHyperlinkCard calls this again after it has placed the card.
    //
    // The rule line is always on the far edge, right-aligned, so it never sits between
    // the pointer and the buttons. When the buttons are on the far edge too they share
    // that row, which is what the two-column hosts are for.
    void TermControl::_applyActionPlacement()
    {
        const auto headerHost = HyperlinkCardHeaderActionsHost();
        const auto footerHost = HyperlinkCardFooterActionsHost();
        const auto actions = HyperlinkCardActions();
        const auto ruleInfo = HyperlinkCardRuleInfo();
        if (!headerHost || !footerHost || !actions)
        {
            return;
        }

        const auto wantNearLink = _currentHyperlinkTooltipSettings.actionPlacement == Control::HyperlinkActionPlacement::NearLink;
        const auto actionsInHeader = wantNearLink != _hyperlinkCardAboveLine;
        // The far edge is the top when the card sits above the link, the bottom when
        // it sits below it -- i.e. exactly the header when the card is above.
        const auto ruleInHeader = _hyperlinkCardAboveLine;

        const auto reparent = [](const Controls::Grid& target, const Controls::Grid& other, const UIElement& child) {
            if (!child)
            {
                return;
            }
            uint32_t index = 0;
            if (other.Children().IndexOf(child, index))
            {
                other.Children().RemoveAt(index);
            }
            if (!target.Children().IndexOf(child, index))
            {
                target.Children().Append(child);
            }
        };

        reparent(actionsInHeader ? headerHost : footerHost, actionsInHeader ? footerHost : headerHost, actions);
        reparent(ruleInHeader ? headerHost : footerHost, ruleInHeader ? footerHost : headerHost, ruleInfo);

        // A host earns its space by holding something that is actually on screen.
        // The button row can be switched off wholesale and the rule line is off by
        // default, so either host may end up empty.
        const auto actionsShown = actions.Visibility() == Visibility::Visible;
        const auto ruleShown = ruleInfo && ruleInfo.Visibility() == Visibility::Visible;
        const auto headerUsed = (actionsInHeader && actionsShown) || (ruleInHeader && ruleShown);
        const auto footerUsed = (!actionsInHeader && actionsShown) || (!ruleInHeader && ruleShown);
        headerHost.Visibility(headerUsed ? Visibility::Visible : Visibility::Collapsed);
        footerHost.Visibility(footerUsed ? Visibility::Visible : Visibility::Collapsed);
    }

    // Names the rule that decided how this card looks, and -- just as usefully --
    // says when no rule did. A link that is detected but matches nothing gets the
    // global defaults, and "why is this not previewing the way I set it up" is
    // answered by seeing that stated rather than by guessing.
    void TermControl::_applyRuleInfo()
    {
        const auto ruleInfo = HyperlinkCardRuleInfo();
        const auto ruleText = HyperlinkCardRuleInfoText();
        if (!ruleInfo || !ruleText)
        {
            return;
        }

        if (!_currentHyperlinkTooltipSettings.showRule)
        {
            ruleInfo.Visibility(Visibility::Collapsed);
            return;
        }

        const auto matched = _currentHyperlinkTooltipSettings.ruleIndex >= 0;
        if (matched)
        {
            const auto& name = _currentHyperlinkTooltipSettings.ruleName;
            const auto displayName = name.empty() ? RS_(L"HyperlinkRuleUnnamed") : name;
            ruleText.Text(winrt::hstring{ fmt::format(FMT_COMPILE(L"{} {}"),
                                                      std::wstring_view{ RS_(L"HyperlinkRuleMatchedPrefix") },
                                                      std::wstring_view{ displayName }) });
        }
        else
        {
            ruleText.Text(RS_(L"HyperlinkRuleNoMatch"));
        }
        ruleInfo.Visibility(Visibility::Visible);
    }

    void TermControl::_applyIntegrationDisplayMode()
    {
        const auto mode = _currentHyperlinkTooltipSettings.integrationDisplayMode;
        const auto leftIcon = HyperlinkCardLeftIcon();
        const auto previewHeader = HyperlinkCardPreviewHeader();
        if (!leftIcon || !previewHeader)
        {
            return;
        }

        const auto isPreviewVisible = HyperlinkCardPreview().Visibility() == Visibility::Visible;
        const auto hasIcon = leftIcon.Content() != nullptr || HyperlinkCardPreviewIcon().Content() != nullptr;

        if (!isPreviewVisible || !hasIcon || mode == Control::HyperlinkIntegrationDisplayMode::None)
        {
            leftIcon.Visibility(Visibility::Collapsed);
            previewHeader.Visibility(Visibility::Collapsed);
        }
        else if (mode == Control::HyperlinkIntegrationDisplayMode::Above)
        {
            leftIcon.Visibility(Visibility::Collapsed);
            previewHeader.Visibility(Visibility::Visible);
        }
        else if (mode == Control::HyperlinkIntegrationDisplayMode::Left)
        {
            leftIcon.Visibility(Visibility::Visible);
            previewHeader.Visibility(Visibility::Collapsed);
        }
    }

    // Ask the app to put this link's preview in a pane. The card is not the place
    // for the answer, so it goes away; the integration hint travels with the request
    // so the pane resolves the same link with the same integration the card used.
    void TermControl::_raiseShowHyperlinkPreviewRequested(bool hover, bool createPane)
    {
        if (_hoveredUri.empty())
        {
            return;
        }

        auto args = winrt::make<ShowHyperlinkPreviewRequestedEventArgs>(_hoveredUri,
            _currentHyperlinkTooltipSettings.showPreview ? _currentHyperlinkTooltipSettings.integration : winrt::hstring{ L"none" },
            winrt::hstring{ _resolvedHyperlinkTarget() });
        args.IsHover(hover);
        args.CreatePane(createPane);
        ShowHyperlinkPreviewRequested.raise(*this, args);
    }

    void TermControl::_HyperlinkShowInPaneClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        _invokeHyperlinkActionById(L"showInPane");
        _hideHyperlinkCard();
    }

    // The rule line, clicked. Hand the app the rule to open; it owns the settings
    // UI and we do not. The card goes away first: the settings are about to take
    // the foreground, and a tooltip left hanging over them looks like a bug.
    // Read before hiding, since _hideHyperlinkCard is free to reset the hover state.
    void TermControl::_HyperlinkRuleInfoClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        const auto ruleIndex = _currentHyperlinkTooltipSettings.ruleIndex;
        const auto ruleName = _currentHyperlinkTooltipSettings.ruleName;
        _hideHyperlinkCard();
        EditHyperlinkRuleRequested.raise(*this, winrt::make<EditHyperlinkRuleRequestedEventArgs>(ruleIndex, ruleName));
    }

    // The pane's "pane only" switch. Nothing is torn down: the card simply stops
    // being shown, and starts again the moment this is cleared.
    void TermControl::HyperlinkTooltipsSuppressed(bool suppressed)
    {
        _hyperlinkTooltipsSuppressed = suppressed;
        if (suppressed)
        {
            _hideHyperlinkCard();
        }
    }

    // One Grid row per field. This is built in code rather than by an ItemsControl
    // with a DataTemplate because the labels have to line up with each other: a
    // single Grid gives every row the same Auto-sized first column, and UWP XAML has
    // no SharedSizeGroup with which to reach that across separate per-row Grids.
    void TermControl::_fillHyperlinkFields(const Control::HyperlinkPreview& preview)
    {
        HyperlinkCardBreadcrumbHost().Children().Clear();
        HyperlinkCardTitleHost().Children().Clear();
        HyperlinkCardStatusHost().Children().Clear();
        HyperlinkCardMetadataHost().Children().Clear();
        const auto grid = HyperlinkCardPreviewFields();
        grid.Children().Clear();
        grid.RowDefinitions().Clear();

        const auto fields = preview ? preview.Fields() : nullptr;
        if (!fields)
        {
            HyperlinkCardCommitPanel().Visibility(Visibility::Collapsed);
            HyperlinkCardFieldsScroll().Visibility(Visibility::Collapsed);
            return;
        }

        bool isCommit = false;
        std::wstring commitTitle;
        std::wstring commitBody;
        std::wstring commitSha;
        std::wstring commitParent;
        int32_t commitAdditions = 0;
        int32_t commitDeletions = 0;
        int32_t commitFilesCount = 0;

        for (const auto& f : fields)
        {
            if (!f)
            {
                continue;
            }
            // Field labels are presentation, not identity: Jira also has a Parent.
            // Only GitHub's explicit commit group selects this specialized view.
            if (::Microsoft::Terminal::PreviewPresentation::IsCommitGroup(preview.IntegrationId(), f.Group())) isCommit = true;
            const auto label = f.Label();
            const auto val = f.Value();
            if (f.IsTitle() && (label == L"Message" || label.empty()))
            {
                const std::wstring text{ val };
                const auto newline = text.find(L"\n");
                if (newline != std::wstring::npos)
                {
                    commitTitle = text.substr(0, newline);
                    auto body = text.substr(newline + 1);
                    while (!body.empty() && (body.front() == L'\r' || body.front() == L'\n'))
                    {
                        body.erase(0, 1);
                    }
                    commitBody = body;
                }
                else
                {
                    commitTitle = text;
                }
            }
            else if (label == L"Commit")
            {
                commitSha = val;
            }
            else if (label == L"Parent")
            {
                commitParent = val;
            }
            else if (label == L"Added")
            {
                commitAdditions = _wtoi(val.c_str());
            }
            else if (label == L"Removed")
            {
                commitDeletions = _wtoi(val.c_str());
            }
            else if (label == L"Files changed")
            {
                commitFilesCount = _wtoi(val.c_str());
            }
        }

        if (isCommit)
        {
            HyperlinkCardCommitPanel().Visibility(Visibility::Visible);
            HyperlinkCardFieldsScroll().Visibility(Visibility::Collapsed);

            HyperlinkCommitTitle().Text(winrt::hstring{ commitTitle.empty() ? commitSha : commitTitle });
            if (!commitBody.empty())
            {
                HyperlinkCommitBody().Text(winrt::hstring{ commitBody });
                HyperlinkCommitBody().Visibility(Visibility::Visible);
            }
            else
            {
                HyperlinkCommitBody().Visibility(Visibility::Collapsed);
            }

            // A commit need not belong to main, or even to a single branch.
            HyperlinkCommitBranch().Visibility(Visibility::Collapsed);

            if (!commitParent.empty())
            {
                const auto shortParent = commitParent.size() >= 7 ? commitParent.substr(0, 7) : commitParent;
                HyperlinkCommitParents().Text(winrt::hstring{ shortParent + L" parent" });
            }
            else
            {
                HyperlinkCommitParents().Text(L"");
            }

            _currentCommitFullSha = commitSha;
            const auto shortSha = commitSha.size() >= 7 ? commitSha.substr(0, 7) : commitSha;
            HyperlinkCommitSha().Text(winrt::hstring{ shortSha });

            const auto fileCount = commitFilesCount;
            HyperlinkCommitFilesChanged().Text(winrt::hstring{ fmt::format(L"{} changed file{}", fileCount, (fileCount == 1 ? L"" : L"s")) });
            HyperlinkCommitStatsText().Text(winrt::hstring{ fmt::format(L"+{} -{}", commitAdditions, commitDeletions) });

            const auto squares = HyperlinkCommitDiffSquares();
            squares.Children().Clear();
            const auto total = commitAdditions + commitDeletions;
            int greenCount = 0;
            int redCount = 0;
            if (total > 0)
            {
                greenCount = std::clamp(static_cast<int>(std::round(5.0 * commitAdditions / total)), 0, 5);
                redCount = std::clamp(static_cast<int>(std::round(5.0 * commitDeletions / total)), 0, 5 - greenCount);
            }
            const int grayCount = 5 - greenCount - redCount;

            auto addSquare = [&](Windows::UI::Color color) {
                Controls::Border square;
                square.Width(8);
                square.Height(8);
                square.Margin(Thickness{ 1, 0, 1, 0 });
                square.CornerRadius(Windows::UI::Xaml::CornerRadius{ 1, 1, 1, 1 });
                square.Background(Media::SolidColorBrush{ color });
                squares.Children().Append(square);
            };

            for (int i = 0; i < greenCount; ++i)
            {
                addSquare(Windows::UI::ColorHelper::FromArgb(255, 45, 164, 78));
            }
            for (int i = 0; i < redCount; ++i)
            {
                addSquare(Windows::UI::ColorHelper::FromArgb(255, 207, 34, 46));
            }
            for (int i = 0; i < grayCount; ++i)
            {
                addSquare(Windows::UI::ColorHelper::FromArgb(255, 208, 215, 222));
            }

            return;
        }

        HyperlinkCardCommitPanel().Visibility(Visibility::Collapsed);
        HyperlinkCardFieldsScroll().Visibility(Visibility::Visible);

        auto row = 0;
        for (const auto& field : fields)
        {
            // Keep zero counts in the full pane, but omit them from the hover card.
            if (!field || (!field.IsTitle() && ::Microsoft::Terminal::PreviewPresentation::IsZeroCount(field.Value())))
            {
                continue;
            }

            Controls::RowDefinition rowDefinition;
            rowDefinition.Height(GridLength{ 0, GridUnitType::Auto });
            grid.RowDefinitions().Append(rowDefinition);

            if (field.IsTitle())
            {
                // A title is the subject of the card, not a labelled row, so it spans
                // both columns and wraps inside whatever width the card ended up with.
                Controls::TextBlock title;
                title.Text(field.Value());
                title.FontWeight(Windows::UI::Text::FontWeight{ 600 });
                title.TextWrapping(TextWrapping::Wrap);
                title.MaxLines(3);
                title.IsTextSelectionEnabled(true);
                Controls::Grid::SetRow(title, row);
                Controls::Grid::SetColumn(title, 0);
                Controls::Grid::SetColumnSpan(title, 2);
                HyperlinkCardTitleHost().Children().Append(title);
                ++row;
                continue;
            }

            const auto placement = field.Placement();
            if (placement == L"status" || placement == L"header" || placement == L"breadcrumb")
            {
                Controls::StackPanel item;
                item.Orientation(Controls::Orientation::Horizontal);
                item.Spacing(4);
                if (field.HasIcon())
                {
                    Controls::Image icon;
                    icon.Width(16); icon.Height(16);
                    icon.VerticalAlignment(VerticalAlignment::Center);
                    icon.Source(HyperlinkPreviewHelpers::ImageFromUri(field.IconUri()));
                    item.Children().Append(icon);
                }
                if (!field.LinkUri().empty())
                {
                    Controls::HyperlinkButton link;
                    link.Content(box_value(field.Value()));
                    link.Padding(Thickness{ 0, 0, 0, 0 });
                    try { link.NavigateUri(Windows::Foundation::Uri{ field.LinkUri() }); } CATCH_LOG();
                    Controls::ToolTipService::SetToolTip(link, box_value(field.Label()));
                    Control::HyperlinkPreviewHelpers::AttachLinkTooltips(link, _hyperlinkPreviewProvider, _core.Settings(), true, 0);
                    item.Children().Append(link);
                }
                else
                {
                    Controls::TextBlock text;
                    text.Text(field.Value());
                    Controls::Border badge;
                    badge.Child(text);
                    badge.Padding(Thickness{ 6, 2, 6, 2 });
                    badge.CornerRadius(Windows::UI::Xaml::CornerRadius{ 4 });
                    if (field.IsBadge()) badge.Background(HyperlinkPreviewHelpers::BadgeBrush(field.Color()));
                    item.Children().Append(badge);
                }
                if (placement == L"breadcrumb")
                {
                    // Jira's own separator, and the reason a breadcrumb cannot be
                    // a run of independent chips: the slash only means something
                    // between two of them, so it is drawn by whoever arrives
                    // second rather than declared by either.
                    const auto trail = HyperlinkCardBreadcrumbHost();
                    if (trail.Children().Size() > 0)
                    {
                        Controls::TextBlock separator;
                        separator.Text(winrt::hstring{ L"/" });
                        separator.Opacity(0.5);
                        separator.VerticalAlignment(VerticalAlignment::Center);
                        trail.Children().Append(separator);
                    }
                    trail.Children().Append(item);
                    continue;
                }
                const auto host = placement == L"status" ? HyperlinkCardStatusHost() : HyperlinkCardMetadataHost();
                host.Children().Append(item);
                continue;
            }

            Controls::TextBlock label;
            label.Text(field.Label());
            label.Opacity(0.7);
            label.VerticalAlignment(VerticalAlignment::Top);
            label.TextWrapping(TextWrapping::Wrap);
            Controls::Grid::SetRow(label, row);
            Controls::Grid::SetColumn(label, 0);
            grid.Children().Append(label);

            // The value cell is a Grid and not a horizontal StackPanel: a horizontal
            // StackPanel measures its children with infinite width, so TextWrapping
            // never takes effect and one long value drags the whole card to MaxWidth.
            Controls::Grid cell;
            cell.ColumnSpacing(6);
            {
                Controls::ColumnDefinition iconColumn;
                iconColumn.Width(GridLength{ 16, GridUnitType::Pixel });
                cell.ColumnDefinitions().Append(iconColumn);

                Controls::ColumnDefinition valueColumn;
                valueColumn.Width(GridLength{ 1, GridUnitType::Star });
                cell.ColumnDefinitions().Append(valueColumn);
            }

            if (field.HasIcon())
            {
                Controls::Image icon;
                icon.Width(16);
                icon.Height(16);
                icon.VerticalAlignment(VerticalAlignment::Top);
                icon.Margin(Thickness{ 0, 2, 0, 0 });
                icon.Source(HyperlinkPreviewHelpers::ImageFromUri(field.IconUri()));
                Controls::Grid::SetColumn(icon, 0);
                cell.Children().Append(icon);
            }

            if (field.IsBadge())
            {
                Controls::Border badge;
                badge.Padding(Thickness{ 6, 1, 6, 1 });
                badge.CornerRadius(Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
                badge.Background(HyperlinkPreviewHelpers::BadgeBrush(field.Color()));
                badge.HorizontalAlignment(HorizontalAlignment::Left);
                badge.VerticalAlignment(VerticalAlignment::Top);

                Controls::TextBlock badgeText;
                badgeText.FontSize(12);
                badgeText.Text(field.Value());
                badge.Child(badgeText);

                Controls::Grid::SetColumn(badge, 1);
                cell.Children().Append(badge);
            }
            else if (!field.LinkUri().empty())
            {
                // A row whose value IS somewhere to go -- a pull request against
                // the ticket -- is worth clicking, and worth hovering: the
                // nested preview resolves it with whichever integration claims
                // it, so a Jira card leads to a GitHub one.
                Controls::TextBlock text;
                text.Text(field.Value());
                text.TextWrapping(TextWrapping::Wrap);
                text.MaxLines(6);

                Controls::HyperlinkButton link;
                link.Content(text);
                link.Padding(Thickness{ 0, 0, 0, 0 });
                link.HorizontalAlignment(HorizontalAlignment::Left);
                try { link.NavigateUri(Windows::Foundation::Uri{ field.LinkUri() }); } CATCH_LOG();
                Control::HyperlinkPreviewHelpers::AttachLinkTooltips(link, _hyperlinkPreviewProvider, _core.Settings(), true, 0);
                Controls::Grid::SetColumn(link, 1);
                cell.Children().Append(link);
            }
            else
            {
                Controls::TextBlock value;
                value.Text(field.Value());
                value.TextWrapping(TextWrapping::Wrap);
                value.MaxLines(6);
                value.IsTextSelectionEnabled(true);
                Controls::Grid::SetColumn(value, 1);
                cell.Children().Append(value);
            }

            Controls::Grid::SetRow(cell, row);
            Controls::Grid::SetColumn(cell, field.Label().empty() ? 0 : 1);
            if (field.Label().empty()) Controls::Grid::SetColumnSpan(cell, 2);
            grid.Children().Append(cell);
            ++row;
        }
    }

    void TermControl::_fillHyperlinkComments(const Control::HyperlinkPreviewTab& tab)
    {
        const auto host = HyperlinkCardComments();
        host.Children().Clear();

        const auto comments = tab ? tab.Comments() : nullptr;
        if (!comments)
        {
            return;
        }

        for (const auto& comment : comments)
        {
            if (!comment)
            {
                continue;
            }

            Controls::Grid entry;
            entry.ColumnSpacing(6);
            {
                Controls::ColumnDefinition avatarColumn;
                avatarColumn.Width(GridLength{ 0, GridUnitType::Auto });
                entry.ColumnDefinitions().Append(avatarColumn);

                Controls::ColumnDefinition bodyColumn;
                bodyColumn.Width(GridLength{ 1, GridUnitType::Star });
                entry.ColumnDefinitions().Append(bodyColumn);
            }

            if (const auto avatar = comment.AvatarUri(); !avatar.empty())
            {
                Controls::Image image;
                image.Width(20);
                image.Height(20);
                image.VerticalAlignment(VerticalAlignment::Top);
                image.Source(HyperlinkPreviewHelpers::ImageFromUri(avatar));
                Controls::Grid::SetColumn(image, 0);
                entry.Children().Append(image);
            }

            Controls::StackPanel text;
            text.Spacing(2);

            Controls::TextBlock heading;
            auto headingText = std::wstring{ comment.Author() };
            if (const auto time = comment.Time(); !time.empty())
            {
                if (!headingText.empty())
                {
                    headingText.append(L" · ");
                }
                headingText.append(std::wstring_view{ time });
            }
            heading.Text(winrt::hstring{ headingText });
            heading.Opacity(0.7);
            heading.FontSize(12);
            heading.TextWrapping(TextWrapping::Wrap);
            text.Children().Append(heading);

            bool rendered = false;
            if (tab.Format() == L"markdown" && !comment.Body().empty())
            {
                try
                {
                    auto body = winrt::Microsoft::Terminal::UI::Markdown::Builder::Convert(comment.Body(), L"");
                    Control::HyperlinkPreviewHelpers::AttachLinkTooltips(body, _hyperlinkPreviewProvider, _core.Settings(), true, 0);
                    body.MaxHeight(180);
                    body.IsTextSelectionEnabled(true);
                    text.Children().Append(body);
                    rendered = true;
                }
                CATCH_LOG();
            }
            if (!rendered)
            {
                auto body = Control::HyperlinkPreviewHelpers::CreateTextView(comment.Body(), _hyperlinkPreviewProvider, _core.Settings(), true, 0);
                body.MaxLines(6);
                text.Children().Append(body);
            }

            Controls::Grid::SetColumn(text, 1);
            entry.Children().Append(text);

            host.Children().Append(entry);
        }
    }

    // A row of ToggleButtons, one for the built-in field list and one for each tab
    // the integration declared. Deliberately not a Pivot or a TabView: both bring a
    // great deal of chrome for what is, on a tooltip, four words in a row.
    void TermControl::_rebuildHyperlinkTabStrip(const Control::HyperlinkPreview& preview)
    {
        const auto strip = HyperlinkCardTabStrip();
        strip.Children().Clear();
        _hyperlinkSelectedTab = -1;

        const auto tabs = preview ? preview.Tabs() : nullptr;
        if (!tabs || tabs.Size() == 0)
        {
            HyperlinkCardTabStripBorder().Visibility(Visibility::Collapsed);
            strip.Visibility(Visibility::Collapsed);
            _showHyperlinkTab(-1);
            return;
        }

        auto addButton = [&](const winrt::hstring& label, int32_t index) {
            Controls::Primitives::ToggleButton button;
            button.Content(winrt::box_value(label));
            button.Tag(winrt::box_value(index));
            button.Padding(Thickness{ 10, 3, 10, 3 });
            button.MinWidth(0);
            button.FontSize(12);
            button.BorderThickness(Thickness{ 0, 0, 0, 0 });
            button.CornerRadius(Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
            button.IsChecked(index == _hyperlinkSelectedTab);
            button.Click({ this, &TermControl::_HyperlinkTabClick });
            strip.Children().Append(button);
        };

        addButton(RS_(L"HyperlinkFieldsTabLabel"), -1);
        for (uint32_t i = 0; i < tabs.Size(); ++i)
        {
            const auto tab = tabs.GetAt(i);
            addButton(tab ? tab.Label() : winrt::hstring{}, gsl::narrow_cast<int32_t>(i));
        }

        strip.Visibility(Visibility::Visible);
        HyperlinkCardTabStripBorder().Visibility(Visibility::Visible);
        _showHyperlinkTab(-1);
    }

    void TermControl::_HyperlinkTabClick(const IInspectable& sender, const RoutedEventArgs& /*e*/)
    {
        if (const auto button = sender.try_as<Controls::Primitives::ToggleButton>())
        {
            if (const auto index = button.Tag().try_as<int32_t>())
            {
                if (*index == _hyperlinkSelectedTab)
                {
                    button.IsChecked(true);
                    return;
                }
                _showHyperlinkTab(*index);
            }
        }
    }

    // -1 is the built-in field list; 0.. index into the preview's own Tabs. Anything
    // out of range falls back to the field list rather than showing nothing at all.
    void TermControl::_showHyperlinkTab(int32_t index)
    {
        Control::HyperlinkPreviewTab tab{ nullptr };
        if (index >= 0 && _currentHyperlinkPreview)
        {
            if (const auto tabs = _currentHyperlinkPreview.Tabs(); tabs && gsl::narrow_cast<uint32_t>(index) < tabs.Size())
            {
                tab = tabs.GetAt(gsl::narrow_cast<uint32_t>(index));
            }
        }
        if (!tab)
        {
            index = -1;
        }

        _hyperlinkSelectedTab = index;

        const auto kind = tab ? tab.Kind() : Control::HyperlinkPreviewTabKind::Fields;
        const auto showFields = !tab || kind == Control::HyperlinkPreviewTabKind::Fields;
        const auto showBody = tab && kind == Control::HyperlinkPreviewTabKind::Body;
        const auto showComments = tab && kind == Control::HyperlinkPreviewTabKind::Comments;
        // A tab the integration handed over as a name only. Asking for its
        // content is what a first visit does; the ring stands in until it
        // arrives, and the answer is kept so a second visit is immediate.
        const auto pending = tab && tab.Pending();

        if (pending)
        {
            Controls::ProgressRing ring;
            ring.Width(16);
            ring.Height(16);
            ring.MinWidth(0);
            ring.MinHeight(0);
            ring.HorizontalAlignment(HorizontalAlignment::Left);
            ring.Margin(Thickness{ 0, 4, 0, 4 });
            ring.IsActive(true);

            if (showComments)
            {
                HyperlinkCardComments().Children().Clear();
                HyperlinkCardComments().Children().Append(ring);
            }
            else
            {
                HyperlinkCardBody().Visibility(Visibility::Collapsed);
                HyperlinkCardBodyMarkdown().Content(ring);
                HyperlinkCardBodyMarkdown().Visibility(Visibility::Visible);
            }

            _requestHyperlinkTab(_hyperlinkPreviewGeneration, _currentHyperlinkPreview, index, tab.Key());
        }
        else if (showBody)
        {
            // A markdown body is rendered, not printed. It used to be shown as its
            // own source on the grounds that the card is small - but small is an
            // argument for rendering it, not against: "**Blocked** on `AT-158`"
            // costs more characters than it does formatting, and a Jira
            // description arrived as a wall of punctuation.
            //
            // Converted ADF lands here as markdown too, so Jira and GitHub bodies
            // now go through the same renderer as everything else.
            auto rendered = false;
            if (tab.Format() == L"markdown" && !tab.Body().empty())
            {
                try
                {
                    HyperlinkCardBodyMarkdown().Content(
                        winrt::Microsoft::Terminal::UI::Markdown::Builder::Convert(tab.Body(), L""));
                    if (const auto body = HyperlinkCardBodyMarkdown().Content().try_as<FrameworkElement>())
                        Control::HyperlinkPreviewHelpers::AttachLinkTooltips(body, _hyperlinkPreviewProvider, _core.Settings(), true, 0);
                    rendered = true;
                }
                CATCH_LOG(); // a body that will not parse is still worth reading as text
            }

            if (!rendered)
            {
                HyperlinkCardBodyMarkdown().Content(Control::HyperlinkPreviewHelpers::CreateTextView(tab.Body(), _hyperlinkPreviewProvider, _core.Settings(), true, 0));
                rendered = true;
            }

            HyperlinkCardBody().Visibility(rendered ? Visibility::Collapsed : Visibility::Visible);
            HyperlinkCardBodyMarkdown().Visibility(rendered ? Visibility::Visible : Visibility::Collapsed);
        }
        if (showComments && !pending)
        {
            _fillHyperlinkComments(tab);
        }

        HyperlinkCardFieldsScroll().Visibility(showFields ? Visibility::Visible : Visibility::Collapsed);
        HyperlinkCardBodyScroll().Visibility(showBody ? Visibility::Visible : Visibility::Collapsed);
        HyperlinkCardCommentsScroll().Visibility(showComments ? Visibility::Visible : Visibility::Collapsed);

        Media::Brush activeBrush = nullptr;
        if (Resources().HasKey(winrt::box_value(L"ApplicationPageBackgroundThemeBrush")))
        {
            activeBrush = Resources().Lookup(winrt::box_value(L"ApplicationPageBackgroundThemeBrush")).try_as<Media::Brush>();
        }
        if (!activeBrush && Application::Current().Resources().HasKey(winrt::box_value(L"ApplicationPageBackgroundThemeBrush")))
        {
            activeBrush = Application::Current().Resources().Lookup(winrt::box_value(L"ApplicationPageBackgroundThemeBrush")).try_as<Media::Brush>();
        }
        if (!activeBrush)
        {
            activeBrush = Media::SolidColorBrush{ Windows::UI::Colors::Gray() };
        }
        const Media::SolidColorBrush transparentBrush{ Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0) };

        // Only one of the toggles can be down at a time; there is no ready-made radio
        // behaviour on ToggleButton, so it is enforced here.
        for (const auto& child : HyperlinkCardTabStrip().Children())
        {
            if (const auto button = child.try_as<Controls::Primitives::ToggleButton>())
            {
                const auto buttonIndex = button.Tag().try_as<int32_t>();
                const auto isSelected = buttonIndex && *buttonIndex == index;
                button.IsChecked(isSelected);
                button.FontWeight(isSelected ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
                button.Background(isSelected ? activeBrush : transparentBrush);
            }
        }
    }

    // One tab's content, fetched because the user opened it. Two guards, for two
    // different races: the generation says the pointer has not moved on to
    // another link, and the preview itself says an action's refresh has not
    // replaced the object whose Tabs we are about to write into. The content is
    // stored even when the user has since switched tabs -- it is the answer to a
    // question they asked, and coming back to it should cost nothing.
    safe_void_coroutine TermControl::_requestHyperlinkTab(uint32_t generation, Control::HyperlinkPreview preview, int32_t index, winrt::hstring tabKey)
    {
        const auto weakThis{ get_weak() };
        const auto provider{ _hyperlinkPreviewProvider };
        const auto dispatcher{ Dispatcher() };
        if (!provider || !dispatcher || !preview || tabKey.empty() || index < 0)
        {
            co_return;
        }

        auto text = preview.SourceText();
        if (text.empty())
        {
            text = _hoveredUri;
        }
        const auto hint = _currentHyperlinkTooltipSettings.integration;

        Control::HyperlinkPreviewTab built{ nullptr };
        try
        {
            built = co_await provider.GetTabAsync(text, hint, tabKey);
        }
        CATCH_LOG();

        co_await winrt::resume_foreground(dispatcher);

        const auto self = weakThis.get();
        if (!self || self->_IsClosing() || self->_hyperlinkPreviewGeneration != generation ||
            self->_currentHyperlinkPreview != preview || !built)
        {
            co_return;
        }

        const auto tabs = preview.Tabs();
        if (!tabs || gsl::narrow_cast<uint32_t>(index) >= tabs.Size())
        {
            co_return;
        }
        tabs.SetAt(gsl::narrow_cast<uint32_t>(index), built);

        if (self->_hyperlinkSelectedTab == index)
        {
            self->_showHyperlinkTab(index);
            // The card just grew by a description or a comment thread, so it has
            // to be measured and placed for the size it is now.
            if (self->HyperlinkCard().Visibility() == Visibility::Visible)
            {
                self->_showHyperlinkCard();
            }
        }
    }

    // Only the first action is offered. The card has room for one row, and no
    // integration declares more than one today; a second would want the pane.
    void TermControl::_rebuildHyperlinkActions(const Control::HyperlinkPreview& preview)
    {
        const auto combo = HyperlinkActionOptions();
        combo.Items().Clear();
        HyperlinkActionFields().Children().Clear();
        HyperlinkActionFields().Visibility(Visibility::Collapsed);
        HyperlinkActionError().Visibility(Visibility::Collapsed);
        _currentHyperlinkAction = nullptr;

        const auto actions = preview ? preview.Actions() : nullptr;
        if (!actions || actions.Size() == 0)
        {
            HyperlinkCardActionRow().Visibility(Visibility::Collapsed);
            return;
        }

        _currentHyperlinkAction = actions.GetAt(0);
        const auto options = _currentHyperlinkAction ? _currentHyperlinkAction.Options() : nullptr;
        if (!options || options.Size() == 0)
        {
            _currentHyperlinkAction = nullptr;
            HyperlinkCardActionRow().Visibility(Visibility::Collapsed);
            return;
        }

        for (const auto& option : options)
        {
            if (!option)
            {
                continue;
            }

            // Jira's own menu wording: what the transition is called, then the status
            // it lands in. The badge is the same one the field list would draw.
            Controls::StackPanel content;
            content.Orientation(Controls::Orientation::Horizontal);
            content.Spacing(6);

            Controls::TextBlock label;
            label.Text(option.Label());
            content.Children().Append(label);

            if (const auto badgeText = option.Badge(); !badgeText.empty())
            {
                Controls::TextBlock arrow;
                arrow.Text(winrt::hstring{ L"→" });
                arrow.Opacity(0.7);
                content.Children().Append(arrow);

                Controls::Border badge;
                badge.Padding(Thickness{ 6, 1, 6, 1 });
                badge.CornerRadius(Windows::UI::Xaml::CornerRadius{ 4, 4, 4, 4 });
                badge.Background(HyperlinkPreviewHelpers::BadgeBrush(option.Color()));
                badge.VerticalAlignment(VerticalAlignment::Center);

                Controls::TextBlock badgeLabel;
                badgeLabel.FontSize(12);
                badgeLabel.Text(badgeText);
                badge.Child(badgeLabel);
                content.Children().Append(badge);
            }

            Controls::ComboBoxItem item;
            item.Content(content);
            // The option itself, so the picked one never has to be looked up by index
            // in a list that a refresh may have rebuilt underneath us.
            item.Tag(option);
            combo.Items().Append(item);
        }

        HyperlinkCardActionRow().Visibility(Visibility::Visible);
        // An undo offered by an action that has already run outlives the refresh that
        // followed it, which is what makes the button still be there afterwards.
        HyperlinkActionUndoButton().Visibility(_hyperlinkUndoChoiceId.empty() ? Visibility::Collapsed : Visibility::Visible);
        _updateHyperlinkActionApplyState();
    }

    Control::HyperlinkPreviewActionOption TermControl::_selectedHyperlinkActionOption()
    {
        if (const auto item = HyperlinkActionOptions().SelectedItem().try_as<Controls::ComboBoxItem>())
        {
            return item.Tag().try_as<Control::HyperlinkPreviewActionOption>();
        }
        return nullptr;
    }

    // The far end sometimes refuses a change without extra values -- Jira's "pick a
    // resolution" dialog is the canonical one. The form only appears for an option
    // that says so, and Apply stays disabled until the required boxes have something.
    void TermControl::_updateHyperlinkActionFields()
    {
        const auto host = HyperlinkActionFields();
        host.Children().Clear();

        const auto option = _selectedHyperlinkActionOption();
        const auto fields = option ? option.Fields() : nullptr;
        if (!option || !option.NeedsFields() || !fields)
        {
            host.Visibility(Visibility::Collapsed);
            _updateHyperlinkActionApplyState();
            return;
        }

        auto onChanged = [weakThis = get_weak()]() {
            if (const auto self = weakThis.get(); self && !self->_IsClosing())
            {
                self->_updateHyperlinkActionApplyState();
            }
        };

        for (const auto& field : fields)
        {
            if (!field)
            {
                continue;
            }

            Controls::TextBlock label;
            label.Text(field.Label());
            label.Opacity(0.7);
            label.FontSize(12);
            label.TextWrapping(TextWrapping::Wrap);
            host.Children().Append(label);

            const auto options = field.Options();
            if (options && options.Size() > 0)
            {
                Controls::ComboBox picker;
                picker.Tag(winrt::box_value(field.Key()));
                picker.MinWidth(150);
                for (const auto& choice : options)
                {
                    picker.Items().Append(winrt::box_value(choice));
                }
                picker.SelectionChanged([onChanged](auto&&, auto&&) { onChanged(); });
                host.Children().Append(picker);
            }
            else
            {
                Controls::TextBox box;
                box.Tag(winrt::box_value(field.Key()));
                box.TextChanged([onChanged](auto&&, auto&&) { onChanged(); });
                host.Children().Append(box);
            }
        }

        host.Visibility(host.Children().Size() > 0 ? Visibility::Visible : Visibility::Collapsed);
        _updateHyperlinkActionApplyState();
    }

    // Apply is only live once something is picked and every required value is filled in.
    void TermControl::_updateHyperlinkActionApplyState()
    {
        const auto option = _selectedHyperlinkActionOption();
        auto ready = static_cast<bool>(option);

        if (ready && option.NeedsFields())
        {
            const auto values = _collectHyperlinkActionFields();
            if (const auto fields = option.Fields())
            {
                for (const auto& field : fields)
                {
                    if (!field || !field.Required())
                    {
                        continue;
                    }
                    if (!values.HasKey(field.Key()) || values.Lookup(field.Key()).empty())
                    {
                        ready = false;
                        break;
                    }
                }
            }
        }

        HyperlinkActionApplyButton().IsEnabled(ready);
    }

    Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring> TermControl::_collectHyperlinkActionFields()
    {
        auto values = winrt::single_threaded_map<winrt::hstring, winrt::hstring>();

        for (const auto& child : HyperlinkActionFields().Children())
        {
            if (const auto box = child.try_as<Controls::TextBox>())
            {
                if (const auto key = box.Tag().try_as<winrt::hstring>(); key && !key->empty())
                {
                    values.Insert(*key, box.Text());
                }
            }
            else if (const auto picker = child.try_as<Controls::ComboBox>())
            {
                if (const auto key = picker.Tag().try_as<winrt::hstring>(); key && !key->empty())
                {
                    const auto selected = picker.SelectedItem().try_as<winrt::hstring>();
                    values.Insert(*key, selected ? *selected : winrt::hstring{});
                }
            }
        }

        return values;
    }

    void TermControl::_setHyperlinkActionBusy(bool busy)
    {
        HyperlinkActionProgress().IsActive(busy);
        HyperlinkActionProgress().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        HyperlinkActionOptions().IsEnabled(!busy);
        HyperlinkActionUndoButton().IsEnabled(!busy);
        // A StackPanel is not a Control, so it has no IsEnabled: block the input
        // and dim it instead, which is what IsEnabled would have looked like.
        HyperlinkActionFields().IsHitTestVisible(!busy);
        HyperlinkActionFields().Opacity(busy ? 0.5 : 1.0);
        if (busy)
        {
            HyperlinkActionApplyButton().IsEnabled(false);
        }
        else
        {
            _updateHyperlinkActionApplyState();
        }
    }

    void TermControl::_HyperlinkActionOptionChanged(const IInspectable& /*sender*/, const Controls::SelectionChangedEventArgs& /*e*/)
    {
        HyperlinkActionError().Visibility(Visibility::Collapsed);
        _updateHyperlinkActionFields();

        // The form may have just appeared or gone away underneath the card.
        if (HyperlinkCard().Visibility() == Visibility::Visible)
        {
            _showHyperlinkCard();
        }
    }

    void TermControl::_HyperlinkActionDropDownOpened(const IInspectable& /*sender*/, const IInspectable& /*e*/)
    {
        _hyperlinkActionDropDownOpen = true;
        _hyperlinkHideTimer.Stop();
    }

    void TermControl::_HyperlinkActionDropDownClosed(const IInspectable& /*sender*/, const IInspectable& /*e*/)
    {
        _hyperlinkActionDropDownOpen = false;
        // The pointer may have ended up outside the card while the list was open, in
        // which case the hide the list was holding off is now due.
        _scheduleHyperlinkCardHide();
    }

    void TermControl::_HyperlinkActionApplyClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        if (const auto option = _selectedHyperlinkActionOption())
        {
            _invokeHyperlinkAction(_hyperlinkPreviewGeneration, option.Id());
        }
    }

    void TermControl::_HyperlinkActionUndoClick(const IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        const auto choiceId = _hyperlinkUndoChoiceId;
        if (choiceId.empty())
        {
            return;
        }

        // Undo is the far end's own way back, not the form the user filled in for the
        // change being undone -- clear it so nothing stale rides along with the request.
        _hyperlinkUndoChoiceId = {};
        HyperlinkActionUndoButton().Visibility(Visibility::Collapsed);
        HyperlinkActionFields().Children().Clear();
        HyperlinkActionFields().Visibility(Visibility::Collapsed);

        _invokeHyperlinkAction(_hyperlinkPreviewGeneration, choiceId);
    }

    // Running an action is two round trips -- the change, then a refresh that shows
    // it -- so it takes the same generation guard the preview fetch does: by the time
    // either returns the pointer may be over a different link entirely.
    safe_void_coroutine TermControl::_invokeHyperlinkAction(uint32_t generation, winrt::hstring choiceId)
    {
        const auto weakThis{ get_weak() };
        const auto provider{ _hyperlinkPreviewProvider };
        const auto dispatcher{ Dispatcher() };
        const auto action{ _currentHyperlinkAction };
        const auto preview{ _currentHyperlinkPreview };
        if (!provider || !dispatcher || !action || !preview)
        {
            co_return;
        }

        // What the integration said this preview was made from, so an action runs
        // against the same thing even when the hovered run was a text match.
        auto sourceText = preview.SourceText();
        if (sourceText.empty())
        {
            sourceText = _hoveredUri;
        }
        const auto integration = _currentHyperlinkTooltipSettings.integration;
        const auto actionKey = action.Key();
        const auto fieldValues = _collectHyperlinkActionFields();

        _setHyperlinkActionBusy(true);
        HyperlinkActionError().Visibility(Visibility::Collapsed);

        Control::HyperlinkActionResult result{ nullptr };
        try
        {
            result = co_await provider.InvokeActionAsync(sourceText, integration, actionKey, choiceId, fieldValues);
        }
        CATCH_LOG();

        Control::HyperlinkPreview refreshed{ nullptr };
        if (result && result.Ok())
        {
            try
            {
                refreshed = co_await provider.RefreshAsync(sourceText, integration);
            }
            CATCH_LOG();
        }

        co_await winrt::resume_foreground(dispatcher);

        const auto self = weakThis.get();
        if (!self || self->_IsClosing() || self->_hyperlinkPreviewGeneration != generation)
        {
            co_return;
        }

        self->_setHyperlinkActionBusy(false);

        if (!result || !result.Ok())
        {
            auto message = result ? result.Error() : winrt::hstring{};
            if (message.empty())
            {
                message = RS_(L"HyperlinkActionFailed");
            }
            self->HyperlinkActionError().Text(message);
            self->HyperlinkActionError().Visibility(Visibility::Visible);
            co_return;
        }

        // Repaint from the refreshed result before the undo state is written, because
        // rebuilding the action row is what puts the Undo button back on screen.
        if (refreshed)
        {
            self->_applyHyperlinkPreview(refreshed);
        }

        self->_hyperlinkUndoChoiceId = result.UndoChoiceId();
        const auto hasUndo = !self->_hyperlinkUndoChoiceId.empty();
        if (hasUndo)
        {
            if (const auto undoLabel = result.UndoLabel(); !undoLabel.empty())
            {
                if (!self->_hyperlinkUndoDefaultContent)
                {
                    self->_hyperlinkUndoDefaultContent = self->HyperlinkActionUndoButton().Content();
                }
                self->HyperlinkActionUndoButton().Content(winrt::box_value(undoLabel));
            }
        }
        else if (self->_hyperlinkUndoDefaultContent)
        {
            self->HyperlinkActionUndoButton().Content(self->_hyperlinkUndoDefaultContent);
        }
        self->HyperlinkActionUndoButton().Visibility(hasUndo ? Visibility::Visible : Visibility::Collapsed);

        // An empty UndoChoiceId is a real answer: the integration went looking for a
        // way back and there isn't one. Left unsaid it would look exactly like a
        // button that has not appeared yet, so the card says which of the two it is.
        if (!hasUndo)
        {
            self->HyperlinkActionError().Text(RS_(L"HyperlinkActionNoUndo"));
            self->HyperlinkActionError().Visibility(Visibility::Visible);
        }

        if (self->HyperlinkCard().Visibility() == Visibility::Visible)
        {
            self->_showHyperlinkCard();
        }
    }

    safe_void_coroutine TermControl::_updateSelectionMarkers(IInspectable /*sender*/, Control::UpdateSelectionMarkersEventArgs args)
    {
        if (!args)
        {
            co_return;
        }

        auto weakThis{ get_weak() };
        co_await resume_foreground(Dispatcher());
        if (const auto strong = weakThis.get())
        {
            if (_core.HasSelection() && !args.ClearMarkers())
            {
                // retrieve all of the necessary selection marker data
                // from the TerminalCore layer under one lock to improve performance
                const auto markerData{ _core.SelectionInfo() };

                // lambda helper function that can be used to display a selection marker
                // - targetEnd: if true, target the "end" selection marker. Otherwise, target "start".
                auto displayMarker = [&](bool targetEnd) {
                    const auto flipMarker{ targetEnd ? markerData.EndAtRightBoundary : markerData.StartAtLeftBoundary };
                    const auto& marker{ targetEnd ? SelectionEndMarker() : SelectionStartMarker() };

                    // Ensure the marker is oriented properly
                    // (i.e. if start is at the beginning of the buffer, it should be flipped)
                    //
                    // Note - This RenderTransform might not be a
                    // ScaleTransform, if we haven't had a _coreFontSizeChanged
                    // handled yet, because that's the first place we set the
                    // RenderTransform
                    if (const auto& transform{ marker.RenderTransform().try_as<Windows::UI::Xaml::Media::ScaleTransform>() })
                    {
                        transform.ScaleX(std::abs(transform.ScaleX()) * (flipMarker ? -1.0 : 1.0));
                        marker.RenderTransform(transform);
                    }

                    // Compute the location of the top left corner of the cell in DIPS
                    auto terminalPos{ targetEnd ? markerData.EndPos : markerData.StartPos };
                    if (flipMarker)
                    {
                        // When we flip the marker, a negative scaling makes us be one cell-width to the left.
                        // Add one to the viewport pos' x-coord to fix that.
                        terminalPos.X += 1;
                    }
                    const auto locationInDIPs{ _toPosInDips(terminalPos) };

                    // Move the marker to the top left corner of the cell
                    SelectionCanvas().SetLeft(marker,
                                              (locationInDIPs.X - SwapChainPanel().ActualOffset().x));
                    SelectionCanvas().SetTop(marker,
                                             (locationInDIPs.Y - SwapChainPanel().ActualOffset().y));
                    marker.Visibility(Visibility::Visible);
                };

                // show/update selection markers
                // figure out which endpoint to move, get it and the relevant icon (hide the other icon)
                const auto movingEnd{ WI_IsFlagSet(markerData.Endpoint, SelectionEndpointTarget::End) };
                const auto selectionAnchor{ movingEnd ? markerData.EndPos : markerData.StartPos };
                const auto& marker{ movingEnd ? SelectionEndMarker() : SelectionStartMarker() };
                const auto& otherMarker{ movingEnd ? SelectionStartMarker() : SelectionEndMarker() };
                if (selectionAnchor.Y < 0 || selectionAnchor.Y >= _core.ViewportSize().Height)
                {
                    // if the endpoint is outside of the viewport,
                    // just hide the markers
                    marker.Visibility(Visibility::Collapsed);
                    otherMarker.Visibility(Visibility::Collapsed);
                    co_return;
                }
                else if (WI_AreAllFlagsSet(markerData.Endpoint, SelectionEndpointTarget::Start | SelectionEndpointTarget::End))
                {
                    // display both markers
                    displayMarker(true);
                    displayMarker(false);
                }
                else
                {
                    // display one marker,
                    // but hide the other
                    displayMarker(movingEnd);
                    otherMarker.Visibility(Visibility::Collapsed);
                }
            }
            else
            {
                // hide selection markers
                SelectionStartMarker().Visibility(Visibility::Collapsed);
                SelectionEndMarker().Visibility(Visibility::Collapsed);
            }
        }
    }

    winrt::Windows::Foundation::Point TermControl::_toPosInDips(const Core::Point terminalCellPos)
    {
        const auto marginsInDips{ GetPadding() };
        const auto fontSize{ _core.FontSizeInDips() };
        return {
            terminalCellPos.X * fontSize.Width + static_cast<float>(marginsInDips.Left),
            terminalCellPos.Y * fontSize.Height + static_cast<float>(marginsInDips.Top),
        };
    }

    void TermControl::_coreFontSizeChanged(const IInspectable& /*sender*/,
                                           const Control::FontSizeChangedArgs& args)
    {
        // scale the selection markers to be the size of a cell
        const auto dpiScale = SwapChainPanel().CompositionScaleX();
        auto scaleMarker = [&args, &dpiScale](const Windows::UI::Xaml::Shapes::Path& shape) {
            // The selection markers were designed to be 5x14 in size,
            // so use those dimensions below for the scaling
            const auto scaleX = args.Width() / 5.0 / dpiScale;
            const auto scaleY = args.Height() / 14.0 / dpiScale;

            Windows::UI::Xaml::Media::ScaleTransform transform;
            transform.ScaleX(scaleX);
            transform.ScaleY(scaleY);
            shape.RenderTransform(transform);

            // now hide the shape
            shape.Visibility(Visibility::Collapsed);
        };
        scaleMarker(SelectionStartMarker());
        scaleMarker(SelectionEndMarker());

        if (Feature_QuickFix::IsEnabled())
        {
            QuickFixButton().Height(args.Height() / dpiScale);
            QuickFixIcon().FontSize(static_cast<double>(args.Width() / dpiScale));
            RefreshQuickFixMenu();
        }

        _searchScrollOffset = _calculateSearchScrollOffset();

        // _ShowResizeOverlay is shown when the swap chain panel size changes.
        // But changing the font size changes the viewport size as well.
        // So, track that too.
        _ShowResizeOverlay();
    }

    void TermControl::_coreRaisedNotice(const IInspectable& /*sender*/,
                                        const Control::NoticeEventArgs& eventArgs)
    {
        // Don't try to inspect the core here. The Core might be raising this
        // while it's holding its write lock. If the handlers calls back to some
        // method on the TermControl on the same thread, and _that_ method calls
        // to ControlCore, we might be in danger of deadlocking.
        RaiseNotice.raise(*this, eventArgs);
    }

    Control::MouseButtonState TermControl::GetPressedMouseButtons(const winrt::Windows::UI::Input::PointerPoint point)
    {
        Control::MouseButtonState state{};
        WI_SetFlagIf(state, Control::MouseButtonState::IsLeftButtonDown, point.Properties().IsLeftButtonPressed());
        WI_SetFlagIf(state, Control::MouseButtonState::IsMiddleButtonDown, point.Properties().IsMiddleButtonPressed());
        WI_SetFlagIf(state, Control::MouseButtonState::IsRightButtonDown, point.Properties().IsRightButtonPressed());
        return state;
    }

    unsigned int TermControl::GetPointerUpdateKind(const winrt::Windows::UI::Input::PointerPoint point)
    {
        const auto props = point.Properties();

        // Which mouse button changed state (and how)
        unsigned int uiButton{};
        switch (props.PointerUpdateKind())
        {
        case winrt::Windows::UI::Input::PointerUpdateKind::LeftButtonPressed:
            uiButton = WM_LBUTTONDOWN;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::LeftButtonReleased:
            uiButton = WM_LBUTTONUP;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::MiddleButtonPressed:
            uiButton = WM_MBUTTONDOWN;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::MiddleButtonReleased:
            uiButton = WM_MBUTTONUP;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::RightButtonPressed:
            uiButton = WM_RBUTTONDOWN;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::RightButtonReleased:
            uiButton = WM_RBUTTONUP;
            break;
        default:
            uiButton = WM_MOUSEMOVE;
        }

        return uiButton;
    }

    void TermControl::_coreWarningBell(const IInspectable& /*sender*/, const IInspectable& /*args*/)
    {
        _playWarningBell->Run();
    }

    hstring TermControl::ReadEntireBuffer() const
    {
        return _core.ReadEntireBuffer();
    }
    hstring TermControl::ReadViewportText(int32_t maxRows) const
    {
        return _core.ReadViewportText(maxRows);
    }
    bool TermControl::ViewportContains(const hstring& needle) const
    {
        return _core.ViewportContains(needle);
    }
    Control::CommandHistoryContext TermControl::CommandHistory() const
    {
        return _core.CommandHistory();
    }

    void TermControl::UpdateWinGetSuggestions(Windows::Foundation::Collections::IVector<hstring> suggestions)
    {
        get_self<ControlCore>(_core)->UpdateQuickFixes(suggestions);
    }

    void TermControl::AdjustOpacity(const float opacity, const bool relative)
    {
        _core.AdjustOpacity(opacity, relative);
    }

    // - You'd think this should just be "Opacity", but UIElement already
    //   defines an "Opacity", which we're actually not setting at all. We're
    //   not overriding or changing _that_ value. Callers that want the opacity
    //   set by the settings should call this instead.
    float TermControl::BackgroundOpacity() const
    {
        return _core.Opacity();
    }

    bool TermControl::HasSelection() const
    {
        return _core.HasSelection();
    }
    bool TermControl::HasMultiLineSelection() const
    {
        return _core.HasMultiLineSelection();
    }
    winrt::hstring TermControl::SelectedText(bool trimTrailingWhitespace) const
    {
        return _core.SelectedText(trimTrailingWhitespace);
    }

    void TermControl::_refreshSearch()
    {
        if (!_searchBox || !_searchBox->IsOpen())
        {
            return;
        }

        const auto text = _searchBox->Text();
        if (text.empty())
        {
            return;
        }

        const auto goForward = _searchBox->GoForward();
        const auto caseSensitive = _searchBox->CaseSensitive();
        const auto regularExpression = _searchBox->RegularExpression();
        _handleSearchResults(_core.Search(SearchRequest{
            .Text = text,
            .GoForward = goForward,
            .CaseSensitive = caseSensitive,
            .RegularExpression = regularExpression,
            .ExecuteSearch = false,
            .ScrollIntoView = false,
            .ScrollOffset = _searchScrollOffset,
        }));
    }

    void TermControl::_handleSearchResults(SearchResults results)
    {
        if (!_searchBox)
        {
            return;
        }

        // Only show status when we have a search term
        if (_searchBox->Text().empty())
        {
            _searchBox->ClearStatus();
        }
        else
        {
            _searchBox->SetStatus(results.TotalMatches, results.CurrentMatch, results.SearchRegexInvalid);
        }

        if (results.SearchInvalidated)
        {
            if (_showMarksInScrollbar)
            {
                const auto scrollBar = ScrollBar();
                ScrollBarUpdate update{
                    .newValue = scrollBar.Value(),
                    .newMaximum = scrollBar.Maximum(),
                    .newMinimum = scrollBar.Minimum(),
                    .newViewportSize = scrollBar.ViewportSize(),
                };
                _updateScrollBar->Run(update);
            }
        }

        if (auto automationPeer{ FrameworkElementAutomationPeer::FromElement(*this) })
        {
            const auto status = _searchBox->GetAccessibleStatus(results.TotalMatches, results.CurrentMatch, results.SearchRegexInvalid);
            if (!status.empty())
            {
                automationPeer.RaiseNotificationEvent(
                    AutomationNotificationKind::ActionCompleted,
                    AutomationNotificationProcessing::ImportantMostRecent,
                    status,
                    L"SearchBoxResultAnnouncement" /* unique name for this group of notifications */);
            }
        }
    }

    void TermControl::_coreOutputIdle(const IInspectable& /*sender*/, const IInspectable& /*args*/)
    {
        _refreshSearch();
    }

    void TermControl::OwningHwnd(uint64_t owner)
    {
        _core.OwningHwnd(owner);
    }

    uint64_t TermControl::OwningHwnd()
    {
        return _core.OwningHwnd();
    }

    void TermControl::PreviewInput(const winrt::hstring& text)
    {
        get_self<ControlCore>(_core)->PreviewInput(text);

        if (!text.empty())
        {
            if (auto automationPeer{ FrameworkElementAutomationPeer::FromElement(*this) })
            {
                automationPeer.RaiseNotificationEvent(
                    AutomationNotificationKind::ItemAdded,
                    AutomationNotificationProcessing::All,
                    RS_fmt(L"PreviewTextAnnouncement", text),
                    L"PreviewTextAnnouncement" /* unique name for this group of notifications */);
            }
        }
    }

    void TermControl::AddMark(const Control::ScrollMark& mark)
    {
        _core.AddMark(mark);
    }
    void TermControl::ClearMark() { _core.ClearMark(); }
    void TermControl::ClearAllMarks() { _core.ClearAllMarks(); }
    void TermControl::ScrollToMark(const Control::ScrollToMarkDirection& direction) { _core.ScrollToMark(direction); }

    Windows::Foundation::Collections::IVector<Control::ScrollMark> TermControl::ScrollMarks() const
    {
        return _core.ScrollMarks();
    }

    void TermControl::SelectCommand(const bool goUp)
    {
        _core.SelectCommand(goUp);
    }

    void TermControl::SelectOutput(const bool goUp)
    {
        _core.SelectOutput(goUp);
    }

    void TermControl::ColorSelection(Control::SelectionColor fg, Control::SelectionColor bg, Core::MatchMode matchMode)
    {
        _core.ColorSelection(fg, bg, matchMode);
    }

    // Returns the text cursor's position relative to our origin, in DIPs.
    Windows::Foundation::Point TermControl::CursorPositionInDips()
    {
        const auto cursorPos{ _core.CursorPosition() };

        // CharacterDimensions returns a font size in pixels.
        const auto fontSize{ CharacterDimensions() };

        // Account for the margins, which are in DIPs
        auto padding{ GetPadding() };

        // Convert text buffer cursor position to client coordinate position
        // within the window. This point is in _pixels_
        return {
            cursorPos.X * fontSize.Width + static_cast<float>(padding.Left),
            cursorPos.Y * fontSize.Height + static_cast<float>(padding.Top),
        };
    }

    void TermControl::_contextMenuHandler(IInspectable /*sender*/,
                                          Control::ContextMenuRequestedEventArgs args)
    {
        const auto inverseScale = 1.0f / static_cast<float>(XamlRoot().RasterizationScale());
        const auto padding = GetPadding();
        const auto pos = args.Position();
        _showContextMenuAt({
            pos.X * inverseScale + static_cast<float>(padding.Left),
            pos.Y * inverseScale + static_cast<float>(padding.Top),
        });
    }

    void TermControl::_showContextMenuAt(const winrt::Windows::Foundation::Point& controlRelativePos)
    {
        Controls::Primitives::FlyoutShowOptions myOption{};
        myOption.ShowMode(Controls::Primitives::FlyoutShowMode::Standard);
        myOption.Placement(Controls::Primitives::FlyoutPlacementMode::TopEdgeAlignedLeft);
        myOption.Position(controlRelativePos);

        // The "Select command" and "Select output" buttons should only be
        // visible if shell integration is actually turned on.
        const auto shouldShowSelectCommand{ _core.ShouldShowSelectCommand() };
        const auto shouldShowSelectOutput{ _core.ShouldShowSelectOutput() };
        SelectCommandButton().Visibility(shouldShowSelectCommand ? Visibility::Visible : Visibility::Collapsed);
        SelectOutputButton().Visibility(shouldShowSelectOutput ? Visibility::Visible : Visibility::Collapsed);
        SelectCommandWithSelectionButton().Visibility(shouldShowSelectCommand ? Visibility::Visible : Visibility::Collapsed);
        SelectOutputWithSelectionButton().Visibility(shouldShowSelectOutput ? Visibility::Visible : Visibility::Collapsed);

        (_core.HasSelection() ? SelectionContextMenu() :
                                ContextMenu())
            .ShowAt(*this, myOption);
    }

    void TermControl::ShowContextMenu()
    {
        const bool hasSelection = _core.HasSelection();
        til::point cursorPos{
            hasSelection ? _core.SelectionInfo().EndPos :
                           _core.CursorPosition()
        };
        // Offset this position a bit:
        // * {+0,+1} if there's a selection. The selection endpoint is already
        //   exclusive, so add one row to align to the bottom of the selection
        // * {+1,+1} if there's no selection, to be on the bottom-right corner of
        //   the cursor position
        cursorPos += til::point{ hasSelection ? 0 : 1, 1 };
        _showContextMenuAt(_toControlOrigin(cursorPos));
    }

    double TermControl::QuickFixButtonWidth()
    {
        const auto leftPadding = GetPadding().Left;
        if (_quickFixButtonCollapsible)
        {
            const auto cellWidth = CharacterDimensions().Width;
            if (leftPadding == 0)
            {
                return cellWidth;
            }
            return leftPadding + (cellWidth / 2.0);
        }
        return leftPadding;
    }

    double TermControl::QuickFixButtonCollapsedWidth()
    {
        return std::max(CharacterDimensions().Width * 2.0 / 3.0, GetPadding().Left);
    }

    bool TermControl::OpenQuickFixMenu()
    {
        if constexpr (Feature_QuickFix::IsEnabled())
        {
            if (_core.QuickFixesAvailable())
            {
                // Expand the quick fix button if it's collapsed (looks nicer)
                if (_quickFixButtonCollapsible)
                {
                    VisualStateManager::GoToState(*this, StateNormal, false);
                }
                auto quickFixBtn = QuickFixButton();
                quickFixBtn.Flyout().ShowAt(quickFixBtn);
                return true;
            }
        }
        return false;
    }

    void TermControl::RefreshQuickFixMenu()
    {
        if (!Feature_QuickFix::IsEnabled())
        {
            return;
        }

        auto quickFixBtn = QuickFixButton();
        if (!_core.QuickFixesAvailable())
        {
            quickFixBtn.Visibility(Visibility::Collapsed);
            return;
        }

        // If the gutter is narrow, display the collapsed version
        const auto& termPadding = GetPadding();

        // Make sure to update _quickFixButtonCollapsible and QuickFix button widths BEFORE updating the VisualState
        _quickFixButtonCollapsible = termPadding.Left < CharacterDimensions().Width;
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"QuickFixButtonWidth" });
        PropertyChanged.raise(*this, Windows::UI::Xaml::Data::PropertyChangedEventArgs{ L"QuickFixButtonCollapsedWidth" });
        VisualStateManager::GoToState(*this, !_quickFixButtonCollapsible ? StateNormal : StateCollapsed, false);

        const auto rd = get_self<ControlCore>(_core)->GetRenderData();
        rd->LockConsole();
        const auto viewportBufferPosition = rd->GetViewport();
        rd->UnlockConsole();
        if (_quickFixBufferPos < viewportBufferPosition.Top() || _quickFixBufferPos > viewportBufferPosition.BottomInclusive())
        {
            quickFixBtn.Visibility(Visibility::Collapsed);
            return;
        }

        // draw the button in the gutter
        const auto& quickFixBtnPosInDips = _toPosInDips({ 0, _quickFixBufferPos });
        Controls::Canvas::SetLeft(quickFixBtn, -termPadding.Left);
        Controls::Canvas::SetTop(quickFixBtn, quickFixBtnPosInDips.Y - termPadding.Top);
        quickFixBtn.Visibility(Visibility::Visible);

        if (auto automationPeer{ FrameworkElementAutomationPeer::FromElement(*this) })
        {
            automationPeer.RaiseNotificationEvent(
                AutomationNotificationKind::ItemAdded,
                AutomationNotificationProcessing::ImportantMostRecent,
                RS_(L"QuickFixAvailable"),
                L"QuickFixAvailableAnnouncement" /* unique name for this group of notifications */);
        }
    }

    void TermControl::_bubbleSearchMissingCommand(const IInspectable& /*sender*/, const Control::SearchMissingCommandEventArgs& args)
    {
        _quickFixBufferPos = args.BufferRow();
        SearchMissingCommand.raise(*this, args);
    }

    winrt::fire_and_forget TermControl::_bubbleWindowSizeChanged(const IInspectable& /*sender*/, Control::WindowSizeChangedEventArgs args)
    {
        auto weakThis{ get_weak() };
        co_await wil::resume_foreground(Dispatcher());

        if (auto control{ weakThis.get() })
        {
            winrt::Windows::Foundation::Size cellCount{ static_cast<float>(args.Width()), static_cast<float>(args.Height()) };
            const auto pixelSize = GetNewDimensions(cellCount);

            WindowSizeChanged.raise(*this, winrt::make<implementation::WindowSizeChangedEventArgs>(static_cast<int32_t>(pixelSize.Width), static_cast<int32_t>(pixelSize.Height)));
        }
    }

    til::CoordType TermControl::_calculateSearchScrollOffset() const
    {
        auto result = 0;
        if (_searchBox)
        {
            const auto displayInfo = DisplayInformation::GetForCurrentView();
            const auto scaleFactor = _core.FontSize().Height / displayInfo.RawPixelsPerViewPixel();
            const auto searchBoxRows = _searchBox->ActualHeight() / scaleFactor;
            result = static_cast<int32_t>(std::ceil(searchBoxRows));
        }
        return result;
    }

    void TermControl::ClearQuickFix()
    {
        _core.ClearQuickFix();
    }

    void TermControl::_PasteCommandHandler(const IInspectable& /*sender*/,
                                           const IInspectable& /*args*/)
    {
        _interactivity.RequestPasteTextFromClipboard();
        ContextMenu().Hide();
        SelectionContextMenu().Hide();
    }
    void TermControl::_CopyCommandHandler(const IInspectable& /*sender*/,
                                          const IInspectable& /*args*/)
    {
        // formats = nullptr -> copy all formats
        _interactivity.CopySelectionToClipboard(false, false, _core.Settings().CopyFormatting());
        ContextMenu().Hide();
        SelectionContextMenu().Hide();
    }
    void TermControl::_SearchCommandHandler(const IInspectable& /*sender*/,
                                            const IInspectable& /*args*/)
    {
        ContextMenu().Hide();
        SelectionContextMenu().Hide();

        // CreateSearchBoxControl will actually create the search box and
        // prepopulate the box with the currently selected text.
        CreateSearchBoxControl();
    }

    void TermControl::_SelectCommandHandler(const IInspectable& /*sender*/,
                                            const IInspectable& /*args*/)
    {
        ContextMenu().Hide();
        SelectionContextMenu().Hide();
        _core.ContextMenuSelectCommand();
    }

    void TermControl::_SelectOutputHandler(const IInspectable& /*sender*/,
                                           const IInspectable& /*args*/)
    {
        ContextMenu().Hide();
        SelectionContextMenu().Hide();
        _core.ContextMenuSelectOutput();
    }

    Control::CursorDisplayState TermControl::CursorVisibility() const noexcept
    {
        return _cursorVisibility;
    }

    void TermControl::CursorVisibility(Control::CursorDisplayState cursorVisibility)
    {
        _cursorVisibility = cursorVisibility;

        // NOTE: This code is specific to broadcast input. It's never been well integrated.
        // Ideally TermControl should not tie focus to XAML in the first place,
        // allowing us to truly say "yeah these two controls both have focus".
        if (_core)
        {
            _core.ForceCursorVisible(cursorVisibility == CursorDisplayState::Shown);
        }
    }
}
