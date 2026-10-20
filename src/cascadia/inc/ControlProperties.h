// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

// --------------------------- Core Appearance ---------------------------
//  All of these settings are defined in ICoreAppearance.
#define CORE_APPEARANCE_SETTINGS(X)                                                                                       \
    X(til::color, DefaultForeground, DEFAULT_FOREGROUND)                                                                  \
    X(til::color, DefaultBackground, DEFAULT_BACKGROUND)                                                                  \
    X(til::color, CursorColor, DEFAULT_CURSOR_COLOR)                                                                      \
    X(winrt::Microsoft::Terminal::Core::CursorStyle, CursorShape, winrt::Microsoft::Terminal::Core::CursorStyle::Vintage) \
    X(uint32_t, CursorHeight, DEFAULT_CURSOR_HEIGHT)                                                                      \
    X(til::color, SelectionBackground, DEFAULT_FOREGROUND)                                                                \
    X(bool, IntenseIsBold)                                                                                                \
    X(bool, IntenseIsBright, true)                                                                                        \
    X(winrt::Microsoft::Terminal::Core::AdjustTextMode, AdjustIndistinguishableColors, winrt::Microsoft::Terminal::Core::AdjustTextMode::Automatic)

// --------------------------- Control Appearance ---------------------------
//  All of these settings are defined in IControlAppearance.
#define CONTROL_APPEARANCE_SETTINGS(X)                                                                                                          \
    X(float, Opacity, 1.0f)                                                                                                                     \
    X(bool, UseAcrylic, false)                                                                                                                  \
    X(winrt::hstring, BackgroundImage)                                                                                                          \
    X(float, BackgroundImageOpacity, 1.0f)                                                                                                      \
    X(winrt::Windows::UI::Xaml::Media::Stretch, BackgroundImageStretchMode, winrt::Windows::UI::Xaml::Media::Stretch::UniformToFill)            \
    X(winrt::Windows::UI::Xaml::HorizontalAlignment, BackgroundImageHorizontalAlignment, winrt::Windows::UI::Xaml::HorizontalAlignment::Center) \
    X(winrt::Windows::UI::Xaml::VerticalAlignment, BackgroundImageVerticalAlignment, winrt::Windows::UI::Xaml::VerticalAlignment::Center)       \
    X(bool, RetroTerminalEffect, false)                                                                                                         \
    X(winrt::hstring, PixelShaderPath)                                                                                                          \
    X(winrt::hstring, PixelShaderImagePath)

// --------------------------- Core Settings ---------------------------
//  All of these settings are defined in ICoreSettings.
#define CORE_SETTINGS(X)                                                                                          \
    X(int32_t, HistorySize, DEFAULT_HISTORY_SIZE)                                                                 \
    X(int32_t, InitialRows, 30)                                                                                   \
    X(int32_t, InitialCols, 80)                                                                                   \
    X(bool, SnapOnInput, true)                                                                                    \
    X(bool, AltGrAliasing, true)                                                                                  \
    X(winrt::hstring, AnswerbackMessage)                                                                          \
    X(winrt::hstring, WordDelimiters, DEFAULT_WORD_DELIMITERS)                                                    \
    X(bool, CopyOnSelect, false)                                                                                  \
    X(bool, FocusFollowMouse, false)                                                                              \
    X(bool, ScrollToZoom, true)                                                                                   \
    X(bool, HyperlinkClickable, true)                                                                             \
    X(winrt::Windows::Foundation::Collections::IVector<winrt::hstring>, HyperlinkClickableKinds, nullptr)         \
    X(winrt::hstring, HyperlinkPrimaryAction)                                                                     \
    X(winrt::hstring, HyperlinkAlternativeAction)                                                                 \
    X(int32_t, HyperlinkTooltipMaxWidth, 640)                                                                     \
    X(int32_t, HyperlinkTooltipMaxHeight, 720)                                                                     \
    X(bool, HyperlinkTooltipNested, false)                                                                     \
    X(int32_t, HyperlinkTooltipShowDelay, 250)                                                                    \
    X(int32_t, HyperlinkTooltipHideDelay, 400)                                                                    \
    X(bool, HyperlinkTooltipActions, true)                                                                        \
    X(winrt::Windows::Foundation::Collections::IVector<winrt::hstring>, HyperlinkTooltipButtons, nullptr)         \
    X(bool, HyperlinkTooltipHint, true)                                                                           \
    X(bool, HyperlinkTooltipShowRule, false)                                                                      \
    X(bool, HyperlinkPreviewInPane, false)                                                                        \
    X(bool, HyperlinkRemoteFilePreviews, false)                                                                   \
    X(bool, ScrollToChangeOpacity, true)                                                                          \
    X(winrt::Windows::Foundation::IReference<winrt::Microsoft::Terminal::Core::Color>, TabColor, nullptr)         \
    X(winrt::Windows::Foundation::IReference<winrt::Microsoft::Terminal::Core::Color>, StartingTabColor, nullptr) \
    X(bool, TrimBlockSelection, true)                                                                             \
    X(bool, SuppressApplicationTitle)                                                                             \
    X(bool, ForceVTInput, false)                                                                                  \
    X(bool, AllowKittyKeyboardMode, true)                                                                         \
    X(winrt::hstring, StartingTitle)                                                                              \
    X(bool, DetectURLs, true)                                                                                     \
    X(winrt::Windows::Foundation::Collections::IVector<winrt::hstring>, TextPatterns, nullptr)                                                                                     \
    X(bool, AutoMarkPrompts)                                                                                      \
    X(bool, RepositionCursorWithMouse, false)                                                                     \
    X(bool, RainbowSuggestions)                                                                                   \
    X(bool, AllowVtChecksumReport)                                                                                \
    X(bool, AllowVtClipboardWrite, true)                                                                          \
    X(bool, AllowOscNotifications, false)

// --------------------------- Control Settings ---------------------------
//  All of these settings are defined in IControlSettings.
#define CONTROL_SETTINGS(X)                                                                                                                              \
    X(winrt::guid, SessionId)                                                                                                                            \
    X(bool, EnableUnfocusedAcrylic, false)                                                                                                               \
    X(winrt::hstring, Padding, DEFAULT_PADDING)                                                                                                          \
    X(winrt::hstring, FontFace, L"Consolas")                                                                                                             \
    X(float, FontSize, DEFAULT_FONT_SIZE)                                                                                                                \
    X(winrt::Windows::UI::Text::FontWeight, FontWeight)                                                                                                  \
    X(IFontFeatureMap, FontFeatures)                                                                                                                     \
    X(IFontAxesMap, FontAxes)                                                                                                                            \
    X(bool, EnableBuiltinGlyphs, true)                                                                                                                   \
    X(bool, EnableColorGlyphs, true)                                                                                                                     \
    X(winrt::hstring, CellWidth)                                                                                                                         \
    X(winrt::hstring, CellHeight)                                                                                                                        \
    X(winrt::hstring, Commandline)                                                                                                                       \
    X(winrt::hstring, StartingDirectory)                                                                                                                 \
    X(winrt::Microsoft::Terminal::Control::ScrollbarState, ScrollState, winrt::Microsoft::Terminal::Control::ScrollbarState::Visible)                    \
    X(winrt::Microsoft::Terminal::Control::TextAntialiasingMode, AntialiasingMode, winrt::Microsoft::Terminal::Control::TextAntialiasingMode::Grayscale) \
    X(winrt::Microsoft::Terminal::Control::GraphicsAPI, GraphicsAPI)                                                                                     \
    X(bool, DisablePartialInvalidation, false)                                                                                                           \
    X(bool, SoftwareRendering, false)                                                                                                                    \
    X(winrt::Microsoft::Terminal::Control::TextMeasurement, TextMeasurement)                                                                             \
    X(winrt::Microsoft::Terminal::Control::AmbiguousWidth, AmbiguousWidth, winrt::Microsoft::Terminal::Control::AmbiguousWidth::Narrow)                  \
    X(winrt::Microsoft::Terminal::Control::DefaultInputScope, DefaultInputScope, winrt::Microsoft::Terminal::Control::DefaultInputScope::Default)        \
    X(bool, UseBackgroundImageForWindow, false)                                                                                                          \
    X(bool, ShowMarks, false)                                                                                                                            \
    X(winrt::Microsoft::Terminal::Control::CopyFormat, CopyFormatting, 0)                                                                                \
    X(bool, RightClickContextMenu, false)                                                                                                                \
    X(winrt::Microsoft::Terminal::Control::PathTranslationStyle, PathTranslationStyle, winrt::Microsoft::Terminal::Control::PathTranslationStyle::None)  \
    X(winrt::Microsoft::Terminal::Control::HyperlinkClickModifier, HyperlinkPrimaryClickModifier, winrt::Microsoft::Terminal::Control::HyperlinkClickModifier::None)                      \
    X(winrt::Microsoft::Terminal::Control::HyperlinkClickGesture, HyperlinkPrimaryClickGesture, winrt::Microsoft::Terminal::Control::HyperlinkClickGesture::LeftClick)                    \
    X(winrt::Microsoft::Terminal::Control::HyperlinkClickModifier, HyperlinkAlternativeClickModifier, winrt::Microsoft::Terminal::Control::HyperlinkClickModifier::Ctrl)                  \
    X(winrt::Microsoft::Terminal::Control::HyperlinkClickGesture, HyperlinkAlternativeClickGesture, winrt::Microsoft::Terminal::Control::HyperlinkClickGesture::LeftClick)                \
    X(winrt::Microsoft::Terminal::Control::HyperlinkIntegrationDisplayMode, HyperlinkIntegrationDisplayMode, winrt::Microsoft::Terminal::Control::HyperlinkIntegrationDisplayMode::Above) \
    X(winrt::Microsoft::Terminal::Control::HyperlinkActionPlacement, HyperlinkActionPlacement, winrt::Microsoft::Terminal::Control::HyperlinkActionPlacement::FarFromLink) \
    X(winrt::hstring, DragDropDelimiter, L" ")
